/*
 * lcd_ce.h - describe the live CE LCD framebuffer for screen mirroring.
 */
#ifndef LCD_CE_H
#define LCD_CE_H

#include "dock_dev_hal.h" /* dock_fb_t */

/*
 * Fill *fb from the live PL111 LCD controller (0xE30000): base pointer,
 * fixed 320x240 geometry, pixel format derived from lcd_Control, and - for
 * palettized modes - a RAM copy of the hardware palette (the palette lives in
 * MMIO at 0xE30200, which the USB DMA engine cannot read, so it must be copied
 * into RAM before it can be streamed). Returns false if the LCD is in a mode
 * the dock codec does not support (e.g. 24bpp).
 */
bool halce_lcd_fb(dock_fb_t *fb);

#endif /* LCD_CE_H */
