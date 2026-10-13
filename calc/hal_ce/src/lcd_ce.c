/*
 * lcd_ce.c - read the CE PL111 LCD controller and describe the framebuffer.
 *
 * Register map (sys/lcd.h):
 *   lcd_Control (0xE30018, 24-bit)  bits [3:1] = LcdBpp, bit 8 = BGR,
 *                                   bit 10 (0x400) = BEPO (sub-byte pixel order)
 *   lcd_UpBase  (0xE30010, 24-bit)  active framebuffer base (in VRAM)
 *   lcd_Palette (0xE30200)          256 * uint16 little-endian palette entries
 *
 * LcdBpp -> dock_pix_fmt (SPEC-independent; this is PL111, see dock_screen.h):
 *   0 = 1bpp     -> DOCK_PIX_I1
 *   1 = 2bpp     -> DOCK_PIX_I2
 *   2 = 4bpp     -> DOCK_PIX_I4
 *   3 = 8bpp     -> DOCK_PIX_I8
 *   4 = 16bpp    -> DOCK_PIX_RGB1555
 *   5 = 24bpp    -> unsupported by the dock codec (return false)
 *   6 = 16bpp565 -> DOCK_PIX_RGB565  (TI-OS default: control 0x92D -> 565 BGR)
 *   7 = 12bpp444 -> DOCK_PIX_RGB444
 *
 * The VRAM framebuffer at lcd_UpBase is a valid USB DMA source (SPEC 5.4:
 * 0xD40000-0xD657FF is DMA-reachable), so screen rows stream straight out of
 * VRAM with no copy. The palette does need a RAM copy (see header).
 */
#include "lcd_ce.h"

#include <stdint.h>
#include <sys/lcd.h>

#include "dock_common.h"
#include "dock_screen.h"

/* RAM mirror of the hardware palette, refreshed each call, so palette_le points
 * at DMA-reachable RAM and not at the MMIO palette window. */
static uint8_t s_pal[DOCK_PALETTE_MAX * 2u];

bool halce_lcd_fb(dock_fb_t *fb)
{
    uint24_t ctrl = lcd_Control;
    uint8_t code = (uint8_t)((ctrl >> 1) & 0x7u);
    uint8_t fmt;
    uint16_t pal_n;

    switch (code) {
    case 0: fmt = DOCK_PIX_I1; break;
    case 1: fmt = DOCK_PIX_I2; break;
    case 2: fmt = DOCK_PIX_I4; break;
    case 3: fmt = DOCK_PIX_I8; break;
    case 4: fmt = DOCK_PIX_RGB1555; break;
    case 6: fmt = DOCK_PIX_RGB565; break;
    case 7: fmt = DOCK_PIX_RGB444; break;
    default: return false; /* 24bpp (5) or reserved: dock codec has no mapping */
    }

    fb->width = LCD_WIDTH;   /* 320 */
    fb->height = LCD_HEIGHT; /* 240 */
    fb->fmt = fmt;
    /* CE VRAM rows are tightly packed, so the stride is just the row size. */
    fb->stride = dock_row_bytes(LCD_WIDTH, fmt);
    fb->flags = (uint8_t)(((ctrl & 0x100u) ? DOCK_SCRF_BGR : 0u) |
                          ((ctrl & 0x400u) ? DOCK_SCRF_BEPO : 0u));
    fb->pixels = (const uint8_t *)(uint24_t)lcd_UpBase;

    pal_n = dock_palette_size(fmt);
    if (pal_n) {
        uint16_t i;
        volatile uint16_t *hw = lcd_Palette;
        if (pal_n > DOCK_PALETTE_MAX)
            pal_n = DOCK_PALETTE_MAX;
        for (i = 0; i < pal_n; i++)
            dock_put_u16(s_pal + (size_t)i * 2u, hw[i]);
        fb->palette_le = s_pal;
    } else {
        fb->palette_le = NULL;
    }
    return true;
}
