/*
 * bav.h - "BAV" raw video format used by the Bad Apple demo.
 *
 * Deliberately trivial so neither the dock nor the calculator decodes
 * anything: frames are stored exactly as the TI-84 Plus CE LCD wants them in
 * 1 bpp mode, and the calculator DMA-reads each one straight into VRAM.
 *
 *   offset size  field
 *   0      4     magic "BAV1"
 *   4      2     width            (320)
 *   6      2     height           (240)
 *   8      1     bpp              (1)
 *   9      1     flags            BAV_F_LSB_FIRST: pixel 0 is bit 0 of byte 0
 *   10     2     reserved (0)
 *   12     4     fps_milli        frames per 1000 s (30000 = 30 fps)
 *   16     4     frame_count
 *   20     4     frame_bytes      width*height*bpp/8
 *   24     4     data_offset      offset of frame 0 (>= 32)
 *   28     4     reserved (0)
 *   data_offset + i*frame_bytes   frame i; bit value 1 = white, 0 = black
 *
 * Audio lives in a separate PCM WAV file played by the dock.
 */
#ifndef DOCK_BAV_H
#define DOCK_BAV_H

#include "dock_common.h"

#define BAV_MAGIC "BAV1"
#define BAV_HEADER_LEN 32u
#define BAV_F_LSB_FIRST 0x01u

typedef struct bav_header {
    uint16_t width, height;
    uint8_t bpp, flags;
    uint32_t fps_milli;
    uint32_t frame_count;
    uint32_t frame_bytes;
    uint32_t data_offset;
} bav_header_t;

static inline void bav_header_write(uint8_t out[BAV_HEADER_LEN], const bav_header_t *h)
{
    memset(out, 0, BAV_HEADER_LEN);
    memcpy(out, BAV_MAGIC, 4);
    dock_put_u16(out + 4, h->width);
    dock_put_u16(out + 6, h->height);
    out[8] = h->bpp;
    out[9] = h->flags;
    dock_put_u32(out + 12, h->fps_milli);
    dock_put_u32(out + 16, h->frame_count);
    dock_put_u32(out + 20, h->frame_bytes);
    dock_put_u32(out + 24, h->data_offset);
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
    h->fps_milli = dock_get_u32(in + 12);
    h->frame_count = dock_get_u32(in + 16);
    h->frame_bytes = dock_get_u32(in + 20);
    h->data_offset = dock_get_u32(in + 24);
    return h->width && h->height && h->bpp && h->fps_milli &&
           h->data_offset >= BAV_HEADER_LEN &&
           h->frame_bytes == (uint32_t)h->width * h->height * h->bpp / 8u;
}

#endif /* DOCK_BAV_H */
