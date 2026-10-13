/*
 * display_drm.h - DRM/KMS output for the dock (Raspberry Pi 4: vc4).
 *
 * Two 320x240 XRGB8888 source buffers exist: the calculator mirror and the
 * dock's own UI. One of them is shown fullscreen, integer scaled and
 * letterboxed. Preferred path: the buffers are DRM dumb buffers scanned out
 * by an overlay plane and the display controller (vc4 HVS) does the scaling,
 * so writing a pixel into the buffer is all it takes to show it. Fallback
 * (plane scaling refused): the buffers live in RAM and damaged rows are
 * scaled nearest-neighbour by the CPU into a full-resolution scanout buffer.
 */
#ifndef HOST_DISPLAY_DRM_H
#define HOST_DISPLAY_DRM_H

#include <stdbool.h>
#include <stdint.h>

#define DRM_SRC_W 320u
#define DRM_SRC_H 240u

enum drm_buf { DRM_BUF_UI = 0, DRM_BUF_MIRROR = 1, DRM_BUF_COUNT };

typedef struct drm_display drm_display_t;

/* card: device path, or NULL to pick the first card with a connected
 * connector. Returns NULL on failure (reason logged). */
drm_display_t *drm_display_open(const char *card);
void drm_display_close(drm_display_t *d);

/* Pixel memory of a source buffer (DRM_SRC_W x DRM_SRC_H, stride in pixels).
 * Stays valid for the lifetime of the display. */
uint32_t *drm_display_buffer(drm_display_t *d, int which, uint32_t *stride_px);

/* Active size of a buffer's content (<= 320x240), default full size. */
void drm_display_set_source(drm_display_t *d, int which, uint16_t w, uint16_t h);

/* Rows of a buffer were rewritten. Accumulated until drm_display_flush(). */
void drm_display_damage(drm_display_t *d, int which, uint16_t y0, uint16_t count);

/* Choose the buffer that is scanned out. */
void drm_display_show(drm_display_t *d, int which);

/* Push accumulated damage to the screen (call once per loop iteration). */
void drm_display_flush(drm_display_t *d);

/* Give the screen away (drop to shell) and take it back. */
bool drm_display_release(drm_display_t *d);
bool drm_display_acquire(drm_display_t *d);

#endif /* HOST_DISPLAY_DRM_H */
