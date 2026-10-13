/*
 * bav.h - "BAV" raw video format for the calcScreen player.
 *
 * Deliberately trivial so neither the dock nor the calculator decodes
 * anything: every frame is stored exactly as the TI-84 Plus CE LCD wants it in
 * the chosen pixel format, and the calculator DMA-reads it straight into VRAM.
 * The dock then mirrors whatever LCD mode the calculator set, so the same
 * player handles black-and-white and colour videos alike.
 *
 *   offset size  field
 *   0      4     magic "BAV1"
 *   4      2     width            (<= 320)
 *   6      2     height           (<= 240)
 *   8      1     bpp              bits per pixel (1,2,4,8,16)
 *   9      1     flags            BAV_F_LSB_FIRST: for sub-byte formats, pixel 0
 *                                 is in the least-significant bits of byte 0
 *   10     1     fmt              bav_fmt / dock_pix_fmt (0=I1..6=RGB444)
 *   11     1     reserved (0)
 *   12     4     fps_milli        frames per 1000 s (30000 = 30 fps)
 *   16     4     frame_count
 *   20     4     frame_bytes      width*height*bpp/8
 *   24     4     data_offset      offset of frame 0 (>= 32 + palette_count*2)
 *   28     2     palette_count    palette entries (0 for direct-colour formats)
 *   30     2     reserved (0)
 *   32     ...   palette          palette_count * u16 LE, RGB1555 (bit15 unused)
 *   data_offset + i*frame_bytes   frame i
 *
 * A 1-bpp file (fmt 0, no palette) is byte-identical to the original format.
 * Audio lives in a separate PCM WAV file played by the dock.
 */
#ifndef DOCK_BAV_H
#define DOCK_BAV_H

#include "dock_common.h"

#define BAV_MAGIC "BAV1"
#define BAV_HEADER_LEN 32u
#define BAV_F_LSB_FIRST 0x01u
#define BAV_PALETTE_MAX 256u

/* Pixel formats (same numbering as enum dock_pix_fmt in dock_screen.h; kept
 * here as plain constants so bav.h depends only on dock_common.h). */
enum bav_fmt {
    BAV_FMT_I1 = 0,      /* 1 bpp, palette indexed (black/white by default) */
    BAV_FMT_I2 = 1,      /* 2 bpp, palette indexed */
    BAV_FMT_I4 = 2,      /* 4 bpp, palette indexed (16 colours) */
    BAV_FMT_I8 = 3,      /* 8 bpp, palette indexed (256 colours) */
    BAV_FMT_RGB1555 = 4, /* 16 bpp direct, bit 15 ignored */
    BAV_FMT_RGB565 = 5,  /* 16 bpp direct (most colour; TI-OS native order) */
    BAV_FMT_RGB444 = 6,  /* 12 bpp direct, stored in 16 bits */
    BAV_FMT_COUNT
};

/* Bits per pixel for a format (0 for an unknown format). */
static inline uint8_t bav_bpp_for_fmt(uint8_t fmt)
{
    switch (fmt) {
    case BAV_FMT_I1: return 1;
    case BAV_FMT_I2: return 2;
    case BAV_FMT_I4: return 4;
    case BAV_FMT_I8: return 8;
    case BAV_FMT_RGB1555:
    case BAV_FMT_RGB565:
    case BAV_FMT_RGB444: return 16;
    default: return 0;
    }
}

/* Palette entries a format uses (0 for direct colour). */
static inline uint16_t bav_palette_for_fmt(uint8_t fmt)
{
    return fmt <= BAV_FMT_I8 ? (uint16_t)(1u << bav_bpp_for_fmt(fmt)) : 0u;
}

typedef struct bav_header {
    uint16_t width, height;
    uint8_t bpp, flags, fmt;
    uint32_t fps_milli;
    uint32_t frame_count;
    uint32_t frame_bytes;
    uint32_t data_offset;
    uint16_t palette_count;
} bav_header_t;

static inline void bav_header_write(uint8_t out[BAV_HEADER_LEN], const bav_header_t *h)
{
    memset(out, 0, BAV_HEADER_LEN);
    memcpy(out, BAV_MAGIC, 4);
    dock_put_u16(out + 4, h->width);
    dock_put_u16(out + 6, h->height);
    out[8] = h->bpp;
    out[9] = h->flags;
    out[10] = h->fmt;
    dock_put_u32(out + 12, h->fps_milli);
    dock_put_u32(out + 16, h->frame_count);
    dock_put_u32(out + 20, h->frame_bytes);
    dock_put_u32(out + 24, h->data_offset);
    dock_put_u16(out + 28, h->palette_count);
}

/* Returns false if the header is not a valid BAV header. */
static inline bool bav_header_read(const uint8_t in[BAV_HEADER_LEN], bav_header_t *h)
{
    if (memcmp(in, BAV_MAGIC, 4) != 0)
        return false;
    h->width = dock_get_u16(in + 4);
    h->height = dock_get_u16(in + 6);
    h->bpp = in[8];
    h->flags = in[9];
    h->fmt = in[10];
    h->fps_milli = dock_get_u32(in + 12);
    h->frame_count = dock_get_u32(in + 16);
    h->frame_bytes = dock_get_u32(in + 20);
    h->data_offset = dock_get_u32(in + 24);
    h->palette_count = dock_get_u16(in + 28);
    return h->width && h->height && h->fps_milli &&
           h->fmt < BAV_FMT_COUNT && h->bpp == bav_bpp_for_fmt(h->fmt) &&
           h->palette_count <= bav_palette_for_fmt(h->fmt) &&
           h->data_offset >= BAV_HEADER_LEN + (uint32_t)h->palette_count * 2u &&
           h->frame_bytes == (uint32_t)h->width * h->height * h->bpp / 8u;
}

#endif /* DOCK_BAV_H */
