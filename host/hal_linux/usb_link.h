/*
 * usb_link.h - the calculator's USB connection (libusb, poll()-integrated).
 *
 * FAST: the DOCK driver on the calculator enumerates as a vendor-specific
 * bulk device (DOCK_USB_VID:PID, interface FF/D0/C5). Four 16 KiB bulk IN
 * transfers stay queued and feed the received bytes to fast_rx(); transmit
 * data is staged and sent as asynchronous bulk OUT transfers, so tx never
 * blocks. If more than USB_LINK_TX_MAX bytes are pending the calculator is
 * considered dead and the link is dropped.
 *
 * DUSB: a stock TI-84 Plus CE (0451:E008). Synchronous raw-packet I/O for
 * core/dusb.c (half duplex, every call bounded by its timeout).
 *
 * Devices are found by libusb hotplug callbacks (or a 1 s rescan where
 * hotplug is unsupported); failures are retried with backoff. Callbacks are
 * only ever invoked from usb_link_handle() (never from inside libusb
 * transfer callbacks), except fast_rx which runs from the IN completion.
 */
#ifndef HOST_USB_LINK_H
#define HOST_USB_LINK_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_LINK_TX_MAX (4u * 1024u * 1024u)

typedef struct usb_link_cb {
    void *user;
    void (*fast_up)(void *user);
    void (*fast_rx)(void *user, const uint8_t *p, size_t n);
    void (*fast_down)(void *user);
    void (*dusb_up)(void *user);
    void (*dusb_down)(void *user);
} usb_link_cb_t;

typedef struct usb_link usb_link_t;

usb_link_t *usb_link_open(const usb_link_cb_t *cb);
void usb_link_close(usb_link_t *u);

int usb_link_pollfds(usb_link_t *u, struct pollfd *pfd, int max);
int usb_link_timeout_ms(usb_link_t *u, uint64_t now_ms);
/* Handle libusb events, hotplug, retries; flushes pending tx. */
void usb_link_handle(usb_link_t *u, uint64_t now_ms);

/* ---- FAST ---- */
bool usb_link_fast_up(const usb_link_t *u);
/* Copy n bytes into the transmit queue. False if the link is down/dead. */
bool usb_link_tx(usb_link_t *u, const void *p, size_t n);
/* Submit queued transmit data now. */
void usb_link_flush(usb_link_t *u);

/* ---- DUSB ---- */
bool usb_link_dusb_up(const usb_link_t *u);
/* A stock calculator is attached (whether or not DUSB is allowed). */
bool usb_link_dusb_present(const usb_link_t *u);
/* DUSB devices are only opened while allowed (not while join codes are
 * required). Disallowing closes an open one. */
void usb_link_set_dusb_allowed(usb_link_t *u, bool allowed);
bool usb_link_dusb_send(usb_link_t *u, const uint8_t *p, size_t n);
size_t usb_link_dusb_recv(usb_link_t *u, uint8_t *p, size_t cap, uint32_t timeout_ms);
/* The DUSB session failed: close the device and retry later. */
void usb_link_dusb_fail(usb_link_t *u);

/* Leave the calculator alone (drop to shell): close everything and stop
 * opening devices until unpaused. */
void usb_link_set_paused(usb_link_t *u, bool paused);

#endif /* HOST_USB_LINK_H */
