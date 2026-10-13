/*
 * BADAPPLE - the calcScreen video player (and end-to-end test).
 *
 * Plays a pre-converted video (badapple.bav) from the dock's USB drive by
 * DMA-reading each frame straight into VRAM, and has the dock play the
 * soundtrack (badapple.wav). The dock mirrors the LCD to the monitor, so the
 * video and sound come out of the Pi while the calculator does the work.
 *
 * The file carries its own pixel format, so this plays black-and-white *and*
 * colour videos: the player switches the LCD to the format in the header
 * (1/2/4/8 bpp palettized, or 16 bpp RGB565/RGB1555/RGB444 direct), loads the
 * palette, and streams. The dock reports the live LCD mode and converts it, so
 * nothing here is specific to Bad Apple. Any .bav the converter produces plays.
 *
 * Video is slaved to the audio clock reported by the dock, so they stay in
 * sync. Controls: CLEAR or ON quits, ENTER pauses.
 */
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <keypadc.h>
#include <sys/lcd.h>
#include <sys/timers.h>
#include <ti/getcsc.h>
#include <ti/screen.h>
#include <tice.h>

#include "dock.h"
#include "../../../core/bav.h"

#define VIDEO_FILE "badapple.bav"
#define AUDIO_FILE "badapple.wav"
#define LCD_W 320u
#define LCD_H 240u
#define VRAM_BYTES (LCD_W * LCD_H * 2u) /* one 16-bpp frame = the LCD's RAM */

/* PL111 LCD control register fields (sys/lcd.h, lcd_Control at 0xE30018).
 * bits [3:1] = LcdBpp code, bit 8 = BGR, bit 10 = BEPO (sub-byte pixel order). */
#define LCD_BPP_SHIFT 1u
#define LCD_BPP_MASK (0x7u << LCD_BPP_SHIFT)
#define LCD_BGR 0x100u
#define LCD_BEPO 0x400u

static uint8_t *const vram = (uint8_t *)0xD40000;

static void message(const char *line1, const char *line2)
{
    os_ClrHome();
    os_PutStrFull(line1);
    os_NewLine();
    if (line2)
        os_PutStrFull(line2);
    while (!os_GetCSC())
        ;
}

/* frame = pos_ms * fps_milli / 1e6 without overflowing 32 bits. */
static uint32_t frame_at(uint32_t pos_ms, uint32_t fps_milli)
{
    uint32_t sec = pos_ms / 1000u, ms = pos_ms % 1000u;
    return sec * fps_milli / 1000u + ms * fps_milli / 1000000u;
}

/* Local fallback clock when the dock cannot play audio. */
static uint32_t clock_ms(void)
{
    return (uint32_t)((uint64_t)clock() * 1000u / CLOCKS_PER_SEC);
}

/* The PL111 LcdBpp code for a BAV pixel format (bits [3:1] of lcd_Control). */
static uint8_t lcd_bpp_code(uint8_t fmt)
{
    switch (fmt) {
    case BAV_FMT_I1: return 0;
    case BAV_FMT_I2: return 1;
    case BAV_FMT_I4: return 2;
    case BAV_FMT_I8: return 3;
    case BAV_FMT_RGB1555: return 4;
    case BAV_FMT_RGB565: return 6;
    case BAV_FMT_RGB444: return 7;
    default: return 6;
    }
}

int main(void)
{
    dock_t *d = dock_open();
    bav_header_t bav;
    uint8_t hdr[BAV_HEADER_LEN];
    uint8_t fh;
    size_t got = 0;
    uint24_t saved_ctrl, saved_base;
    uint16_t saved_pal[BAV_PALETTE_MAX];
    uint32_t frame_bytes, max_bytes, data_off;
    uint32_t shown = 0xFFFFFFFFu, next, t0, paused_at = 0;
    bool have_audio, paused = false, direct;
    uint8_t back = 1;
    uint24_t ctrl;
    int rc, i;

    if (!d) {
        message("DOCK not installed.", "Run DOCK first.");
        return 1;
    }
    if (dock_status(d) != DOCK_OK) {
        dock_close(d);
        message("Not connected to a", "calcScreen dock.");
        return 1;
    }
    if (dock_fs_open(d, VIDEO_FILE, DOCK_FS_READ, &fh) != DOCK_OK) {
        dock_close(d);
        message("No badapple.bav on", "the dock's USB drive.");
        return 1;
    }
    rc = dock_fs_read(d, fh, hdr, sizeof hdr, &got);
    if (rc != DOCK_OK || got != sizeof hdr || !bav_header_read(hdr, &bav) ||
        bav.width != LCD_W || bav.height != LCD_H) {
        dock_fs_close(d, fh);
        dock_close(d);
        message("badapple.bav is not a", "320x240 BAV file.");
        return 1;
    }

    frame_bytes = bav.frame_bytes;
    data_off = bav.data_offset;
    /* Two frames must fit in VRAM for double buffering. */
    max_bytes = VRAM_BYTES / 2u;
    if (frame_bytes == 0 || frame_bytes > max_bytes) {
        dock_fs_close(d, fh);
        dock_close(d);
        message("Frame too large for", "double-buffered VRAM.");
        return 1;
    }

    /* Save the LCD state we are about to change. */
    saved_ctrl = lcd_Control;
    saved_base = lcd_UpBase;
    direct = bav.fmt >= BAV_FMT_RGB1555;
    for (i = 0; i < (int)BAV_PALETTE_MAX; i++)
        saved_pal[i] = lcd_Palette[i];

    /* Load the palette for palettized formats. If the file carries one, use
     * it (colour videos do); otherwise synthesize a grayscale ramp so plain
     * 1/2/4/8-bpp files (e.g. black-and-white Bad Apple) still render. */
    if (!direct) {
        uint16_t count = bav_palette_for_fmt(bav.fmt); /* 2/4/16/256 */
        if (bav.palette_count) {
            uint16_t pal[BAV_PALETTE_MAX];
            uint32_t n = (uint32_t)bav.palette_count * 2u;
            if (dock_fs_read(d, fh, pal, n, &got) != DOCK_OK || got != n) {
                dock_fs_close(d, fh);
                dock_close(d);
                message("Could not read the", "video palette.");
                return 1;
            }
            for (i = 0; i < (int)bav.palette_count; i++)
                lcd_Palette[i] = pal[i];
        } else {
            /* Grayscale ramp in RGB1555: index 0 = black .. max = white. */
            for (i = 0; i < (int)count; i++) {
                uint16_t g = count > 1
                                 ? (uint16_t)((uint32_t)i * 31u / (count - 1u))
                                 : 31u;
                lcd_Palette[i] = (uint16_t)((g << 10) | (g << 5) | g);
            }
        }
    }

    /* Switch the LCD to the file's format. Sub-byte formats carry a bit order;
     * BEPO set means the first pixel is in the high bits (no LSB-first flag). */
    memset(vram, 0, VRAM_BYTES);
    ctrl = (saved_ctrl & ~(uint24_t)(LCD_BPP_MASK | LCD_BEPO)) |
           ((uint24_t)lcd_bpp_code(bav.fmt) << LCD_BPP_SHIFT);
    if (!direct && !(bav.flags & BAV_F_LSB_FIRST))
        ctrl |= LCD_BEPO;
    lcd_UpBase = (uint24_t)vram;
    lcd_Control = ctrl;

    have_audio = dock_audio_play_file(d, AUDIO_FILE) == DOCK_OK;
    t0 = clock_ms();

    for (;;) {
        uint8_t key = os_GetCSC();
        uint32_t pos;
        if (key == sk_Clear || kb_On)
            break;
        if (key == sk_Enter) {
            paused = !paused;
            if (have_audio) {
                if (paused)
                    dock_audio_stop(d);
            } else if (paused) {
                paused_at = clock_ms() - t0;
            } else {
                t0 = clock_ms() - paused_at;
            }
        }
        if (paused) {
            dock_service(d);
            continue;
        }

        if (have_audio) {
            bool playing = false;
            if (dock_audio_pos(d, &pos, &playing) != DOCK_OK)
                break;
            if (!playing && pos > 0)
                break; /* soundtrack finished */
        } else {
            pos = clock_ms() - t0;
        }

        next = frame_at(pos, bav.fps_milli);
        if (next >= bav.frame_count)
            break;
        if (next == shown) {
            dock_service(d);
            continue;
        }

        /* Read the frame straight into the back buffer, then flip. */
        if (next != shown + 1u) {
            uint32_t off = data_off + next * frame_bytes;
            if (dock_fs_seek(d, fh, (int32_t)off, DOCK_SEEK_SET, NULL) != DOCK_OK)
                break;
        }
        if (dock_fs_read(d, fh, vram + (uint32_t)back * max_bytes, frame_bytes,
                         &got) != DOCK_OK ||
            got != frame_bytes)
            break;
        lcd_UpBase = (uint24_t)(vram + (uint32_t)back * max_bytes);
        back ^= 1u;
        shown = next;
        dock_screen_push(d);
    }

    if (have_audio)
        dock_audio_stop(d);
    dock_fs_close(d, fh);

    /* Restore TI-OS's LCD setup and redraw the homescreen. */
    lcd_Control = saved_ctrl;
    lcd_UpBase = saved_base;
    for (i = 0; i < (int)BAV_PALETTE_MAX; i++)
        lcd_Palette[i] = saved_pal[i];
    dock_close(d);
    os_ClrHome();
    os_ClrLCDFull();
    os_DrawStatusBar();
    return 0;
}
