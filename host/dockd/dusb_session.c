/* dusb_session.c - see dusb_session.h. */
#define _GNU_SOURCE
#include "dusb_session.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define DUSB_TIMEOUT_MS 2000u
#define KEYS_PER_STEP 8u
#define VBUF_SIZE (DOCK_DUSB_SCREEN_BYTES + 64u)

static bool io_send(void *user, const uint8_t *p, size_t n)
{
    return usb_link_dusb_send((usb_link_t *)user, p, n);
}

static size_t io_recv(void *user, uint8_t *p, size_t cap, uint32_t timeout_ms)
{
    if (timeout_ms > DUSB_TIMEOUT_MS)
        timeout_ms = DUSB_TIMEOUT_MS;
    return usb_link_dusb_recv((usb_link_t *)user, p, cap, timeout_ms);
}

bool dusb_session_init(dusb_session_t *s, usb_link_t *usb, uint32_t *mirror,
                       uint32_t stride_px)
{
    memset(s, 0, sizeof *s);
    s->usb = usb;
    s->vbuf = malloc(VBUF_SIZE);
    s->shot = malloc(DOCK_DUSB_SCREEN_BYTES);
    if (!s->vbuf || !s->shot) {
        dusb_session_free(s);
        return false;
    }
    /* The CE screenshot is 320x240 RGB565 little-endian, red in the high
     * bits. */
    dock_scr_rx_init(&s->conv, mirror, DOCK_DUSB_SCREEN_W, DOCK_DUSB_SCREEN_H, stride_px);
    s->conv.width = DOCK_DUSB_SCREEN_W;
    s->conv.height = DOCK_DUSB_SCREEN_H;
    s->conv.fmt = DOCK_PIX_RGB565;
    s->conv.flags = DOCK_SCRF_BGR;
    return true;
}

void dusb_session_free(dusb_session_t *s)
{
    free(s->vbuf);
    free(s->shot);
    s->vbuf = s->shot = NULL;
}

void dusb_session_start(dusb_session_t *s)
{
    dock_dusb_io_t io;
    io.user = s->usb;
    io.send = io_send;
    io.recv = io_recv;
    dock_dusb_init(&s->dusb, &io, s->vbuf, VBUF_SIZE);
    s->dusb.timeout_ms = DUSB_TIMEOUT_MS;
    s->active = true;
    s->handshaken = false;
    s->have_frame = false;
    s->khead = s->ktail = 0;
}

void dusb_session_stop(dusb_session_t *s)
{
    s->active = false;
    s->handshaken = false;
    s->have_frame = false;
}

void dusb_session_queue_key(dusb_session_t *s, uint16_t tikey)
{
    unsigned next = (s->khead + 1u) % DUSB_KEYQ;
    if (!s->active || next == s->ktail)
        return; /* full: drop rather than lag behind */
    s->keyq[s->khead] = tikey;
    s->khead = next;
}

static void fail(dusb_session_t *s, const char *what, int rc)
{
    logv("dusb: %s failed (%d)", what, rc);
    dusb_session_stop(s);
    usb_link_dusb_fail(s->usb); /* calls back dusb_down */
}

bool dusb_session_step(dusb_session_t *s)
{
    unsigned sent = 0;
    int rc;
    if (!s->active || !usb_link_dusb_up(s->usb))
        return false;
    if (!s->handshaken) {
        rc = dock_dusb_handshake(&s->dusb);
        if (rc != DOCK_OK) {
            fail(s, "handshake", rc);
            return false;
        }
        s->handshaken = true;
        return false;
    }
    /* Keys first: typing should never wait for a screenshot. */
    while (s->ktail != s->khead && sent++ < KEYS_PER_STEP) {
        rc = dock_dusb_send_key(&s->dusb, s->keyq[s->ktail]);
        s->ktail = (s->ktail + 1u) % DUSB_KEYQ;
        if (rc != DOCK_OK) {
            fail(s, "send key", rc);
            return false;
        }
    }
    rc = dock_dusb_screenshot(&s->dusb, s->shot, DOCK_DUSB_SCREEN_BYTES);
    if (rc != DOCK_OK) {
        fail(s, "screenshot", rc);
        return false;
    }
    dock_scr_convert_rows(&s->conv, s->shot, DOCK_DUSB_SCREEN_W * 2u, 0,
                          DOCK_DUSB_SCREEN_H);
    s->have_frame = true;
    return true;
}
