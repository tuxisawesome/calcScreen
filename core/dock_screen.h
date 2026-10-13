/*
 * dock_screen.h - screen mirroring codec.
 *
 * Sender side (calculator): keeps one hash per framebuffer row and turns
 * changed rows into contiguous bands. Rows are contiguous in memory on the CE
 * (and on most MCU framebuffers), so each band is sent as one zero-copy tail.
 * Scanning is incremental (a few rows per call) so a caller running inside an
 * OS hook never stalls the OS for long.
 *
 * Receiver side (dock): converts rows of any supported pixel format into an
 * XRGB8888 framebuffer.
 */
#ifndef DOCK_SCREEN_H
#define DOCK_SCREEN_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel formats (mirror the PL111-style LCD modes of the TI-84 Plus CE). */
enum dock_pix_fmt {
    DOCK_PIX_I1 = 0,      /* 1 bpp palette indexed */
    DOCK_PIX_I2 = 1,      /* 2 bpp palette indexed */
    DOCK_PIX_I4 = 2,      /* 4 bpp palette indexed */
    DOCK_PIX_I8 = 3,      /* 8 bpp palette indexed */
    DOCK_PIX_RGB1555 = 4, /* 16 bpp, bit 15 = intensity */
    DOCK_PIX_RGB565 = 5,  /* 16 bpp (TI-OS default) */
    DOCK_PIX_RGB444 = 6,  /* 12 bpp stored in 16 bits */
    DOCK_PIX_COUNT
};

/* SCR_MODE flags */
#define DOCK_SCRF_BGR 0x01u  /* red is in the most significant field */
#define DOCK_SCRF_BEPO 0x02u /* sub-byte pixels: first pixel in the MSBs */

#define DOCK_SCR_MAX_W 320u
#define DOCK_SCR_MAX_H 240u
#define DOCK_PALETTE_MAX 256u

uint8_t dock_pix_bits(uint8_t fmt);
/* Bytes per row for a given width/format (0 for an unknown format). */
uint16_t dock_row_bytes(uint16_t width, uint8_t fmt);
/* Number of palette entries a format uses (0 for direct colour). */
uint16_t dock_palette_size(uint8_t fmt);

/* Default row hash (Fletcher-style, position dependent). Platforms may supply
 * a faster one; the values never leave the sender. */
uint32_t dock_row_hash_c(const uint8_t *row, size_t n);

/* ---------------- sender ---------------- */

typedef uint32_t (*dock_hash_fn)(const uint8_t *row, size_t n);

typedef struct dock_scr_tx {
    /* configuration */
    dock_hash_fn hash;
    /* current mode */
    uint16_t width, height, stride;
    uint8_t fmt, flags;
    /* state */
    uint32_t row_hash[DOCK_SCR_MAX_H];
    uint8_t dirty[DOCK_SCR_MAX_H / 8];
    uint16_t scan_y;        /* next row to hash */
    uint32_t pal_hash;
    bool mode_dirty;
    bool pal_dirty;
    bool force_all;         /* resend everything on the next pass */
} dock_scr_tx_t;

void dock_scr_tx_init(dock_scr_tx_t *s, dock_hash_fn hash);

/*
 * Tell the sender what the framebuffer looks like right now. Changes of
 * geometry/format mark everything dirty and raise mode_dirty.
 */
void dock_scr_tx_set_mode(dock_scr_tx_t *s, uint16_t width, uint16_t height,
                          uint16_t stride, uint8_t fmt, uint8_t flags);

/* Check whether the palette changed (n little-endian 16-bit entries). */
void dock_scr_tx_check_palette(dock_scr_tx_t *s, const uint8_t *pal_le, uint16_t n);

/* Mark every row dirty (and resend mode + palette). */
void dock_scr_tx_refresh(dock_scr_tx_t *s);

/*
 * Hash up to `max_rows` rows starting at the scan cursor, marking changed ones
 * dirty. Returns true when the cursor wrapped (a full pass finished).
 */
bool dock_scr_tx_scan(dock_scr_tx_t *s, const uint8_t *fb, uint16_t max_rows);

/*
 * Pop the next dirty band (at most max_rows rows). Returns false if nothing is
 * dirty. The band's rows are cleared from the dirty set.
 */
bool dock_scr_tx_next_band(dock_scr_tx_t *s, uint16_t max_rows, uint16_t *y0,
                           uint16_t *count);

bool dock_scr_tx_any_dirty(const dock_scr_tx_t *s);

/* ---------------- receiver ---------------- */

typedef struct dock_scr_rx {
    uint16_t width, height;
    uint8_t fmt, flags;
    uint32_t palette[DOCK_PALETTE_MAX]; /* XRGB8888 */
    uint32_t *fb;                       /* destination XRGB8888 */
    uint32_t fb_stride;                 /* in pixels */
    uint16_t fb_w, fb_h;                /* destination capacity */
    uint32_t frames;                    /* frame_end markers seen */
    uint32_t rows;                      /* rows applied */
} dock_scr_rx_t;

void dock_scr_rx_init(dock_scr_rx_t *r, uint32_t *fb, uint16_t fb_w,
                      uint16_t fb_h, uint32_t fb_stride_px);

/* Convert one raw 16-bit colour of the current mode/palette format. */
uint32_t dock_color16_to_xrgb(uint16_t c, uint8_t fmt, uint8_t flags);

/* Apply message payloads. Return DOCK_OK or DOCK_E_INVALID. */
int dock_scr_rx_mode(dock_scr_rx_t *r, const uint8_t *p, size_t n);
int dock_scr_rx_palette(dock_scr_rx_t *r, const uint8_t *p, size_t n);
/* p/n is the whole SCR_ROWS payload; y0/count report the rows touched. */
int dock_scr_rx_rows(dock_scr_rx_t *r, const uint8_t *p, size_t n,
                     uint16_t *y0, uint16_t *count, bool *frame_end);

/* Convert raw rows in an arbitrary buffer (used by DUSB screenshots too). */
void dock_scr_convert_rows(const dock_scr_rx_t *r, const uint8_t *src,
                           uint16_t stride, uint16_t y0, uint16_t count);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_SCREEN_H */
