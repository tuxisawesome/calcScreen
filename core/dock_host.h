/*
 * dock_host.h - portable host (dock) engine.
 *
 * Feed it the bytes received from the device; it parses frames, keeps the
 * mirrored XRGB8888 framebuffer up to date and answers requests through the
 * dock_hhal_* services. Buffers are supplied by the caller (no malloc).
 */
#ifndef DOCK_HOST_H
#define DOCK_HOST_H

#include "dock_common.h"
#include "dock_host_hal.h"
#include "dock_proto.h"
#include "dock_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dock_host {
    void *hal;
    dock_rx_t rx;
    dock_scr_rx_t scr;

    uint8_t *resp;      /* response scratch (header + fields + data) */
    size_t resp_cap;
    uint8_t seq;

    bool accepted;      /* WELCOME OK sent */
    uint32_t calc_id;
    uint8_t hello_flags;
    uint16_t driver_ver;

    /* statistics */
    uint32_t frames_rx;
    uint32_t bytes_rx;
    uint32_t requests;
    uint32_t bad_frames;
} dock_host_t;

/*
 * rxbuf: must hold the largest frame (DOCK_MAX_PAYLOAD recommended).
 * respbuf: response scratch; FS/HTTP reads are clipped to fit (64 KiB+ is
 *          plenty).
 * fb: XRGB8888 mirror, fb_w x fb_h pixels, fb_stride_px pixels per row.
 */
void dock_host_init(dock_host_t *h, void *hal, uint8_t *rxbuf, size_t rxcap,
                    uint8_t *respbuf, size_t respcap, uint32_t *fb,
                    uint16_t fb_w, uint16_t fb_h, uint32_t fb_stride_px);

/* The link (re)started: forget the session and wait for HELLO. */
void dock_host_reset(dock_host_t *h);

void dock_host_feed(dock_host_t *h, const uint8_t *data, size_t n);

/* Host -> device messages. Return false if the link is gone. */
bool dock_host_send_key(dock_host_t *h, uint16_t tikey, uint8_t scancode,
                        uint8_t flags);
bool dock_host_send_hidkey(dock_host_t *h, uint8_t usage, uint8_t mods,
                           bool down, uint8_t ascii);
bool dock_host_request_refresh(dock_host_t *h);
bool dock_host_ping(dock_host_t *h);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_HOST_H */
