/* display_drm.c - see display_drm.h. */
#define _GNU_SOURCE
#include "display_drm.h"
#include "util.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct dumb {
    uint32_t handle, fb_id, pitch, w, h;
    uint64_t size;
    uint32_t *map;
} dumb_t;

typedef struct src {
    uint32_t *px;       /* dumb map (plane mode) or malloc'd shadow */
    uint32_t stride;    /* pixels */
    dumb_t dumb;        /* plane mode only */
    uint16_t w, h;      /* active content size */
    int dmg_y0, dmg_y1; /* pending damage [y0, y1), y0 >= y1 means none */
} src_t;

struct drm_display {
    int fd;
    uint32_t conn_id, crtc_id;
    int crtc_index;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc;

    dumb_t bg;          /* full-resolution scanout buffer behind the plane */
    src_t src[DRM_BUF_COUNT];
    int shown;

    bool plane_mode;    /* hardware scaling via plane_id */
    uint32_t plane_id;
    bool dirty_supported;
    bool released;
    bool tty_graphics;  /* we switched stdin's VT to KD_GRAPHICS */
};

/* ------------------------------------------------------------------ */
/* dumb buffers                                                        */

static void dumb_destroy(int fd, dumb_t *b)
{
    struct drm_mode_destroy_dumb dd;
    if (b->map)
        munmap(b->map, b->size);
    if (b->fb_id)
        drmModeRmFB(fd, b->fb_id);
    if (b->handle) {
        memset(&dd, 0, sizeof dd);
        dd.handle = b->handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    memset(b, 0, sizeof *b);
}

static bool dumb_create(int fd, dumb_t *b, uint32_t w, uint32_t h)
{
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    void *map;

    memset(b, 0, sizeof *b);
    memset(&cd, 0, sizeof cd);
    cd.width = w;
    cd.height = h;
    cd.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
        loge("drm: create dumb %ux%u: %s", w, h, strerror(errno));
        return false;
    }
    b->handle = cd.handle;
    b->pitch = cd.pitch;
    b->size = cd.size;
    b->w = w;
    b->h = h;

    handles[0] = cd.handle;
    pitches[0] = cd.pitch;
    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                      &b->fb_id, 0) != 0) {
        loge("drm: addfb2: %s", strerror(errno));
        dumb_destroy(fd, b);
        return false;
    }
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) {
        loge("drm: map dumb: %s", strerror(errno));
        dumb_destroy(fd, b);
        return false;
    }
    map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
    if (map == MAP_FAILED) {
        loge("drm: mmap: %s", strerror(errno));
        dumb_destroy(fd, b);
        return false;
    }
    b->map = map;
    memset(b->map, 0, cd.size); /* black */
    return true;
}

/* ------------------------------------------------------------------ */
/* card / connector / crtc selection                                   */

static bool pick_mode(drmModeConnector *c, drmModeModeInfo *out)
{
    int i;
    if (c->connection != DRM_MODE_CONNECTED || c->count_modes <= 0)
        return false;
    *out = c->modes[0];
    for (i = 0; i < c->count_modes; i++) {
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            *out = c->modes[i];
            break;
        }
    }
    return true;
}

/* Find a CRTC usable by the connector; prefer the one already driving it. */
static bool pick_crtc(int fd, drmModeRes *res, drmModeConnector *c,
                      uint32_t *crtc_id, int *crtc_index)
{
    int i, j;
    uint32_t want = 0;
    if (c->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
        if (e) {
            want = e->crtc_id;
            drmModeFreeEncoder(e);
        }
    }
    for (i = 0; want && i < res->count_crtcs; i++) {
        if (res->crtcs[i] == want) {
            *crtc_id = want;
            *crtc_index = i;
            return true;
        }
    }
    for (j = 0; j < c->count_encoders; j++) {
        drmModeEncoder *e = drmModeGetEncoder(fd, c->encoders[j]);
        if (!e)
            continue;
        for (i = 0; i < res->count_crtcs; i++) {
            if (e->possible_crtcs & (1u << i)) {
                *crtc_id = res->crtcs[i];
                *crtc_index = i;
                drmModeFreeEncoder(e);
                return true;
            }
        }
        drmModeFreeEncoder(e);
    }
    return false;
}

/* Try one card: true if it has a connected output (fills d). */
static bool probe_card(drm_display_t *d, const char *path)
{
    drmModeRes *res;
    int fd, i;
    bool ok = false;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    res = drmModeGetResources(fd);
    if (!res) { /* render-only node such as v3d */
        close(fd);
        return false;
    }
    for (i = 0; i < res->count_connectors && !ok; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c)
            continue;
        if (pick_mode(c, &d->mode) &&
            pick_crtc(fd, res, c, &d->crtc_id, &d->crtc_index)) {
            d->conn_id = c->connector_id;
            ok = true;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    if (!ok) {
        close(fd);
        return false;
    }
    d->fd = fd;
    logv("drm: %s connector %u crtc %u mode %s %ux%u@%u", path, d->conn_id,
         d->crtc_id, d->mode.name, d->mode.hdisplay, d->mode.vdisplay,
         d->mode.vrefresh);
    return true;
}

static bool open_card(drm_display_t *d, const char *card)
{
    char path[32];
    int i;
    if (card)
        return probe_card(d, card);
    for (i = 0; i < 16; i++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        if (probe_card(d, path))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* planes                                                              */

/* Value of a named property of a plane, or -1. Optionally its prop id. */
static int64_t plane_prop(int fd, uint32_t plane_id, const char *name,
                          uint32_t *prop_id, drmModePropertyRes **prop_out)
{
    drmModeObjectProperties *props;
    int64_t val = -1;
    uint32_t i;
    props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return -1;
    for (i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        if (strcmp(p->name, name) == 0) {
            val = (int64_t)props->prop_values[i];
            if (prop_id)
                *prop_id = p->prop_id;
            if (prop_out) {
                *prop_out = p;
                p = NULL;
            }
        }
        if (p)
            drmModeFreeProperty(p);
        if (val >= 0)
            break;
    }
    drmModeFreeObjectProperties(props);
    return val;
}

/* Ask for crisp pixels (kernel >= 5.18 exposes SCALING_FILTER on vc4). */
static void plane_set_nearest(int fd, uint32_t plane_id)
{
    drmModePropertyRes *p = NULL;
    uint32_t prop_id = 0;
    int i;
    if (plane_prop(fd, plane_id, "SCALING_FILTER", &prop_id, &p) < 0 || !p)
        return;
    for (i = 0; i < p->count_enums; i++) {
        if (strcmp(p->enums[i].name, "Nearest Neighbor") == 0) {
            if (drmModeObjectSetProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                         prop_id, p->enums[i].value) != 0)
                logv("drm: SCALING_FILTER: %s", strerror(errno));
            break;
        }
    }
    drmModeFreeProperty(p);
}

static bool plane_has_xrgb(drmModePlane *p)
{
    uint32_t i;
    for (i = 0; i < p->count_formats; i++)
        if (p->formats[i] == DRM_FORMAT_XRGB8888)
            return true;
    return false;
}

/* Integer scale + centring of a w x h source on the mode. */
static void fit_rect(const drm_display_t *d, uint16_t w, uint16_t h,
                     uint32_t *x, uint32_t *y, uint32_t *dw, uint32_t *dh)
{
    uint32_t mw = d->mode.hdisplay, mh = d->mode.vdisplay;
    uint32_t s = mw / w < mh / h ? mw / w : mh / h;
    if (s == 0) { /* tiny screen: shrink to fit (not integer) */
        *dw = mw;
        *dh = mh;
    } else {
        *dw = w * s;
        *dh = h * s;
    }
    *x = (mw - *dw) / 2;
    *y = (mh - *dh) / 2;
}

static bool plane_show(drm_display_t *d, int which)
{
    src_t *s = &d->src[which];
    uint32_t x, y, w, h;
    fit_rect(d, s->w, s->h, &x, &y, &w, &h);
    return drmModeSetPlane(d->fd, d->plane_id, d->crtc_id, s->dumb.fb_id, 0,
                           (int32_t)x, (int32_t)y, w, h, 0, 0,
                           (uint32_t)s->w << 16, (uint32_t)s->h << 16) == 0;
}

/* Find a plane on our CRTC that accepts the scaled source: overlays first,
 * then the primary plane. */
static bool setup_plane(drm_display_t *d)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(d->fd);
    int pass;
    uint32_t i;
    if (!pr)
        return false;
    for (pass = 0; pass < 2 && !d->plane_mode; pass++) {
        int64_t want = pass == 0 ? DRM_PLANE_TYPE_OVERLAY : DRM_PLANE_TYPE_PRIMARY;
        for (i = 0; i < pr->count_planes && !d->plane_mode; i++) {
            drmModePlane *p = drmModeGetPlane(d->fd, pr->planes[i]);
            bool usable;
            if (!p)
                continue;
            usable = (p->possible_crtcs & (1u << d->crtc_index)) && plane_has_xrgb(p) &&
                     plane_prop(d->fd, p->plane_id, "type", NULL, NULL) == want;
            if (usable) {
                d->plane_id = p->plane_id;
                plane_set_nearest(d->fd, d->plane_id);
                if (plane_show(d, d->shown)) {
                    d->plane_mode = true;
                    logv("drm: hardware scaling on plane %u", d->plane_id);
                } else {
                    logv("drm: plane %u refused: %s", d->plane_id, strerror(errno));
                }
            }
            drmModeFreePlane(p);
        }
    }
    drmModeFreePlaneResources(pr);
    return d->plane_mode;
}

/* ------------------------------------------------------------------ */
/* CPU fallback scaling                                                */

static void cpu_clear_bg(drm_display_t *d)
{
    memset(d->bg.map, 0, d->bg.size);
}

/* Scale rows [y0, y1) of the shown source into the scanout buffer. */
static void cpu_scale_rows(drm_display_t *d, int y0, int y1)
{
    src_t *s = &d->src[d->shown];
    uint32_t ox, oy, dw, dh, scale, bstride = d->bg.pitch / 4u;
    int y;
    fit_rect(d, s->w, s->h, &ox, &oy, &dw, &dh);
    scale = dw / s->w;
    if (scale == 0)
        return; /* screen smaller than the source: not supported here */
    for (y = y0; y < y1 && y < s->h; y++) {
        const uint32_t *sp = s->px + (size_t)y * s->stride;
        uint32_t *dp = d->bg.map + (size_t)(oy + (uint32_t)y * scale) * bstride + ox;
        uint32_t x, k;
        for (x = 0; x < s->w; x++)
            for (k = 0; k < scale; k++)
                *dp++ = sp[x];
        dp -= dw;
        for (k = 1; k < scale; k++)
            memcpy(dp + (size_t)k * bstride, dp, (size_t)dw * 4u);
    }
}

/* ------------------------------------------------------------------ */
/* modesetting                                                         */

static void tty_mode(drm_display_t *d, bool graphics)
{
    if (!isatty(STDIN_FILENO))
        return;
    if (ioctl(STDIN_FILENO, KDSETMODE, graphics ? KD_GRAPHICS : KD_TEXT) == 0)
        d->tty_graphics = graphics;
    else
        logv("drm: KDSETMODE: %s", strerror(errno));
}

static bool modeset(drm_display_t *d)
{
    if (drmModeSetCrtc(d->fd, d->crtc_id, d->bg.fb_id, 0, 0, &d->conn_id, 1,
                       &d->mode) != 0) {
        loge("drm: setcrtc: %s", strerror(errno));
        return false;
    }
    if (d->plane_mode && !plane_show(d, d->shown)) {
        loge("drm: setplane after modeset: %s", strerror(errno));
        return false;
    }
    if (!d->plane_mode) {
        cpu_clear_bg(d);
        d->src[d->shown].dmg_y0 = 0;
        d->src[d->shown].dmg_y1 = d->src[d->shown].h;
    }
    return true;
}

static bool alloc_sources(drm_display_t *d)
{
    int i;
    for (i = 0; i < DRM_BUF_COUNT; i++) {
        src_t *s = &d->src[i];
        s->w = DRM_SRC_W;
        s->h = DRM_SRC_H;
        s->dmg_y0 = s->dmg_y1 = 0;
        if (!dumb_create(d->fd, &s->dumb, DRM_SRC_W, DRM_SRC_H))
            return false;
        s->px = s->dumb.map;
        s->stride = s->dumb.pitch / 4u;
    }
    return true;
}

/* Plane scaling refused: move the sources into RAM. */
static bool use_cpu_sources(drm_display_t *d)
{
    int i;
    for (i = 0; i < DRM_BUF_COUNT; i++) {
        src_t *s = &d->src[i];
        dumb_destroy(d->fd, &s->dumb);
        s->px = calloc((size_t)DRM_SRC_W * DRM_SRC_H, 4u);
        if (!s->px)
            return false;
        s->stride = DRM_SRC_W;
    }
    logv("drm: no scaling plane, using CPU scaling");
    return true;
}

drm_display_t *drm_display_open(const char *card)
{
    drm_display_t *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->fd = -1;
    d->dirty_supported = true;
    d->shown = DRM_BUF_UI;
    if (!open_card(d, card)) {
        loge("drm: no card with a connected display");
        free(d);
        return NULL;
    }
    if (drmSetClientCap(d->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
        logv("drm: universal planes cap: %s", strerror(errno));
    if (drmSetMaster(d->fd) != 0)
        logv("drm: setmaster: %s (continuing)", strerror(errno));
    d->saved_crtc = drmModeGetCrtc(d->fd, d->crtc_id);

    if (!dumb_create(d->fd, &d->bg, d->mode.hdisplay, d->mode.vdisplay) ||
        !alloc_sources(d))
        goto fail;
    tty_mode(d, true);
    if (drmModeSetCrtc(d->fd, d->crtc_id, d->bg.fb_id, 0, 0, &d->conn_id, 1,
                       &d->mode) != 0) {
        loge("drm: setcrtc: %s", strerror(errno));
        goto fail;
    }
    if (!setup_plane(d) && !use_cpu_sources(d))
        goto fail;
    return d;
fail:
    drm_display_close(d);
    return NULL;
}

void drm_display_close(drm_display_t *d)
{
    int i;
    if (!d)
        return;
    if (d->fd >= 0) {
        if (d->plane_mode && !d->released)
            drmModeSetPlane(d->fd, d->plane_id, d->crtc_id, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0);
        if (d->saved_crtc && !d->released) {
            drmModeCrtc *c = d->saved_crtc;
            if (c->buffer_id)
                drmModeSetCrtc(d->fd, c->crtc_id, c->buffer_id, c->x, c->y,
                               &d->conn_id, 1, &c->mode);
        }
        for (i = 0; i < DRM_BUF_COUNT; i++) {
            if (d->src[i].dumb.handle)
                dumb_destroy(d->fd, &d->src[i].dumb);
            else
                free(d->src[i].px);
        }
        dumb_destroy(d->fd, &d->bg);
        if (d->saved_crtc)
            drmModeFreeCrtc(d->saved_crtc);
        if (!d->released)
            drmDropMaster(d->fd);
        close(d->fd);
    }
    if (d->tty_graphics)
        tty_mode(d, false);
    free(d);
}

uint32_t *drm_display_buffer(drm_display_t *d, int which, uint32_t *stride_px)
{
    *stride_px = d->src[which].stride;
    return d->src[which].px;
}

void drm_display_set_source(drm_display_t *d, int which, uint16_t w, uint16_t h)
{
    src_t *s = &d->src[which];
    if (w == 0 || h == 0 || w > DRM_SRC_W || h > DRM_SRC_H)
        return;
    if (s->w == w && s->h == h)
        return;
    s->w = w;
    s->h = h;
    if (which != d->shown || d->released)
        return;
    if (d->plane_mode) {
        if (!plane_show(d, which))
            loge("drm: setplane: %s", strerror(errno));
    } else {
        cpu_clear_bg(d);
        s->dmg_y0 = 0;
        s->dmg_y1 = h;
    }
}

void drm_display_damage(drm_display_t *d, int which, uint16_t y0, uint16_t count)
{
    src_t *s = &d->src[which];
    int y1 = (int)y0 + count;
    if (count == 0)
        return;
    if (s->dmg_y0 >= s->dmg_y1) {
        s->dmg_y0 = y0;
        s->dmg_y1 = y1;
        return;
    }
    if (y0 < s->dmg_y0)
        s->dmg_y0 = y0;
    if (y1 > s->dmg_y1)
        s->dmg_y1 = y1;
}

void drm_display_show(drm_display_t *d, int which)
{
    if (which == d->shown)
        return;
    d->shown = which;
    if (d->released)
        return;
    if (d->plane_mode) {
        if (!plane_show(d, which))
            loge("drm: setplane: %s", strerror(errno));
    } else {
        cpu_clear_bg(d);
        d->src[which].dmg_y0 = 0;
        d->src[which].dmg_y1 = d->src[which].h;
    }
}

static void dirty_fb(drm_display_t *d, uint32_t fb_id, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h)
{
    drmModeClip clip;
    if (!d->dirty_supported)
        return;
    clip.x1 = (uint16_t)x;
    clip.y1 = (uint16_t)y;
    clip.x2 = (uint16_t)(x + w);
    clip.y2 = (uint16_t)(y + h);
    if (drmModeDirtyFB(d->fd, fb_id, &clip, 1) != 0 &&
        (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL)) {
        d->dirty_supported = false; /* driver scans memory directly */
    }
}

void drm_display_flush(drm_display_t *d)
{
    src_t *s = &d->src[d->shown];
    int i;
    if (d->released) {
        return; /* keep damage pending until we own the screen again */
    }
    if (s->dmg_y0 < s->dmg_y1) {
        int y0 = s->dmg_y0, y1 = s->dmg_y1 > s->h ? s->h : s->dmg_y1;
        if (d->plane_mode) {
            dirty_fb(d, s->dumb.fb_id, 0, (uint32_t)y0, s->w, (uint32_t)(y1 - y0));
        } else if (y0 < y1) {
            uint32_t ox, oy, dw, dh, scale;
            cpu_scale_rows(d, y0, y1);
            fit_rect(d, s->w, s->h, &ox, &oy, &dw, &dh);
            scale = dw / s->w;
            dirty_fb(d, d->bg.fb_id, ox, oy + (uint32_t)y0 * scale, dw,
                     (uint32_t)(y1 - y0) * scale);
        }
    }
    /* Damage of the hidden buffer is irrelevant: showing it redraws all. */
    for (i = 0; i < DRM_BUF_COUNT; i++)
        d->src[i].dmg_y0 = d->src[i].dmg_y1 = 0;
}

bool drm_display_release(drm_display_t *d)
{
    if (d->released)
        return true;
    if (d->plane_mode)
        drmModeSetPlane(d->fd, d->plane_id, d->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    tty_mode(d, false);
    if (drmDropMaster(d->fd) != 0)
        logv("drm: dropmaster: %s", strerror(errno));
    d->released = true;
    return true;
}

bool drm_display_acquire(drm_display_t *d)
{
    if (!d->released)
        return true;
    if (drmSetMaster(d->fd) != 0) {
        loge("drm: setmaster: %s", strerror(errno));
        return false;
    }
    d->released = false;
    tty_mode(d, true);
    return modeset(d);
}
