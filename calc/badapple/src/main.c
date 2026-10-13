/*
 * BADAPPLE - the calcScreen end-to-end test.
 *
 * Streams a pre-converted 1 bpp video (badapple.bav) from the dock's USB drive
 * straight into VRAM and has the dock play the soundtrack (badapple.wav). The
 * dock mirrors the LCD back to the monitor, so the video and sound come out of
 * the Pi while the calculator does all the work. Video is slaved to the audio
 * clock reported by the dock, so they stay in sync.
 *
 * Controls: CLEAR or ON quits, ENTER pauses.
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
#define FRAME_W 320u
#define FRAME_H 240u
#define FRAME_BYTES (FRAME_W * FRAME_H / 8u)

/* PL111-style LCD control bits (TI-OS uses 0x92D: 16 bpp 565, BGR, TFT). */
#define LCD_BPP_MASK 0x00Eu
#define LCD_BPP_1 0x000u
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
    /* timer_Get(1) counts 32768 Hz ticks when enabled by the toolchain's
     * clock helpers; clock() is milliseconds-agnostic, so use CLOCKS_PER_SEC. */
    return (uint32_t)((uint64_t)clock() * 1000u / CLOCKS_PER_SEC);
}

int main(void)
{
    dock_t *d = dock_open();
    bav_header_t bav;
    uint8_t hdr[BAV_HEADER_LEN];
    uint8_t fh;
    size_t got = 0;
    uint24_t saved_ctrl, saved_base;
    uint16_t saved_pal0, saved_pal1;
    uint32_t shown = 0xFFFFFFFFu, next = 0, t0, paused_at = 0;
    bool have_audio, paused = false;
    uint8_t back = 1;
    int rc;

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
        bav.width != FRAME_W || bav.height != FRAME_H || bav.bpp != 1) {
        dock_fs_close(d, fh);
        dock_close(d);
        message("badapple.bav is not", "a 320x240 1bpp BAV.");
        return 1;
    }

    /* Switch the LCD to 1 bpp, black/white palette. */
    saved_ctrl = lcd_Control;
    saved_base = lcd_UpBase;
    saved_pal0 = lcd_Palette[0];
    saved_pal1 = lcd_Palette[1];
    memset(vram, 0, 2u * FRAME_BYTES);
    lcd_Palette[0] = 0x0000;
    lcd_Palette[1] = 0x7FFF;
    lcd_UpBase = (uint24_t)vram;
    lcd_Control = (saved_ctrl & ~(uint24_t)(LCD_BPP_MASK | LCD_BEPO)) | LCD_BPP_1 |
                  ((bav.flags & BAV_F_LSB_FIRST) ? 0u : LCD_BEPO);

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
                /* The audio API has no pause: stop and resume by restart is
                 * not seekable, so pausing simply stops the music. */
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
            uint32_t off = bav.data_offset + next * bav.frame_bytes;
            if (dock_fs_seek(d, fh, (int32_t)off, DOCK_SEEK_SET, NULL) != DOCK_OK)
                break;
        }
        if (dock_fs_read(d, fh, vram + back * FRAME_BYTES, FRAME_BYTES, &got) != DOCK_OK ||
            got != FRAME_BYTES)
            break;
        lcd_UpBase = (uint24_t)(vram + back * FRAME_BYTES);
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
    lcd_Palette[0] = saved_pal0;
    lcd_Palette[1] = saved_pal1;
    dock_close(d);
    os_ClrHome();
    os_ClrLCDFull();
    os_DrawStatusBar();
    return 0;
}
