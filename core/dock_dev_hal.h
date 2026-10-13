/*
 * dock_dev_hal.h - what a *device* platform (the calculator, a simulator, a
 * future MCU) must provide to run the portable device engine (dock_dev.c).
 *
 * Link-time interface: implement these functions once per platform. Every
 * call gets the opaque `hal` pointer given to dock_dev_init().
 *
 * Transport model: a reliable, ordered byte pipe with at most ONE transmit in
 * flight. dock_dhal_tx() starts sending `n` bytes from `p` and returns
 * immediately; `p` must stay valid until dock_dhal_tx_idle() is true again.
 * That lets the calculator DMA straight out of VRAM.
 */
#ifndef DOCK_DEV_HAL_H
#define DOCK_DEV_HAL_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dock_fb {
    const uint8_t *pixels; /* first row */
    uint16_t width, height;
    uint16_t stride;       /* bytes between rows */
    uint8_t fmt;           /* enum dock_pix_fmt */
    uint8_t flags;         /* DOCK_SCRF_* */
    /* Palette as raw little-endian 16-bit entries (2 bytes each), NULL for
     * direct colour. Sent zero-copy, hence bytes rather than uint16_t. */
    const uint8_t *palette_le;
} dock_fb_t;

/* Pump the low-level driver (bus events, control endpoint, DMA). Cheap when
 * nothing happened. */
void dock_dhal_service(void *hal);

/* True while a host has configured us and the link can carry data. */
bool dock_dhal_link_up(void *hal);

/* Transmit: see the header comment. `last` marks the final piece of a frame
 * so the platform can terminate the transfer (e.g. USB zero-length packet
 * when n is a multiple of the max packet size). Returns false if busy or the
 * link is down. */
bool dock_dhal_tx(void *hal, const void *p, size_t n, bool last);
bool dock_dhal_tx_idle(void *hal);
/* Largest n accepted by one dock_dhal_tx() call. */
size_t dock_dhal_tx_max(void *hal);

/* Non-blocking receive: copy up to max already-received bytes. */
size_t dock_dhal_rx(void *hal, uint8_t *buf, size_t max);

/* Monotonic milliseconds (wraps every 2^32 ms). */
uint32_t dock_dhal_ms(void *hal);

/* Describe the live framebuffer. Return false if there is none. */
bool dock_dhal_fb(void *hal, dock_fb_t *fb);

/* Row hash used for change detection; may simply return dock_row_hash_c(). */
uint32_t dock_dhal_row_hash(const uint8_t *row, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_DEV_HAL_H */
