/*
 * hal_ce.h - embedded TI-84 Plus CE back end for the dock device engine.
 *
 * Implements core/dock_dev_hal.h on top of the calculator's own hardware:
 *   - usb_ce.c  : a small polled USB *device* driver for the FOTG210-style
 *                 controller at 0xE20000 (full speed, EP0 + bulk EP1 IN /
 *                 EP2 OUT). It takes the port from TI-OS and hands it back.
 *   - dma_ce.S  : byte-wise MMIO helpers and the bulk DMA inner loop.
 *   - lcd_ce.c  : reads the live LCD controller (0xE30000) to describe the
 *                 framebuffer (base, geometry, bpp, palette) for mirroring.
 *   - time_ce.c : a monotonic millisecond clock.
 *   - cfg_ce.c  : the join code, stored in an archived AppVar.
 *
 * None of this writes flash or installs anything resident; it is ordinary
 * program code that runs while the dock-aware program is running.
 */
#ifndef HAL_CE_H
#define HAL_CE_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

bool halce_init(void);
void halce_cleanup(void);
void *halce_hal(void);
uint32_t halce_calc_id(void);
bool halce_cfg_load_join(char *out, size_t cap);
void halce_cfg_save_join(const char *code);

#ifdef __cplusplus
}
#endif

#endif /* HAL_CE_H */
