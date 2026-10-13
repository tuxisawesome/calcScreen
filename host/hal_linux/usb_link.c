/* usb_link.c - see usb_link.h. */
#define _GNU_SOURCE
#include "usb_link.h"
#include "dock_common.h"
#include "dusb.h"
#include "util.h"

#include <libusb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define IN_XFERS 4
#define IN_SIZE 16384
#define OUT_XFERS_MAX 16
#define OUT_CHUNK_MAX 65536u
#define RESCAN_MS 1000u       /* polling interval without hotplug support */
#define RETRY_MIN_MS 1000u
#define RETRY_MAX_MS 8000u
#define DUSB_RX_BUF 4096u     /* multiple of the max packet size */

typedef struct fast {
    libusb_device_handle *h;
    int iface;
    uint8_t ep_in, ep_out;
    struct libusb_transfer *in[IN_XFERS];
    struct libusb_transfer *out[OUT_XFERS_MAX];
    int n_out;
    int inflight;             /* submitted transfers not completed yet */
    bool up;
    bool closing;             /* cancelling; finish when inflight == 0 */
    bool notify_up, notify_down;
    uint8_t *stage;           /* queued tx bytes not yet submitted */
    size_t stage_len, stage_cap;
    size_t out_bytes;         /* bytes in submitted OUT transfers */
} fast_t;

typedef struct dusb_dev {
    libusb_device_handle *h;
    int iface;
    uint8_t ep_in, ep_out;
    bool up;
    bool present;
    uint8_t rx[DUSB_RX_BUF];
    size_t rx_off, rx_len;
} dusb_dev_t;

struct usb_link {
    libusb_context *ctx;
    usb_link_cb_t cb;
    bool hotplug;
    libusb_hotplug_callback_handle hp[2];
    bool rescan;              /* look for devices at the next handle() */
    uint64_t next_scan_ms;    /* periodic / backoff rescan */
    uint32_t backoff_ms;
    bool fast_reset;          /* reset the next FAST device we open (see open_fast) */
    bool paused;
    bool dusb_allowed;
    fast_t fast;
    dusb_dev_t dusb;
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void schedule_retry(usb_link_t *u, uint64_t now)
{
    u->next_scan_ms = now + u->backoff_ms;
    u->backoff_ms = u->backoff_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : u->backoff_ms * 2;
}

/* Find the interface with class/subclass/protocol (or any if cls < 0) and
 * its first bulk IN and OUT endpoints. */
static bool find_iface(libusb_device *dev, int cls, int sub, int proto, int *iface,
                       uint8_t *ep_in, uint8_t *ep_out)
{
    struct libusb_config_descriptor *cfg;
    bool found = false;
    int i, a, e;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 &&
        libusb_get_config_descriptor(dev, 0, &cfg) != 0)
        return false;
    for (i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (a = 0; a < itf->num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor *d = &itf->altsetting[a];
            uint8_t in = 0, out = 0;
            if (cls >= 0 && (d->bInterfaceClass != cls || d->bInterfaceSubClass != sub ||
                             d->bInterfaceProtocol != proto))
                continue;
            for (e = 0; e < d->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &d->endpoint[e];
                if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) && !in)
                    in = ep->bEndpointAddress;
                else if (!(ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) && !out)
                    out = ep->bEndpointAddress;
            }
            if (in && out) {
                *iface = d->bInterfaceNumber;
                *ep_in = in;
                *ep_out = out;
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

static libusb_device_handle *open_claim(libusb_device *dev, int iface)
{
    libusb_device_handle *h = NULL;
    int rc = libusb_open(dev, &h);
    if (rc != 0) {
        logv("usb: open: %s", libusb_error_name(rc));
        return NULL;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    rc = libusb_claim_interface(h, iface);
    if (rc != 0) {
        logv("usb: claim interface %d: %s", iface, libusb_error_name(rc));
        libusb_close(h);
        return NULL;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* FAST                                                                */

static void fast_fail(usb_link_t *u, const char *why);

static void LIBUSB_CALL in_cb(struct libusb_transfer *t)
{
    usb_link_t *u = t->user_data;
    u->fast.inflight--;
    if (u->fast.closing)
        return;
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        if (t->actual_length > 0)
            u->cb.fast_rx(u->cb.user, t->buffer, (size_t)t->actual_length);
        if (u->fast.closing)
            return; /* the handler dropped the link */
        if (libusb_submit_transfer(t) == 0) {
            u->fast.inflight++;
            return;
        }
        fast_fail(u, "resubmit");
        return;
    }
    fast_fail(u, t->status == LIBUSB_TRANSFER_NO_DEVICE ? "unplugged" : "IN error");
}

static void LIBUSB_CALL out_cb(struct libusb_transfer *t)
{
    usb_link_t *u = t->user_data;
    fast_t *f = &u->fast;
    int i;
    f->inflight--;
    for (i = 0; i < f->n_out; i++) {
        if (f->out[i] == t) {
            memmove(f->out + i, f->out + i + 1, (size_t)(f->n_out - i - 1) * sizeof f->out[0]);
            f->n_out--;
            break;
        }
    }
    f->out_bytes -= (size_t)t->length;
    if (!f->closing && t->status != LIBUSB_TRANSFER_COMPLETED)
        fast_fail(u, t->status == LIBUSB_TRANSFER_NO_DEVICE ? "unplugged" : "OUT error");
    /* transfer and buffer are freed by libusb (FREE_* flags) */
}

/* Begin tearing the FAST link down; completes in fast_finish_close(). */
static void fast_fail(usb_link_t *u, const char *why)
{
    fast_t *f = &u->fast;
    int i;
    if (f->closing || !f->h)
        return;
    logv("usb: fast link down (%s)", why);
    f->closing = true;
    if (f->up)
        f->notify_down = true;
    f->up = false;
    f->notify_up = false;
    for (i = 0; i < IN_XFERS; i++)
        if (f->in[i])
            libusb_cancel_transfer(f->in[i]);
    for (i = 0; i < f->n_out; i++)
        libusb_cancel_transfer(f->out[i]);
    u->fast_reset = true; /* the calculator may still think it is connected */
}

static void fast_finish_close(usb_link_t *u)
{
    fast_t *f = &u->fast;
    int i;
    if (!f->closing || f->inflight > 0)
        return;
    for (i = 0; i < IN_XFERS; i++) {
        if (f->in[i]) {
            free(f->in[i]->buffer);
            libusb_free_transfer(f->in[i]);
            f->in[i] = NULL;
        }
    }
    libusb_release_interface(f->h, f->iface);
    libusb_close(f->h);
    f->h = NULL;
    f->closing = false;
    f->n_out = 0;
    f->out_bytes = 0;
    f->stage_len = 0;
    u->rescan = true;
    schedule_retry(u, mono_ms());
}

static bool fast_start_reads(usb_link_t *u)
{
    fast_t *f = &u->fast;
    int i;
    for (i = 0; i < IN_XFERS; i++) {
        uint8_t *buf = malloc(IN_SIZE);
        f->in[i] = libusb_alloc_transfer(0);
        if (!buf || !f->in[i]) {
            free(buf);
            return false;
        }
        libusb_fill_bulk_transfer(f->in[i], f->h, f->ep_in, buf, IN_SIZE, in_cb, u, 0);
        if (libusb_submit_transfer(f->in[i]) != 0)
            return false;
        f->inflight++;
    }
    return true;
}

static bool open_fast(usb_link_t *u, libusb_device *dev)
{
    fast_t *f = &u->fast;
    if (!find_iface(dev, DOCK_USB_IFACE_CLASS, DOCK_USB_IFACE_SUBCLASS,
                    DOCK_USB_IFACE_PROTOCOL, &f->iface, &f->ep_in, &f->ep_out)) {
        logv("usb: %04x:%04x has no dock interface", DOCK_USB_VID, DOCK_USB_PID);
        return false;
    }
    f->h = open_claim(dev, f->iface);
    if (!f->h)
        return false;
    /* A calculator that was already configured before we opened it (dockd
     * restarted, came back from the shell, recovered from an error) still
     * believes the session is up and would never send HELLO again. A bus
     * reset makes its driver see a fresh connection. */
    if (u->fast_reset) {
        int rc = libusb_reset_device(f->h);
        if (rc != 0) {
            logv("usb: reset: %s (waiting for re-enumeration)", libusb_error_name(rc));
            libusb_close(f->h);
            f->h = NULL;
            u->fast_reset = false;
            return false;
        }
        u->fast_reset = false;
    }
    f->closing = false;
    f->stage_len = 0;
    if (!fast_start_reads(u)) {
        fast_fail(u, "cannot queue reads");
        return false;
    }
    f->up = true;
    f->notify_up = true;
    logv("usb: fast link up (if %d, in %02x, out %02x)", f->iface, f->ep_in, f->ep_out);
    return true;
}

bool usb_link_fast_up(const usb_link_t *u)
{
    return u->fast.up;
}

bool usb_link_tx(usb_link_t *u, const void *p, size_t n)
{
    fast_t *f = &u->fast;
    if (!f->up)
        return false;
    if (f->stage_len + f->out_bytes + n > USB_LINK_TX_MAX) {
        fast_fail(u, "transmit queue overflow");
        return false;
    }
    if (f->stage_len + n > f->stage_cap) {
        size_t cap = f->stage_cap ? f->stage_cap : 16384;
        uint8_t *nb;
        while (cap < f->stage_len + n)
            cap *= 2;
        nb = realloc(f->stage, cap);
        if (!nb) {
            fast_fail(u, "out of memory");
            return false;
        }
        f->stage = nb;
        f->stage_cap = cap;
    }
    memcpy(f->stage + f->stage_len, p, n);
    f->stage_len += n;
    return true;
}

void usb_link_flush(usb_link_t *u)
{
    fast_t *f = &u->fast;
    while (f->up && f->stage_len > 0 && f->n_out < OUT_XFERS_MAX) {
        size_t n = f->stage_len < OUT_CHUNK_MAX ? f->stage_len : OUT_CHUNK_MAX;
        struct libusb_transfer *t = libusb_alloc_transfer(0);
        uint8_t *buf = malloc(n);
        if (!t || !buf) {
            libusb_free_transfer(t);
            free(buf);
            fast_fail(u, "out of memory");
            return;
        }
        memcpy(buf, f->stage, n);
        memmove(f->stage, f->stage + n, f->stage_len - n);
        f->stage_len -= n;
        /* No timeout: a resident driver may not service USB while TI-OS is
         * busy; the queue limit catches a calculator that is truly gone. */
        libusb_fill_bulk_transfer(t, f->h, f->ep_out, buf, (int)n, out_cb, u, 0);
        t->flags = LIBUSB_TRANSFER_ADD_ZERO_PACKET | LIBUSB_TRANSFER_FREE_BUFFER |
                   LIBUSB_TRANSFER_FREE_TRANSFER;
        if (libusb_submit_transfer(t) != 0) {
            libusb_free_transfer(t); /* frees buf too (FREE_BUFFER) */
            fast_fail(u, "OUT submit");
            return;
        }
        f->out[f->n_out++] = t;
        f->out_bytes += n;
        f->inflight++;
    }
}

/* ------------------------------------------------------------------ */
/* DUSB                                                                */

static void dusb_close(usb_link_t *u)
{
    dusb_dev_t *d = &u->dusb;
    if (!d->h)
        return;
    libusb_release_interface(d->h, d->iface);
    libusb_close(d->h);
    d->h = NULL;
    d->rx_off = d->rx_len = 0;
    if (d->up) {
        d->up = false;
        if (u->cb.dusb_down)
            u->cb.dusb_down(u->cb.user);
    }
}

static bool open_dusb(usb_link_t *u, libusb_device *dev)
{
    dusb_dev_t *d = &u->dusb;
    /* TI uses 0x81 IN / 0x02 OUT on the CE, but trust the descriptor. */
    if (!find_iface(dev, -1, 0, 0, &d->iface, &d->ep_in, &d->ep_out))
        return false;
    d->h = open_claim(dev, d->iface);
    if (!d->h)
        return false;
    d->rx_off = d->rx_len = 0;
    d->up = true;
    logv("usb: DUSB calculator (if %d, in %02x, out %02x)", d->iface, d->ep_in, d->ep_out);
    if (u->cb.dusb_up)
        u->cb.dusb_up(u->cb.user);
    return true;
}

bool usb_link_dusb_up(const usb_link_t *u)
{
    return u->dusb.up;
}

bool usb_link_dusb_present(const usb_link_t *u)
{
    return u->dusb.present;
}

void usb_link_set_dusb_allowed(usb_link_t *u, bool allowed)
{
    if (u->dusb_allowed == allowed)
        return;
    u->dusb_allowed = allowed;
    if (!allowed)
        dusb_close(u);
    u->rescan = true;
}

void usb_link_dusb_fail(usb_link_t *u)
{
    logv("usb: DUSB session failed");
    dusb_close(u);
    u->rescan = true;
    schedule_retry(u, mono_ms());
}

bool usb_link_dusb_send(usb_link_t *u, const uint8_t *p, size_t n)
{
    dusb_dev_t *d = &u->dusb;
    size_t off = 0;
    while (d->h && off < n) {
        int done = 0;
        int rc = libusb_bulk_transfer(d->h, d->ep_out, (unsigned char *)p + off,
                                      (int)(n - off), &done, 2000);
        if (rc != 0 && !(rc == LIBUSB_ERROR_TIMEOUT && done > 0)) {
            logv("usb: DUSB send: %s", libusb_error_name(rc));
            return false;
        }
        off += (size_t)done;
    }
    return d->h && off == n;
}

/* Read exactly n bytes from the DUSB byte stream before `deadline`. */
static bool dusb_read_exact(usb_link_t *u, uint8_t *p, size_t n, uint64_t deadline)
{
    dusb_dev_t *d = &u->dusb;
    while (n) {
        uint64_t now;
        int got = 0, rc;
        if (d->rx_len) {
            size_t take = d->rx_len < n ? d->rx_len : n;
            memcpy(p, d->rx + d->rx_off, take);
            d->rx_off += take;
            d->rx_len -= take;
            p += take;
            n -= take;
            continue;
        }
        now = mono_ms();
        if (!d->h || now >= deadline)
            return false;
        rc = libusb_bulk_transfer(d->h, d->ep_in, d->rx, (int)sizeof d->rx, &got,
                                  (unsigned)(deadline - now));
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            logv("usb: DUSB recv: %s", libusb_error_name(rc));
            return false;
        }
        d->rx_off = 0;
        d->rx_len = got > 0 ? (size_t)got : 0;
        if (rc == LIBUSB_ERROR_TIMEOUT && got == 0)
            return false;
    }
    return true;
}

/* One raw DUSB packet: u32 big-endian payload size, u8 type, payload. The
 * link is treated as a byte stream so packets that span or share USB
 * transfers are handled either way. */
size_t usb_link_dusb_recv(usb_link_t *u, uint8_t *p, size_t cap, uint32_t timeout_ms)
{
    uint64_t deadline = mono_ms() + timeout_ms;
    uint32_t size;
    if (cap < 5 || !dusb_read_exact(u, p, 5, deadline))
        return 0;
    size = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (size > cap - 5) {
        logv("usb: DUSB packet too big (%u)", size);
        u->dusb.rx_len = 0; /* lost sync: drop what is buffered */
        return 0;
    }
    if (!dusb_read_exact(u, p + 5, size, deadline))
        return 0;
    return 5u + size;
}

/* ------------------------------------------------------------------ */
/* discovery                                                           */

static int LIBUSB_CALL hotplug_cb(libusb_context *ctx, libusb_device *dev,
                                  libusb_hotplug_event ev, void *user)
{
    usb_link_t *u = user;
    struct libusb_device_descriptor dd;
    (void)ctx;
    u->rescan = true;
    u->next_scan_ms = 0;
    u->backoff_ms = RETRY_MIN_MS;
    /* A freshly enumerated DOCK device will say HELLO by itself. */
    if (ev == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED &&
        libusb_get_device_descriptor(dev, &dd) == 0 && dd.idVendor == DOCK_USB_VID &&
        dd.idProduct == DOCK_USB_PID)
        u->fast_reset = false;
    return 0;
}

static void scan_devices(usb_link_t *u, uint64_t now)
{
    libusb_device **list;
    ssize_t n, i;
    bool dusb_seen = false, ok = true;

    u->rescan = false;
    n = libusb_get_device_list(u->ctx, &list);
    if (n < 0)
        return;
    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;
        if (dd.idVendor == DOCK_USB_VID && dd.idProduct == DOCK_USB_PID) {
            if (!u->fast.h && !u->paused && !open_fast(u, list[i]))
                ok = false;
        } else if (dd.idVendor == DOCK_DUSB_VID && dd.idProduct == DOCK_DUSB_PID_84PCE) {
            dusb_seen = true;
            if (!u->dusb.h && !u->fast.h && !u->paused && u->dusb_allowed &&
                !open_dusb(u, list[i]))
                ok = false;
        }
    }
    libusb_free_device_list(list, 1);
    u->dusb.present = dusb_seen;
    if (!ok) {
        u->rescan = true;
        schedule_retry(u, now);
    } else {
        u->backoff_ms = RETRY_MIN_MS;
        u->next_scan_ms = u->hotplug ? 0 : now + RESCAN_MS;
        if (!u->hotplug)
            u->rescan = true;
    }
}

/* ------------------------------------------------------------------ */
/* public                                                              */

usb_link_t *usb_link_open(const usb_link_cb_t *cb)
{
    usb_link_t *u = calloc(1, sizeof *u);
    int rc;
    if (!u)
        return NULL;
    u->cb = *cb;
    u->dusb_allowed = true;
    u->fast_reset = true;
    u->backoff_ms = RETRY_MIN_MS;
    rc = libusb_init(&u->ctx);
    if (rc != 0) {
        loge("usb: libusb_init: %s", libusb_error_name(rc));
        free(u);
        return NULL;
    }
    if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        int ev = LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT;
        u->hotplug =
            libusb_hotplug_register_callback(u->ctx, ev, 0, DOCK_USB_VID, DOCK_USB_PID,
                                             LIBUSB_HOTPLUG_MATCH_ANY, hotplug_cb, u,
                                             &u->hp[0]) == 0 &&
            libusb_hotplug_register_callback(u->ctx, ev, 0, DOCK_DUSB_VID,
                                             DOCK_DUSB_PID_84PCE, LIBUSB_HOTPLUG_MATCH_ANY,
                                             hotplug_cb, u, &u->hp[1]) == 0;
    }
    if (!u->hotplug)
        logv("usb: no hotplug support, polling every %u ms", RESCAN_MS);
    u->rescan = true;
    return u;
}

void usb_link_close(usb_link_t *u)
{
    uint64_t t0;
    if (!u)
        return;
    dusb_close(u);
    fast_fail(u, "shutdown");
    t0 = mono_ms();
    while (u->fast.closing && mono_ms() - t0 < 1000u) {
        struct timeval tv = {0, 50000};
        libusb_handle_events_timeout_completed(u->ctx, &tv, NULL);
        fast_finish_close(u);
    }
    if (u->hotplug) {
        libusb_hotplug_deregister_callback(u->ctx, u->hp[0]);
        libusb_hotplug_deregister_callback(u->ctx, u->hp[1]);
    }
    free(u->fast.stage);
    libusb_exit(u->ctx);
    free(u);
}

int usb_link_pollfds(usb_link_t *u, struct pollfd *pfd, int max)
{
    const struct libusb_pollfd **list = libusb_get_pollfds(u->ctx);
    int n = 0, i;
    if (!list)
        return 0;
    for (i = 0; list[i] && n < max; i++) {
        pfd[n].fd = list[i]->fd;
        pfd[n].events = list[i]->events;
        pfd[n++].revents = 0;
    }
    libusb_free_pollfds(list);
    return n;
}

int usb_link_timeout_ms(usb_link_t *u, uint64_t now_ms)
{
    struct timeval tv;
    int t = -1;
    if (libusb_get_next_timeout(u->ctx, &tv) == 1)
        t = (int)(tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000);
    if (u->rescan && !u->paused) {
        int st = u->next_scan_ms <= now_ms ? 0 : (int)(u->next_scan_ms - now_ms);
        if (t < 0 || st < t)
            t = st;
    }
    if (u->fast.closing && (t < 0 || t > 50))
        t = 50;
    return t;
}

void usb_link_handle(usb_link_t *u, uint64_t now_ms)
{
    struct timeval zero = {0, 0};
    fast_t *f = &u->fast;

    libusb_handle_events_timeout_completed(u->ctx, &zero, NULL);
    fast_finish_close(u);
    if (f->notify_down) {
        f->notify_down = false;
        if (u->cb.fast_down)
            u->cb.fast_down(u->cb.user);
    }
    if (u->fast.h && u->dusb.h)
        dusb_close(u); /* the DOCK driver took over the port */
    if (u->rescan && !u->paused && now_ms >= u->next_scan_ms)
        scan_devices(u, now_ms);
    if (f->notify_up) {
        f->notify_up = false;
        if (u->cb.fast_up)
            u->cb.fast_up(u->cb.user);
    }
    usb_link_flush(u);
}

void usb_link_set_paused(usb_link_t *u, bool paused)
{
    if (u->paused == paused)
        return;
    u->paused = paused;
    if (paused) {
        dusb_close(u);
        fast_fail(u, "paused");
    } else {
        u->fast_reset = true;
        u->rescan = true;
        u->next_scan_ms = 0;
        u->backoff_ms = RETRY_MIN_MS;
    }
}
