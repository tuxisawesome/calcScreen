/*
 * resident.h - calcScreen resident fast-mirror driver (EXPERIMENTAL).
 *
 * This is a TI-84 Plus CE flash application that installs a GetCSC hook so the
 * calcScreen fast USB mirror keeps running on the bare TI-OS homescreen after
 * the DOCK program exits. The full design, the confidence grading of every
 * TI-OS detail it relies on, and the CEmu validation plan are in
 * docs/RESIDENT.md and docs/TIOS_HOOKS_SPEC.md. None of this is verified on
 * hardware - treat it as a CEmu-first experiment.
 *
 * Why a flash app: as of TI-OS 5.8.3 hook code may only live inside a flash
 * app or the OS itself, so the hook body is here (stable flash address). Only
 * the driver's MUTABLE STATE needs a RAM home, and it goes in the OS USB
 * buffer area, which is free while our driver owns the USB controller (the OS
 * USB stack is disabled for the whole resident session). These addresses are
 * the single most likely thing to need retuning in CEmu - see docs/RESIDENT.md.
 *
 * RESIDENT_USB_CTX below MUST equal the -DDOCK_RESIDENT_USB_CTX value passed to
 * usb_ce.c in this app's makefile.
 */
#ifndef CALC_RESIDENT_H
#define CALC_RESIDENT_H

#include "dock_common.h"

/* Fixed RAM layout inside the OS USB area (0xD13FD8..0xD177B7, ~14 KB). */
#define RESIDENT_BASE         0xD13FD8u
#define RESIDENT_INJECTED_KEY (RESIDENT_BASE + 0u)   /* u8: pending GetCSC code */
#define RESIDENT_LAST_PUSH    (RESIDENT_BASE + 4u)   /* u32: last screen-push ms */
#define RESIDENT_USB_CTX      (RESIDENT_BASE + 16u)  /* usb_ce.c ctx_t (~1.3 KB) */
#define RESIDENT_DEV_CTX      (RESIDENT_BASE + 16u + 0x800u) /* dock_dev_t */

#define RESIDENT_PUSH_MS 40u /* min ms between full screen diff+push passes */

/* servicer.c - the C driver glue the hook drives. */
bool resident_bringup(void);  /* take over USB + init engine (call once, di) */
void resident_teardown(void); /* hand USB back to TI-OS (uninstall path) */
void resident_tick(void);     /* one service pass, called from the GetCSC hook */

/* hook.S - the $83 GetCSC hook and its (un)installers. */
void resident_hook(void);           /* address only: the $83 entry byte */
void resident_install_hook(void);   /* SetGetCSCHook(&resident_hook) */
void resident_uninstall_hook(void); /* ClrGetKeyHook */

#endif /* CALC_RESIDENT_H */
