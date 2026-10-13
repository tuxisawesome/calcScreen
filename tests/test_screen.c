/* test_screen.c - dirty-row sender and XRGB receiver. */
#include "../core/dock_proto.h"
#include "../core/dock_screen.h"
#include "test.h"

/* ---- independent reference conversion ---- */

static uint32_t rep5(uint32_t v) { return (v * 255u + 15u) / 31u; }
static uint32_t rep6(uint32_t v) { return (v * 255u + 31u) / 63u; }

/* Bit replication equals rounding for 4 bits, and differs from rounding by at
 * most 1 for 5/6 bits; the core uses replication, check that exactly. */
static uint32_t ref_x5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t ref_x6(uint32_t v) { return (v << 2) | (v >> 4); }

static uint32_t ref_rgb(uint32_t msf, uint32_t mid, uint32_t lsf, uint8_t flags)
{
    uint32_t r = (flags & DOCK_SCRF_BGR) ? msf : lsf;
    uint32_t b = (flags & DOCK_SCRF_BGR) ? lsf : msf;
    return 0xFF000000u | (r << 16) | (mid << 8) | b;
}

static uint32_t ref16(uint16_t c, uint8_t fmt, uint8_t flags)
{
    switch (fmt) {
    case DOCK_PIX_RGB565:
        return ref_rgb(ref_x5((c >> 11) & 31u), ref_x6((c >> 5) & 63u),
                       ref_x5(c & 31u), flags);
    case DOCK_PIX_RGB444:
        return ref_rgb(((c >> 8) & 15u) * 17u, ((c >> 4) & 15u) * 17u,
                       (c & 15u) * 17u, flags);
    default:
        return ref_rgb(ref_x5((c >> 10) & 31u), ref_x5((c >> 5) & 31u),
                       ref_x5(c & 31u), flags);
    }
}

/* Pixel x of a raw row in any format, as a palette index or raw 16-bit. */
static uint32_t ref_pixel(const uint8_t *row, uint16_t x, uint8_t fmt,
                          uint8_t flags, const uint32_t *pal)
{
    uint8_t bits = dock_pix_bits(fmt);
    if (bits == 16)
        return ref16((uint16_t)(row[2u * x] | (row[2u * x + 1u] << 8)), fmt, flags);
    if (bits == 8)
        return pal[row[x]];
    {
        unsigned per = 8u / bits, slot = x % per;
        unsigned shift = (flags & DOCK_SCRF_BEPO) ? (per - 1u - slot) * bits : slot * bits;
        return pal[(row[x / per] >> shift) & ((1u << bits) - 1u)];
    }
}

/* ---- helpers to build message payloads ---- */

static size_t mode_payload(uint8_t *p, uint16_t w, uint16_t h, uint8_t fmt,
                           uint8_t flags)
{
    dock_put_u16(p, w);
    dock_put_u16(p + 2, h);
    p[4] = fmt;
    p[5] = flags;
    return 6;
}

static size_t rows_payload(uint8_t *p, uint16_t y0, uint16_t count,
                           uint16_t stride, uint8_t end, const uint8_t *rows)
{
    dock_put_u16(p, y0);
    dock_put_u16(p + 2, count);
    dock_put_u16(p + 4, stride);
    p[6] = end;
    memcpy(p + 7, rows, (size_t)count * stride);
    return 7u + (size_t)count * stride;
}

/* ---- sender ---- */

static uint8_t fb565[240 * 640];

static void fill_random(uint8_t *p, size_t n, uint32_t seed)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)t_rand(&seed);
}

static void t_formats(void)
{
    CHECK_EQ(dock_pix_bits(DOCK_PIX_I1), 1);
    CHECK_EQ(dock_pix_bits(DOCK_PIX_RGB444), 16);
    CHECK_EQ(dock_pix_bits(DOCK_PIX_COUNT), 0);
    CHECK_EQ(dock_row_bytes(320, DOCK_PIX_I1), 40);
    CHECK_EQ(dock_row_bytes(320, DOCK_PIX_I2), 80);
    CHECK_EQ(dock_row_bytes(320, DOCK_PIX_I4), 160);
    CHECK_EQ(dock_row_bytes(320, DOCK_PIX_I8), 320);
    CHECK_EQ(dock_row_bytes(320, DOCK_PIX_RGB565), 640);
    CHECK_EQ(dock_row_bytes(13, DOCK_PIX_I1), 2);
    CHECK_EQ(dock_row_bytes(320, 99), 0);
    CHECK_EQ(dock_palette_size(DOCK_PIX_I1), 2);
    CHECK_EQ(dock_palette_size(DOCK_PIX_I8), 256);
    CHECK_EQ(dock_palette_size(DOCK_PIX_RGB565), 0);
}

static void t_sender_dirty_bands(void)
{
    static dock_scr_tx_t s;
    uint16_t y0 = 0, cnt = 0;
    fill_random(fb565, sizeof fb565, 1);
    dock_scr_tx_init(&s, NULL);
    dock_scr_tx_set_mode(&s, 320, 240, 640, DOCK_PIX_RGB565, DOCK_SCRF_BGR);
    CHECK(s.mode_dirty);
    CHECK(!s.pal_dirty);
    /* The first pass sends everything. */
    CHECK(!dock_scr_tx_scan(&s, fb565, 100));
    CHECK(!dock_scr_tx_scan(&s, fb565, 100));
    CHECK(dock_scr_tx_scan(&s, fb565, 100)); /* wrapped */
    CHECK(dock_scr_tx_next_band(&s, 240, &y0, &cnt));
    CHECK_EQ(y0, 0);
    CHECK_EQ(cnt, 240);
    CHECK(!dock_scr_tx_any_dirty(&s));
    CHECK(!dock_scr_tx_next_band(&s, 240, &y0, &cnt));

    /* Nothing changed: nothing dirty. */
    CHECK(dock_scr_tx_scan(&s, fb565, 240));
    CHECK(!dock_scr_tx_any_dirty(&s));

    /* Change rows 10..12, 100 and 239. */
    fb565[10 * 640 + 5] ^= 0xFF;
    fb565[11 * 640 + 639] ^= 0x01;
    fb565[12 * 640] ^= 0x80;
    fb565[100 * 640 + 300] ^= 0x10;
    fb565[239 * 640 + 1] ^= 0x01;
    CHECK(dock_scr_tx_scan(&s, fb565, 240));
    CHECK(dock_scr_tx_any_dirty(&s));
    CHECK(dock_scr_tx_next_band(&s, 2, &y0, &cnt));
    CHECK_EQ(y0, 10);
    CHECK_EQ(cnt, 2);
    CHECK(dock_scr_tx_next_band(&s, 2, &y0, &cnt));
    CHECK_EQ(y0, 12);
    CHECK_EQ(cnt, 1);
    CHECK(dock_scr_tx_next_band(&s, 240, &y0, &cnt));
    CHECK_EQ(y0, 100);
    CHECK_EQ(cnt, 1);
    CHECK(dock_scr_tx_next_band(&s, 240, &y0, &cnt));
    CHECK_EQ(y0, 239);
    CHECK_EQ(cnt, 1);
    CHECK(!dock_scr_tx_next_band(&s, 240, &y0, &cnt));

    /* Swapping two bytes inside a row must still be detected
     * (position-dependent hash). */
    {
        uint8_t t = fb565[50 * 640 + 0];
        fb565[50 * 640 + 0] = fb565[50 * 640 + 1];
        fb565[50 * 640 + 1] = t;
        if (fb565[50 * 640 + 0] == fb565[50 * 640 + 1])
            fb565[50 * 640 + 0] ^= 1;
    }
    CHECK(dock_scr_tx_scan(&s, fb565, 240));
    CHECK(dock_scr_tx_next_band(&s, 240, &y0, &cnt));
    CHECK_EQ(y0, 50);

    /* Refresh marks everything again. */
    dock_scr_tx_refresh(&s);
    CHECK(s.mode_dirty);
    dock_scr_tx_scan(&s, fb565, 1);
    CHECK(dock_scr_tx_next_band(&s, 240, &y0, &cnt));
    CHECK_EQ(y0, 0);
    CHECK_EQ(cnt, 240);

    /* A mode change forces a full resend and a palette for indexed modes. */
    dock_scr_tx_set_mode(&s, 320, 240, 40, DOCK_PIX_I1, 0);
    CHECK(s.mode_dirty);
    CHECK(s.pal_dirty);
    {
        static const uint8_t pal[4] = {0, 0, 0xFF, 0x7F};
        s.pal_dirty = false;
        dock_scr_tx_check_palette(&s, pal, 2);
        CHECK(s.pal_dirty);
        s.pal_dirty = false;
        dock_scr_tx_check_palette(&s, pal, 2);
        CHECK(!s.pal_dirty);
    }
}

/* ---- receiver ---- */

static uint32_t out[16 * 4];
static dock_scr_rx_t rx;

static void rx_setup(uint16_t w, uint16_t h, uint8_t fmt, uint8_t flags)
{
    uint8_t p[8];
    memset(out, 0, sizeof out);
    dock_scr_rx_init(&rx, out, 16, 4, 16);
    CHECK_EQ(dock_scr_rx_mode(&rx, p, mode_payload(p, w, h, fmt, flags)), DOCK_OK);
}

static void set_palette(const uint16_t *entries, uint16_t n)
{
    uint8_t p[3 + 512];
    uint16_t i;
    p[0] = 0;
    dock_put_u16(p + 1, n);
    for (i = 0; i < n; i++)
        dock_put_u16(p + 3 + 2u * i, entries[i]);
    CHECK_EQ(dock_scr_rx_palette(&rx, p, 3u + 2u * n), DOCK_OK);
}

static void apply_row(const uint8_t *row, uint16_t stride)
{
    uint8_t p[64];
    uint16_t y0 = 99, cnt = 99;
    bool end = false;
    CHECK_EQ(dock_scr_rx_rows(&rx, p, rows_payload(p, 0, 1, stride, 1, row),
                              &y0, &cnt, &end), DOCK_OK);
    CHECK_EQ(y0, 0);
    CHECK_EQ(cnt, 1);
    CHECK(end);
}

static void t_rx_direct(void)
{
    static const struct {
        uint8_t fmt, flags;
        uint16_t raw;
        uint32_t want;
    } v[] = {
        /* RGB565: BGR flag = red in the top field */
        {DOCK_PIX_RGB565, DOCK_SCRF_BGR, 0xF800, 0xFFFF0000u},
        {DOCK_PIX_RGB565, 0, 0xF800, 0xFF0000FFu},
        {DOCK_PIX_RGB565, DOCK_SCRF_BGR, 0x07E0, 0xFF00FF00u},
        {DOCK_PIX_RGB565, DOCK_SCRF_BGR, 0x001F, 0xFF0000FFu},
        {DOCK_PIX_RGB565, 0, 0x001F, 0xFFFF0000u},
        {DOCK_PIX_RGB565, DOCK_SCRF_BGR, 0x8410, 0xFF848284u},
        {DOCK_PIX_RGB565, DOCK_SCRF_BGR, 0xFFFF, 0xFFFFFFFFu},
        {DOCK_PIX_RGB565, 0, 0x0000, 0xFF000000u},
        /* RGB1555: bit 15 (intensity) ignored */
        {DOCK_PIX_RGB1555, DOCK_SCRF_BGR, 0x7C00, 0xFFFF0000u},
        {DOCK_PIX_RGB1555, DOCK_SCRF_BGR, 0xFC00, 0xFFFF0000u},
        {DOCK_PIX_RGB1555, 0, 0x7C00, 0xFF0000FFu},
        {DOCK_PIX_RGB1555, 0, 0x03E0, 0xFF00FF00u},
        {DOCK_PIX_RGB1555, DOCK_SCRF_BGR, 0x4210, 0xFF848484u},
        /* RGB444 in the low 12 bits */
        {DOCK_PIX_RGB444, DOCK_SCRF_BGR, 0x0F00, 0xFFFF0000u},
        {DOCK_PIX_RGB444, 0, 0x0F00, 0xFF0000FFu},
        {DOCK_PIX_RGB444, DOCK_SCRF_BGR, 0x0080, 0xFF008800u},
        {DOCK_PIX_RGB444, 0, 0xF00A, 0xFFAA0000u},
    };
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t row[32];
        uint16_t x;
        rx_setup(16, 4, v[i].fmt, v[i].flags);
        for (x = 0; x < 16; x++)
            dock_put_u16(row + 2u * x, x == 3 ? v[i].raw : 0);
        apply_row(row, 32);
        CHECK_EQ(out[3], v[i].want);
        CHECK_EQ(out[3], ref16(v[i].raw, v[i].fmt, v[i].flags));
        CHECK_EQ(dock_color16_to_xrgb(v[i].raw, v[i].fmt, v[i].flags), v[i].want);
        CHECK_EQ(out[16], 0); /* row 1 untouched */
    }
    /* replication vs rounding sanity (documents the expansion choice) */
    CHECK_EQ(ref_x5(31), rep5(31));
    CHECK_EQ(ref_x6(63), rep6(63));
}

static const uint16_t pal16[16] = {
    0x0000, 0x7C00, 0x03E0, 0x001F, 0x7FFF, 0x4210, 0x7C1F, 0x03FF,
    0x1084, 0x2108, 0x318C, 0x4631, 0x56B5, 0x6739, 0x77BD, 0x0421};

static uint32_t palx(uint8_t i, uint8_t flags)
{
    return ref16(pal16[i], DOCK_PIX_RGB1555, flags);
}

static void t_rx_indexed(void)
{
    uint8_t flags_list[4] = {0, DOCK_SCRF_BGR, DOCK_SCRF_BEPO,
                             DOCK_SCRF_BGR | DOCK_SCRF_BEPO};
    size_t fi;
    for (fi = 0; fi < 4; fi++) {
        uint8_t fl = flags_list[fi];
        bool bepo = (fl & DOCK_SCRF_BEPO) != 0;
        uint8_t row[16];
        uint16_t x;

        /* I1: byte 0x01, 0x80 */
        rx_setup(16, 4, DOCK_PIX_I1, fl);
        set_palette(pal16, 2);
        row[0] = 0x01;
        row[1] = 0x80;
        apply_row(row, 2);
        for (x = 0; x < 16; x++) {
            uint8_t idx;
            if (bepo)
                idx = (uint8_t)((x == 7 || x == 8) ? 1 : 0);
            else
                idx = (uint8_t)((x == 0 || x == 15) ? 1 : 0);
            CHECK_EQ(out[x], palx(idx, fl));
        }

        /* I2: 0xE4 = 11 10 01 00 */
        rx_setup(16, 4, DOCK_PIX_I2, fl);
        set_palette(pal16, 4);
        memset(row, 0, sizeof row);
        row[0] = 0xE4;
        row[3] = 0x1B; /* 00 01 10 11 */
        apply_row(row, 4);
        for (x = 0; x < 4; x++)
            CHECK_EQ(out[x], palx((uint8_t)(bepo ? 3 - x : x), fl));
        for (x = 0; x < 4; x++)
            CHECK_EQ(out[12 + x], palx((uint8_t)(bepo ? x : 3 - x), fl));
        CHECK_EQ(out[5], palx(0, fl));

        /* I4: 0x21 0x43 ... */
        rx_setup(16, 4, DOCK_PIX_I4, fl);
        set_palette(pal16, 16);
        for (x = 0; x < 8; x++)
            row[x] = (uint8_t)(((2u * x + 1u) << 4) | (2u * x));
        apply_row(row, 8);
        for (x = 0; x < 16; x++) {
            uint8_t want = (uint8_t)(bepo ? (x ^ 1u) : x);
            CHECK_EQ(out[x], palx(want, fl));
            CHECK_EQ(out[x], ref_pixel(row, x, DOCK_PIX_I4, fl, rx.palette));
        }

        /* I8 */
        rx_setup(16, 4, DOCK_PIX_I8, fl);
        set_palette(pal16, 16);
        for (x = 0; x < 16; x++)
            row[x] = (uint8_t)(15 - x);
        apply_row(row, 16);
        for (x = 0; x < 16; x++)
            CHECK_EQ(out[x], palx((uint8_t)(15 - x), fl));
    }
}

static void t_rx_invalid(void)
{
    uint8_t p[600];
    uint8_t row[32] = {0};
    rx_setup(16, 4, DOCK_PIX_RGB565, 0);
    /* bad modes */
    CHECK_EQ(dock_scr_rx_mode(&rx, p, mode_payload(p, 16, 4, DOCK_PIX_COUNT, 0)),
             DOCK_E_INVALID);
    CHECK_EQ(dock_scr_rx_mode(&rx, p, mode_payload(p, 0, 4, DOCK_PIX_I1, 0)),
             DOCK_E_INVALID);
    CHECK_EQ(dock_scr_rx_mode(&rx, p, mode_payload(p, 321, 4, DOCK_PIX_I1, 0)),
             DOCK_E_INVALID);
    CHECK_EQ(dock_scr_rx_mode(&rx, p, 5), DOCK_E_INVALID);
    CHECK_EQ(rx.fmt, DOCK_PIX_RGB565); /* unchanged */
    /* rows beyond the height */
    CHECK_EQ(dock_scr_rx_rows(&rx, p, rows_payload(p, 4, 1, 32, 0, row), NULL,
                              NULL, NULL), DOCK_E_INVALID);
    CHECK_EQ(dock_scr_rx_rows(&rx, p, rows_payload(p, 3, 2, 32, 0, row), NULL,
                              NULL, NULL), DOCK_E_INVALID);
    /* stride smaller than a row */
    CHECK_EQ(dock_scr_rx_rows(&rx, p, rows_payload(p, 0, 1, 31, 0, row), NULL,
                              NULL, NULL), DOCK_E_INVALID);
    /* payload shorter than announced */
    CHECK_EQ(dock_scr_rx_rows(&rx, p, rows_payload(p, 0, 1, 32, 0, row) - 1,
                              NULL, NULL, NULL), DOCK_E_INVALID);
    /* palette overflow */
    p[0] = 250;
    dock_put_u16(p + 1, 7);
    CHECK_EQ(dock_scr_rx_palette(&rx, p, 3 + 14), DOCK_E_INVALID);
    p[0] = 0;
    dock_put_u16(p + 1, 4);
    CHECK_EQ(dock_scr_rx_palette(&rx, p, 3 + 7), DOCK_E_INVALID);
    /* destination smaller than the source: clipped, no overflow */
    {
        static uint32_t small[8 * 2];
        static uint8_t big[64 * 4 * 2];
        dock_scr_rx_t r2;
        dock_scr_rx_init(&r2, small, 8, 2, 8);
        CHECK_EQ(dock_scr_rx_mode(&r2, p, mode_payload(p, 64, 4, DOCK_PIX_RGB565, 0)),
                 DOCK_OK);
        memset(big, 0xFF, sizeof big);
        CHECK_EQ(dock_scr_rx_rows(&r2, p, rows_payload(p, 0, 4, 128, 1, big), NULL,
                                  NULL, NULL), DOCK_OK);
        CHECK_EQ(small[15], 0xFFFFFFFFu);
    }
}

/* ---- sender -> receiver round trip ---- */

static uint8_t rt_fb[240 * 640];
static uint32_t rt_out[320 * 240];
static uint32_t rt_want[320 * 240];
static uint8_t rt_msg[7 + 240 * 640];

static void rt_expect(uint16_t w, uint16_t h, uint16_t stride, uint8_t fmt,
                      uint8_t flags, const uint32_t *pal)
{
    uint16_t x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            rt_want[(uint32_t)y * 320 + x] =
                ref_pixel(rt_fb + (uint32_t)y * stride, x, fmt, flags, pal);
}

/* One sender pass: scan everything, send every band as SCR_ROWS. */
static unsigned rt_pass(dock_scr_tx_t *s, dock_scr_rx_t *r, uint16_t band_max)
{
    uint16_t y0, cnt;
    unsigned bands = 0;
    uint16_t rb = dock_row_bytes(s->width, s->fmt);
    while (!dock_scr_tx_scan(s, rt_fb, 7))
        ;
    while (dock_scr_tx_next_band(s, band_max, &y0, &cnt)) {
        size_t n = rows_payload(rt_msg, y0, cnt, rb, 0,
                                rt_fb + (uint32_t)y0 * s->stride);
        CHECK_EQ(dock_scr_rx_rows(r, rt_msg, n, NULL, NULL, NULL), DOCK_OK);
        bands++;
    }
    return bands;
}

static void t_roundtrip(void)
{
    static const struct {
        uint8_t fmt, flags;
    } modes[] = {
        {DOCK_PIX_RGB565, DOCK_SCRF_BGR}, {DOCK_PIX_RGB565, 0},
        {DOCK_PIX_RGB1555, 0},           {DOCK_PIX_RGB444, DOCK_SCRF_BGR},
        {DOCK_PIX_I8, DOCK_SCRF_BGR},     {DOCK_PIX_I4, 0},
        {DOCK_PIX_I2, DOCK_SCRF_BEPO},    {DOCK_PIX_I1, DOCK_SCRF_BEPO},
        {DOCK_PIX_I1, 0},
    };
    size_t m;
    for (m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        static dock_scr_tx_t s;
        dock_scr_rx_t r;
        uint8_t fmt = modes[m].fmt, fl = modes[m].flags, p[600];
        uint16_t rb = dock_row_bytes(320, fmt), i;
        uint32_t seed = (uint32_t)m + 7u;
        unsigned bands;

        fill_random(rt_fb, (size_t)rb * 240u, seed);
        memset(rt_out, 0, sizeof rt_out);
        dock_scr_rx_init(&r, rt_out, 320, 240, 320);
        dock_scr_tx_init(&s, NULL);
        dock_scr_tx_set_mode(&s, 320, 240, rb, fmt, fl);
        CHECK_EQ(dock_scr_rx_mode(&r, p, mode_payload(p, 320, 240, fmt, fl)), DOCK_OK);
        if (dock_palette_size(fmt)) {
            uint16_t n = dock_palette_size(fmt);
            p[0] = 0;
            dock_put_u16(p + 1, n);
            for (i = 0; i < n; i++)
                dock_put_u16(p + 3 + 2u * i, (uint16_t)(i * 0x0421u + 0x1234u));
            CHECK_EQ(dock_scr_rx_palette(&r, p, 3u + 2u * n), DOCK_OK);
        }
        bands = rt_pass(&s, &r, 37);
        CHECK_EQ(bands, (240 + 36) / 37);
        rt_expect(320, 240, rb, fmt, fl, r.palette);
        CHECK_MEM(rt_out, rt_want, sizeof rt_want);

        /* Modify a few rows; only those are resent. */
        rt_fb[(uint32_t)5 * rb] ^= 0x5A;
        rt_fb[(uint32_t)6 * rb + rb - 1u] ^= 0x01;
        rt_fb[(uint32_t)180 * rb + rb / 2u] ^= 0x80;
        bands = rt_pass(&s, &r, 240);
        CHECK_EQ(bands, 2);
        rt_expect(320, 240, rb, fmt, fl, r.palette);
        CHECK_MEM(rt_out, rt_want, sizeof rt_want);
    }
}

void suite_screen(void)
{
    RUN(t_formats);
    RUN(t_sender_dirty_bands);
    RUN(t_rx_direct);
    RUN(t_rx_indexed);
    RUN(t_rx_invalid);
    RUN(t_roundtrip);
}
