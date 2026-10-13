/* dock_screen.c - dirty-row sender and row-to-XRGB receiver. */
#include "dock_screen.h"
#include "dock_proto.h"

static const uint8_t pix_bits[DOCK_PIX_COUNT] = {1, 2, 4, 8, 16, 16, 16};

uint8_t dock_pix_bits(uint8_t fmt)
{
    return fmt < DOCK_PIX_COUNT ? pix_bits[fmt] : 0;
}

uint16_t dock_row_bytes(uint16_t width, uint8_t fmt)
{
    uint32_t bits = (uint32_t)width * dock_pix_bits(fmt);
    return (uint16_t)((bits + 7u) / 8u);
}

uint16_t dock_palette_size(uint8_t fmt)
{
    return (uint16_t)(fmt <= DOCK_PIX_I8 ? 1u << dock_pix_bits(fmt) : 0u);
}

uint32_t dock_row_hash_c(const uint8_t *row, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n--) {
        a = (a + *row++) & 0xFFFFFFu;
        b = (b + a) & 0xFFFFFFu;
    }
    return (b << 8) ^ a;
}

/* ---------------- sender ---------------- */

static void mark_all(dock_scr_tx_t *s)
{
    memset(s->dirty, 0xFF, sizeof s->dirty);
}

void dock_scr_tx_init(dock_scr_tx_t *s, dock_hash_fn hash)
{
    memset(s, 0, sizeof *s);
    s->hash = hash ? hash : dock_row_hash_c;
    s->mode_dirty = true;
    s->pal_dirty = true;
    s->force_all = true;
}

void dock_scr_tx_set_mode(dock_scr_tx_t *s, uint16_t width, uint16_t height,
                          uint16_t stride, uint8_t fmt, uint8_t flags)
{
    if (width > DOCK_SCR_MAX_W)
        width = DOCK_SCR_MAX_W;
    if (height > DOCK_SCR_MAX_H)
        height = DOCK_SCR_MAX_H;
    if (width == s->width && height == s->height && stride == s->stride &&
        fmt == s->fmt && flags == s->flags)
        return;
    s->width = width;
    s->height = height;
    s->stride = stride;
    s->fmt = fmt;
    s->flags = flags;
    s->scan_y = 0;
    s->mode_dirty = true;
    s->pal_dirty = dock_palette_size(fmt) != 0;
    s->pal_hash = 0;
    s->force_all = true;
}

void dock_scr_tx_check_palette(dock_scr_tx_t *s, const uint8_t *pal_le, uint16_t n)
{
    uint32_t h;
    if (!pal_le || !n)
        return;
    h = s->hash(pal_le, (size_t)n * 2u);
    if (h != s->pal_hash) {
        s->pal_hash = h;
        s->pal_dirty = true;
    }
}

void dock_scr_tx_refresh(dock_scr_tx_t *s)
{
    s->mode_dirty = true;
    s->pal_dirty = dock_palette_size(s->fmt) != 0;
    s->force_all = true;
}

bool dock_scr_tx_scan(dock_scr_tx_t *s, const uint8_t *fb, uint16_t max_rows)
{
    uint16_t rb = dock_row_bytes(s->width, s->fmt);
    bool wrapped = false;
    if (s->force_all) {
        /* Rehash everything lazily but send all rows now. */
        mark_all(s);
        s->force_all = false;
    }
    if (s->height == 0 || rb == 0)
        return true;
    while (max_rows--) {
        uint16_t y = s->scan_y;
        uint32_t h = s->hash(fb + (uint32_t)y * s->stride, rb);
        if (h != s->row_hash[y]) {
            s->row_hash[y] = h;
            s->dirty[y >> 3] |= (uint8_t)(1u << (y & 7u));
        }
        if (++s->scan_y >= s->height) {
            s->scan_y = 0;
            wrapped = true;
            break;
        }
    }
    return wrapped;
}

bool dock_scr_tx_any_dirty(const dock_scr_tx_t *s)
{
    uint16_t i;
    for (i = 0; i < (uint16_t)((s->height + 7u) / 8u); i++)
        if (s->dirty[i])
            return true;
    return false;
}

static bool is_dirty(const dock_scr_tx_t *s, uint16_t y)
{
    return (s->dirty[y >> 3] >> (y & 7u)) & 1u;
}

bool dock_scr_tx_next_band(dock_scr_tx_t *s, uint16_t max_rows, uint16_t *y0,
                           uint16_t *count)
{
    uint16_t y = 0, n = 0;
    /* Skip whole clean bytes quickly. */
    while (y < s->height) {
        if (s->dirty[y >> 3] == 0 && (y & 7u) == 0) {
            y = (uint16_t)(y + 8u);
            continue;
        }
        if (is_dirty(s, y))
            break;
        y++;
    }
    if (y >= s->height)
        return false;
    *y0 = y;
    while (y < s->height && n < max_rows && is_dirty(s, y)) {
        s->dirty[y >> 3] &= (uint8_t)~(1u << (y & 7u));
        y++;
        n++;
    }
    *count = n;
    return true;
}

/* ---------------- receiver ---------------- */

static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t expand4(uint32_t v) { return (v << 4) | v; }

uint32_t dock_color16_to_xrgb(uint16_t c, uint8_t fmt, uint8_t flags)
{
    uint32_t hi, mid, lo, r, b;
    switch (fmt) {
    case DOCK_PIX_RGB565:
        hi = expand5((c >> 11) & 0x1Fu);
        mid = expand6((c >> 5) & 0x3Fu);
        lo = expand5(c & 0x1Fu);
        break;
    case DOCK_PIX_RGB444:
        hi = expand4((c >> 8) & 0xFu);
        mid = expand4((c >> 4) & 0xFu);
        lo = expand4(c & 0xFu);
        break;
    default: /* 1555 for direct 16 bpp and for every palette entry */
        hi = expand5((c >> 10) & 0x1Fu);
        mid = expand5((c >> 5) & 0x1Fu);
        lo = expand5(c & 0x1Fu);
        break;
    }
    if (flags & DOCK_SCRF_BGR) {
        r = hi;
        b = lo;
    } else {
        r = lo;
        b = hi;
    }
    return 0xFF000000u | (r << 16) | (mid << 8) | b;
}

void dock_scr_rx_init(dock_scr_rx_t *r, uint32_t *fb, uint16_t fb_w,
                      uint16_t fb_h, uint32_t fb_stride_px)
{
    uint16_t i;
    memset(r, 0, sizeof *r);
    r->fb = fb;
    r->fb_w = fb_w;
    r->fb_h = fb_h;
    r->fb_stride = fb_stride_px;
    /* Sensible defaults: TI-OS mode and a grey ramp palette. */
    r->width = 320;
    r->height = 240;
    r->fmt = DOCK_PIX_RGB565;
    r->flags = DOCK_SCRF_BGR;
    for (i = 0; i < DOCK_PALETTE_MAX; i++)
        r->palette[i] = 0xFF000000u | ((uint32_t)i << 16) | ((uint32_t)i << 8) | i;
}

int dock_scr_rx_mode(dock_scr_rx_t *r, const uint8_t *p, size_t n)
{
    dock_rd_t rd;
    uint16_t w, h;
    uint8_t fmt, flags;
    dock_rd_init(&rd, p, n);
    w = dock_rd_u16(&rd);
    h = dock_rd_u16(&rd);
    fmt = dock_rd_u8(&rd);
    flags = dock_rd_u8(&rd);
    if (rd.underflow || fmt >= DOCK_PIX_COUNT || w == 0 || h == 0 ||
        w > DOCK_SCR_MAX_W || h > DOCK_SCR_MAX_H)
        return DOCK_E_INVALID;
    r->width = w;
    r->height = h;
    r->fmt = fmt;
    r->flags = flags;
    return DOCK_OK;
}

int dock_scr_rx_palette(dock_scr_rx_t *r, const uint8_t *p, size_t n)
{
    dock_rd_t rd;
    uint16_t first, count, i;
    dock_rd_init(&rd, p, n);
    first = dock_rd_u8(&rd);
    count = dock_rd_u16(&rd);
    if (rd.underflow || (uint32_t)first + count > DOCK_PALETTE_MAX ||
        dock_rd_left(&rd) < (size_t)count * 2u)
        return DOCK_E_INVALID;
    for (i = 0; i < count; i++)
        r->palette[first + i] =
            dock_color16_to_xrgb(dock_rd_u16(&rd), DOCK_PIX_RGB1555, r->flags);
    return DOCK_OK;
}

void dock_scr_convert_rows(const dock_scr_rx_t *r, const uint8_t *src,
                           uint16_t stride, uint16_t y0, uint16_t count)
{
    uint16_t y, x, w = r->width < r->fb_w ? r->width : r->fb_w;
    uint8_t bits = dock_pix_bits(r->fmt);
    for (y = 0; y < count; y++) {
        uint16_t dy = (uint16_t)(y0 + y);
        const uint8_t *s = src + (uint32_t)y * stride;
        uint32_t *d;
        if (dy >= r->fb_h)
            break;
        d = r->fb + (uint32_t)dy * r->fb_stride;
        if (bits == 16) {
            for (x = 0; x < w; x++)
                d[x] = dock_color16_to_xrgb(dock_get_u16(s + 2u * x), r->fmt, r->flags);
        } else if (bits == 8) {
            for (x = 0; x < w; x++)
                d[x] = r->palette[s[x]];
        } else {
            uint8_t per = (uint8_t)(8u / bits), mask = (uint8_t)((1u << bits) - 1u);
            for (x = 0; x < w; x++) {
                uint8_t byte = s[x / per];
                uint8_t slot = (uint8_t)(x % per);
                uint8_t shift = (r->flags & DOCK_SCRF_BEPO)
                                    ? (uint8_t)((per - 1u - slot) * bits)
                                    : (uint8_t)(slot * bits);
                d[x] = r->palette[(byte >> shift) & mask];
            }
        }
    }
}

int dock_scr_rx_rows(dock_scr_rx_t *r, const uint8_t *p, size_t n,
                     uint16_t *y0, uint16_t *count, bool *frame_end)
{
    dock_rd_t rd;
    uint16_t sy, cnt, stride, rb;
    uint8_t end;
    dock_rd_init(&rd, p, n);
    sy = dock_rd_u16(&rd);
    cnt = dock_rd_u16(&rd);
    stride = dock_rd_u16(&rd);
    end = dock_rd_u8(&rd);
    rb = dock_row_bytes(r->width, r->fmt);
    if (rd.underflow || stride < rb || (uint32_t)sy + cnt > r->height ||
        dock_rd_left(&rd) < (size_t)cnt * stride)
        return DOCK_E_INVALID;
    dock_scr_convert_rows(r, dock_rd_ptr(&rd), stride, sy, cnt);
    r->rows += cnt;
    if (end)
        r->frames++;
    if (y0)
        *y0 = sy;
    if (count)
        *count = cnt;
    if (frame_end)
        *frame_end = end != 0;
    return DOCK_OK;
}
