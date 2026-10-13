/*
 * servicer.c - the C glue the resident GetCSC hook drives (EXPERIMENTAL).
 *
 * resident_tick() runs one service pass each time the OS key-scan loop calls
 * our hook while idle: pump the USB device + dock engine, and at most every
 * RESIDENT_PUSH_MS diff the live homescreen VRAM and stream the changed rows to
 * the Pi. Any key the Pi sends is stashed for the hook to inject.
 *
 * All mutable state lives at fixed RAM addresses (resident.h), NOT in this
 * app's bss, because the installer program's RAM is reclaimed once it exits but
 * the hook keeps running afterward. The portable core keeps all engine state in
 * the caller-owned dock_dev_t, and usb_ce.c's context is relocated to a fixed
 * address by -DDOCK_RESIDENT_USB_CTX, so nothing we rely on sits in bss. The
 * one exception is time_ce.c's saved-timer statics, which are only needed by
 * time_ce_cleanup() at teardown; time_ce_ms() reads a free-running hardware
 * counter and keeps working regardless. See docs/RESIDENT.md.
 */
#include <stdint.h>

#include "hal_ce.h"
#include "dock_dev.h"
#include "resident.h"

/* time_ce.h lives in hal_ce/src, which isn't on this app's include path. */
uint32_t time_ce_ms(void);

#define R_INJECTED (*(volatile uint8_t *)RESIDENT_INJECTED_KEY)
#define R_LASTPUSH (*(volatile uint32_t *)RESIDENT_LAST_PUSH)
#define R_DEV      ((dock_dev_t *)RESIDENT_DEV_CTX)

bool resident_bringup(void)
{
    /* halce_init() takes the USB controller from TI-OS, masks the OS USB and
     * timer-3 interrupts, and leaves interrupts disabled; the caller re-enables
     * them (keeping the masks) before handing control back to TI-OS. */
    char code[DOCK_JOIN_LEN + 1];
    if (!halce_init())
        return false;
    dock_dev_init(R_DEV, halce_hal(), halce_calc_id());
    /* Reuse the join code the DOCK app saved (archived DOCKCFG AppVar), so a
     * dock that requires one still accepts us with no UI of our own. */
    if (halce_cfg_load_join(code, sizeof code))
        dock_dev_set_join_code(R_DEV, code);
    R_INJECTED = 0;
    R_LASTPUSH = time_ce_ms();
    return true;
}

void resident_teardown(void)
{
    dock_dev_bye(R_DEV);
    halce_cleanup(); /* restores the OS USB stack and the interrupt masks */
}

void resident_tick(void)
{
    dock_key_t k;
    uint32_t now;

    dock_dev_poll(R_DEV); /* service EP0/bulk USB and answer the Pi's requests */

    now = time_ce_ms();
    if ((uint32_t)(now - R_LASTPUSH) >= RESIDENT_PUSH_MS) {
        dock_dev_screen_push(R_DEV); /* diff live VRAM, stream changed rows */
        R_LASTPUSH = now;
    }

    /* Hand the hook the next Pi keystroke to inject (one at a time). */
    if (R_INJECTED == 0 && dock_dev_key_pop(R_DEV, &k) && k.scancode != 0)
        R_INJECTED = k.scancode;
}
