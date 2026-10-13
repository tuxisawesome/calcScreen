/*
 * display.h - output backends for the dock's two 320x240 XRGB8888 buffers
 * (calculator mirror, dock UI):
 *   "drm" / "drm:/dev/dri/cardN"  fullscreen on the monitor (display_drm.c)
 *   "none"                         buffers in RAM, nothing shown
 *   "ppm:PATH"                     buffers in RAM; the mirror is written to
 *                                  PATH at every frame end and on close, the UI
 *                                  to PATH.ui.ppm when it changes (headless
 *                                  tests)
 */
#ifndef HOST_DISPLAY_H
#define HOST_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_W 320u
#define DISPLAY_H 240u

enum display_view { DISPLAY_UI = 0, DISPLAY_MIRROR = 1 };

typedef struct display display_t;

display_t *display_open(const char *spec);
void display_close(display_t *d);

uint32_t *display_buffer(display_t *d, int view, uint32_t *stride_px);
/* Active size of the mirror content. */
void display_set_mirror_size(display_t *d, uint16_t w, uint16_t h);
/* Rows of a buffer changed (the UI is usually redrawn whole). */
void display_damage(display_t *d, int view, uint16_t y0, uint16_t count);
void display_show(display_t *d, int view);
int display_shown(const display_t *d);
/* Once per loop iteration. */
void display_flush(display_t *d);
/* A complete calculator frame has been received. */
void display_frame_end(display_t *d);

bool display_release(display_t *d);
bool display_acquire(display_t *d);

#endif /* HOST_DISPLAY_H */
