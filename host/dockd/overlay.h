/*
 * overlay.h - the diagnostics "performance overlay": a compact Steam-style
 * stats panel stamped into the top-left of the mirrored calculator screen.
 *
 * It is drawn straight into the mirror framebuffer each loop iteration (when
 * enabled and the mirror is shown), then that rectangle is damaged so it
 * reaches the display. Toggling the overlay off repaints the covered region.
 * State (FPS/throughput deltas, system telemetry) lives in overlay_t inside
 * dockd_t; this module keeps no global state.
 */
#ifndef DOCKD_OVERLAY_H
#define DOCKD_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "sysstat.h"

struct dockd;

#define OVERLAY_COL 24      /* chars per stat line (incl. NUL) */
#define OVERLAY_MAXLINES 8  /* FPS(2) + CONN(3) + SYS(3) */

typedef struct overlay {
    sysstat_t sys;
    uint64_t next_sample_ms; /* earliest next FPS/throughput recompute */
    uint32_t base_frames;    /* frame/byte counters at the last sample */
    uint32_t base_bytes;
    bool primed;             /* a baseline sample was taken */
    int fps_tenths;          /* frames per second * 10 */
    int kb_per_s;            /* received KiB/s */
    bool prev_on;            /* previous s.perf_overlay (off-transition hook) */
} overlay_t;

/* Draw the panel into the mirror and damage it. No-op unless the overlay is
 * on, no screen (settings/shell) covers the mirror, and the mirror is shown. */
void overlay_render(struct dockd *d);

/* Refresh the sampled stats (system telemetry + FPS/throughput deltas)
 * without drawing; used by the diagnostics screen. */
void overlay_update_stats(struct dockd *d, uint64_t now_ms);

/* Fill up to `max` lines with the enabled sections' stat text (the same lines
 * the overlay draws). Returns the number of lines written. */
int overlay_lines(const struct dockd *d, char lines[][OVERLAY_COL], int max);

/* Poll interval (ms) that keeps the stats live while the picture is static,
 * or -1 when the overlay is not currently drawing. */
int overlay_timeout_ms(const struct dockd *d);

/* Called from dockd_settings_changed: on a true->false transition of
 * perf_overlay, repaint the region the panel covered. */
void overlay_settings_changed(struct dockd *d);

#endif /* DOCKD_OVERLAY_H */
