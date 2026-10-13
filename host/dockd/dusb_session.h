/*
 * dusb_session.h - the DUSB fallback mirror for a stock calculator: grab
 * screenshots as fast as the link allows and send queued keypresses in
 * between. Every step is a bounded blocking exchange (each USB wait <= 2 s)
 * so the daemon can service input between steps.
 */
#ifndef DOCKD_DUSB_SESSION_H
#define DOCKD_DUSB_SESSION_H

#include "dock_screen.h"
#include "dusb.h"
#include "usb_link.h"

#define DUSB_KEYQ 32u

typedef struct dusb_session {
    usb_link_t *usb;
    dock_dusb_t dusb;
    uint8_t *vbuf;          /* virtual packet reassembly */
    uint8_t *shot;          /* one RGB565 screenshot */
    dock_scr_rx_t conv;     /* converter into the mirror buffer */
    bool active;
    bool handshaken;
    bool have_frame;
    uint16_t keyq[DUSB_KEYQ];
    unsigned khead, ktail;
} dusb_session_t;

bool dusb_session_init(dusb_session_t *s, usb_link_t *usb, uint32_t *mirror,
                       uint32_t stride_px);
void dusb_session_free(dusb_session_t *s);

void dusb_session_start(dusb_session_t *s);
void dusb_session_stop(dusb_session_t *s);
void dusb_session_queue_key(dusb_session_t *s, uint16_t tikey);

/* Do one step (handshake, or keys + one screenshot). Returns true when the
 * mirror buffer was updated. On failure the USB device is dropped and
 * retried later by usb_link. */
bool dusb_session_step(dusb_session_t *s);

#endif /* DOCKD_DUSB_SESSION_H */
