/* display.c - backend selection, plus the "none" and "ppm" backends. */
#define _GNU_SOURCE
#include "display.h"
#include "display_drm.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum backend { BK_NONE, BK_PPM, BK_DRM };

struct display {
    enum backend kind;
    drm_display_t *drm;
    uint32_t *ram[2]; /* BK_NONE / BK_PPM buffers */
    char *ppm_path;
    int shown;
    bool ui_dirty;
};

/* Write an XRGB8888 buffer as binary PPM, atomically (tmp + rename) so a
 * reader never sees a half-written file. */
static bool write_ppm(const char *path, const uint32_t *px, uint32_t w,
                      uint32_t h, uint32_t stride)
{
    char tmp[4096];
    uint8_t *rgb;
    int fd, n;
    uint32_t x, y;
    bool ok;

    n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp)
        return false;
    rgb = malloc((size_t)w * h * 3u);
    if (!rgb)
        return false;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * stride + x];
            uint8_t *o = rgb + ((size_t)y * w + x) * 3u;
            o[0] = (uint8_t)(p >> 16);
            o[1] = (uint8_t)(p >> 8);
            o[2] = (uint8_t)p;
        }
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(rgb);
        return false;
    }
    {
        char hdr[32];
        int hl = snprintf(hdr, sizeof hdr, "P6\n%u %u\n255\n", w, h);
        ok = hl > 0 && write_all(fd, hdr, (size_t)hl) &&
             write_all(fd, rgb, (size_t)w * h * 3u);
    }
    free(rgb);
    if (close(fd) != 0)
        ok = false;
    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok) {
        logv("display: writing %s failed: %s", path, strerror(errno));
        unlink(tmp);
    }
    return ok;
}

static void ppm_write_ui(display_t *d)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s.ui.ppm", d->ppm_path);
    if (n > 0 && (size_t)n < sizeof path)
        write_ppm(path, d->ram[DISPLAY_UI], DISPLAY_W, DISPLAY_H, DISPLAY_W);
}

display_t *display_open(const char *spec)
{
    display_t *d = calloc(1, sizeof *d);
    int i;
    if (!d)
        return NULL;
    d->shown = DISPLAY_UI;
    if (strcmp(spec, "drm") == 0 || strncmp(spec, "drm:", 4) == 0) {
        d->kind = BK_DRM;
        d->drm = drm_display_open(spec[3] == ':' ? spec + 4 : NULL);
        if (!d->drm) {
            free(d);
            return NULL;
        }
        return d;
    }
    if (strncmp(spec, "ppm:", 4) == 0 && spec[4]) {
        d->kind = BK_PPM;
        d->ppm_path = strdup(spec + 4);
        if (!d->ppm_path)
            goto fail;
    } else if (strcmp(spec, "none") == 0) {
        d->kind = BK_NONE;
    } else {
        loge("unknown display '%s' (drm, drm:CARD, none, ppm:PATH)", spec);
        goto fail;
    }
    for (i = 0; i < 2; i++) {
        d->ram[i] = calloc((size_t)DISPLAY_W * DISPLAY_H, 4u);
        if (!d->ram[i])
            goto fail;
    }
    return d;
fail:
    free(d->ram[0]);
    free(d->ram[1]);
    free(d->ppm_path);
    free(d);
    return NULL;
}

void display_close(display_t *d)
{
    if (!d)
        return;
    if (d->kind == BK_DRM) {
        drm_display_close(d->drm);
    } else {
        if (d->kind == BK_PPM)
            write_ppm(d->ppm_path, d->ram[DISPLAY_MIRROR], DISPLAY_W, DISPLAY_H,
                      DISPLAY_W);
        free(d->ram[0]);
        free(d->ram[1]);
        free(d->ppm_path);
    }
    free(d);
}

uint32_t *display_buffer(display_t *d, int view, uint32_t *stride_px)
{
    if (d->kind == BK_DRM)
        return drm_display_buffer(d->drm, view == DISPLAY_MIRROR ? DRM_BUF_MIRROR
                                                                 : DRM_BUF_UI,
                                  stride_px);
    *stride_px = DISPLAY_W;
    return d->ram[view == DISPLAY_MIRROR ? 1 : 0];
}

void display_set_mirror_size(display_t *d, uint16_t w, uint16_t h)
{
    if (d->kind == BK_DRM)
        drm_display_set_source(d->drm, DRM_BUF_MIRROR, w, h);
}

void display_damage(display_t *d, int view, uint16_t y0, uint16_t count)
{
    if (d->kind == BK_DRM)
        drm_display_damage(d->drm, view == DISPLAY_MIRROR ? DRM_BUF_MIRROR : DRM_BUF_UI,
                           y0, count);
    else if (view == DISPLAY_UI)
        d->ui_dirty = true;
}

void display_show(display_t *d, int view)
{
    d->shown = view;
    if (d->kind == BK_DRM)
        drm_display_show(d->drm, view == DISPLAY_MIRROR ? DRM_BUF_MIRROR : DRM_BUF_UI);
}

int display_shown(const display_t *d)
{
    return d->shown;
}

void display_flush(display_t *d)
{
    if (d->kind == BK_DRM) {
        drm_display_flush(d->drm);
    } else if (d->kind == BK_PPM && d->ui_dirty) {
        ppm_write_ui(d);
    }
    d->ui_dirty = false;
}

void display_frame_end(display_t *d)
{
    if (d->kind == BK_PPM)
        write_ppm(d->ppm_path, d->ram[DISPLAY_MIRROR], DISPLAY_W, DISPLAY_H, DISPLAY_W);
}

bool display_release(display_t *d)
{
    return d->kind != BK_DRM || drm_display_release(d->drm);
}

bool display_acquire(display_t *d)
{
    return d->kind != BK_DRM || drm_display_acquire(d->drm);
}
