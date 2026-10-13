/* overlay.c - see overlay.h. */
#define _GNU_SOURCE
#include "overlay.h"
#include "dockd.h"
#include "usb_link.h"
#include "util.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

#define OVL_BG 0xE00A0E12u   /* dark panel (alpha is cosmetic; buffer is XRGB) */
#define OVL_FG DOCK_UI_FG
#define OVL_ACCENT DOCK_UI_ACCENT
#define OVL_PADX 4
#define OVL_PADY 3
#define OVL_LINE 9           /* glyph is 8 px; 1 px leading */
#define SAMPLE_MS 500u

/* The mirror is the shown view (mirrors wanted_view() in main.c). */
static bool mirror_up(const dockd_t *d)
{
    if (d->settings_open || d->shell_active)
        return false;
    return d->mode == MODE_FAST ||
           (d->mode == MODE_DUSB && d->dusb.have_frame);
}

static uint32_t frame_count(const dockd_t *d)
{
    return d->mode == MODE_DUSB ? d->dusb.conv.frames : d->host.scr.frames;
}

/* Received bytes; DUSB has no byte stream, so approximate from frames. */
static uint32_t byte_count(const dockd_t *d)
{
    if (d->mode == MODE_DUSB)
        return d->dusb.conv.frames * (DISPLAY_W * DISPLAY_H * 2u);
    return d->host.bytes_rx;
}

/* Recompute FPS/throughput from ~500 ms deltas. */
static void sample_rates(dockd_t *d, uint64_t now)
{
    overlay_t *o = &d->ovl;
    uint32_t frames = frame_count(d), bytes = byte_count(d);
    if (!o->primed) {
        o->base_frames = frames;
        o->base_bytes = bytes;
        o->next_sample_ms = now + SAMPLE_MS;
        o->primed = true;
        return;
    }
    if (now < o->next_sample_ms)
        return;
    {
        uint64_t dms = SAMPLE_MS; /* nominal; next_sample_ms paces us */
        uint32_t df = frames - o->base_frames;
        uint32_t db = bytes - o->base_bytes;
        o->fps_tenths = (int)((uint64_t)df * 10000u / dms);
        o->kb_per_s = (int)((uint64_t)db * 1000u / (dms * 1024u));
        o->base_frames = frames;
        o->base_bytes = bytes;
        o->next_sample_ms = now + SAMPLE_MS;
    }
}

static const char *mode_label(const dockd_t *d)
{
    switch (d->mode) {
    case MODE_FAST: return "FAST";
    case MODE_DUSB: return "DUSB";
    default: return "WAIT";
    }
}

int overlay_lines(const dockd_t *d, char lines[][OVERLAY_COL], int max)
{
    const overlay_t *o = &d->ovl;
    uint8_t sec = d->s.perf_overlay_sections;
    int n = 0;
    usb_link_info_t info;
    bool have_usb = d->usb && usb_link_info(d->usb, &info);

#define EMIT(...)                                                   \
    do {                                                            \
        if (n < max)                                                \
            snprintf(lines[n++], OVERLAY_COL, __VA_ARGS__);         \
    } while (0)

    if (sec & DOCK_OVL_FPS) {
        uint32_t frames = d->host.scr.frames;
        unsigned rpf = frames ? d->host.scr.rows / frames : 0;
        int mbit = have_usb ? info.speed_mbit : 0;
        if (d->mode == MODE_DUSB)
            rpf = DISPLAY_H;
        EMIT("FPS %d.%d %dKB/s", o->fps_tenths / 10, o->fps_tenths % 10,
             o->kb_per_s);
        if (mbit > 0)
            EMIT("r/f %u %s %dMb", rpf, mode_label(d), mbit);
        else
            EMIT("r/f %u %s", rpf, mode_label(d));
    }
    if (sec & DOCK_OVL_CONN) {
        const char *join = d->host.accepted ? "ok"
                           : d->mode == MODE_JOIN ? "need"
                                                  : "-";
        uint16_t vid = DOCK_USB_VID, pid = DOCK_USB_PID;
        if (have_usb && info.mode != USB_LINK_NONE) {
            vid = info.vid;
            pid = info.pid;
        }
        EMIT("calc %08X d%u", (unsigned)d->host.calc_id, d->host.driver_ver);
        if (d->mode == MODE_DUSB)
            EMIT("%04X:%04X DUSB", vid, pid);
        else
            EMIT("%04X:%04X %s", vid, pid, join);
        EMIT("rs%u bad%u rq%u", (unsigned)d->host.rx.resyncs,
             (unsigned)d->host.bad_frames, (unsigned)d->host.requests);
    }
    if (sec & DOCK_OVL_SYS) {
        const sysstat_t *s = &o->sys;
        if (s->have_cpu && s->have_temp)
            EMIT("CPU %d%% %dC", s->cpu_pct, s->temp_c);
        else if (s->have_cpu)
            EMIT("CPU %d%% n/a", s->cpu_pct);
        else
            EMIT("CPU n/a");
        if (s->have_mem && s->have_uptime)
            EMIT("mem %uM %u:%02u:%02u", s->mem_used_mb, s->uptime_s / 3600u,
                 (s->uptime_s / 60u) % 60u, s->uptime_s % 60u);
        else if (s->have_mem)
            EMIT("mem %uM", s->mem_used_mb);
        else
            EMIT("mem n/a");
        EMIT("v%s", DOCKD_VERSION);
    }
#undef EMIT
    return n;
}

void overlay_update_stats(dockd_t *d, uint64_t now_ms)
{
    sysstat_sample(&d->ovl.sys, now_ms);
    sample_rates(d, now_ms);
}

static int panel_height(int n)
{
    return 2 * OVL_PADY + n * OVL_LINE;
}

void overlay_render(dockd_t *d)
{
    char lines[OVERLAY_MAXLINES][OVERLAY_COL];
    dock_canvas_t c;
    int n, i, w = 0, pw, ph;

    if (!d->s.perf_overlay || !mirror_up(d) || !d->mirror)
        return;
    overlay_update_stats(d, mono_ms());

    n = overlay_lines(d, lines, OVERLAY_MAXLINES);
    if (n == 0)
        return;
    for (i = 0; i < n; i++) {
        int lw = dock_ui_text_width(lines[i], 1);
        if (lw > w)
            w = lw;
    }
    pw = w + 2 * OVL_PADX;
    ph = panel_height(n);
    if (pw > (int)DISPLAY_W / 2)
        pw = (int)DISPLAY_W / 2;

    c.px = d->mirror;
    c.w = DISPLAY_W;
    c.h = DISPLAY_H;
    c.stride = d->mirror_stride;
    dock_ui_fill(&c, 0, 0, pw, ph, OVL_BG);
    dock_ui_fill(&c, 0, ph - 1, pw, 1, OVL_ACCENT);
    for (i = 0; i < n; i++)
        dock_ui_text(&c, OVL_PADX, OVL_PADY + i * OVL_LINE, lines[i], OVL_FG, 1);
    display_damage(d->disp, DISPLAY_MIRROR, 0, (uint16_t)ph);
}

int overlay_timeout_ms(const dockd_t *d)
{
    if (!d->s.perf_overlay || !mirror_up(d))
        return -1;
    return 250;
}

void overlay_settings_changed(dockd_t *d)
{
    bool on = d->s.perf_overlay != 0;
    if (d->ovl.prev_on && !on) {
        /* Panel just turned off: repaint whatever it covered. */
        if (d->mode == MODE_FAST)
            dock_host_request_refresh(&d->host);
        else
            display_damage(d->disp, DISPLAY_MIRROR, 0, DISPLAY_H);
        dockd_redraw(d);
    }
    if (on)
        d->ovl.primed = false; /* restart the FPS baseline cleanly */
    d->ovl.prev_on = on;
}
