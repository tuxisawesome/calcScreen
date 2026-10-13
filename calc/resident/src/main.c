/*
 * main.c - RESIDOCK flash-app entry (EXPERIMENTAL).
 *
 * Launched from the [apps] menu. If the resident mirror is not installed, it
 * takes over USB, installs the GetCSC hook, and returns to the homescreen with
 * the hook live - so the Pi keeps mirroring the bare TI-OS and forwarding keys.
 * Run it again to remove the hook and hand USB back to TI-OS.
 *
 * Removal is also automatic on a RAM clear: the hook pointer and active flag
 * live in RAM, so a reset leaves the calculator completely normal (the app
 * stays in flash until deleted from [mem]). Nothing here writes flash.
 */
#include <stdint.h>

#include <ti/screen.h>
#include <ti/getcsc.h>
#include <tice.h>
#include <intce.h>

#include "resident.h"

/* GetCSC hook active flag = bit 0 of osFlags+0x34; pointer at getKeyHookPtr. */
#define OS_HOOKFLAGS2   (*(volatile uint8_t *)0xD000B4)
#define GETKEY_HOOK_PTR (*(void *volatile *)0xD025DE)
#define GETCSC_HOOK_ACTIVE 0x01u

static bool resident_is_installed(void)
{
    return (OS_HOOKFLAGS2 & GETCSC_HOOK_ACTIVE) &&
           GETKEY_HOOK_PTR == (void *)&resident_hook;
}

static void wait_key(void)
{
    while (!os_GetCSC())
        ;
}

int main(void)
{
    os_ClrHome();

    if (resident_is_installed()) {
        uint8_t key;
        os_PutStrFull("calcScreen resident");
        os_NewLine();
        os_PutStrFull("mirror is ON.");
        os_NewLine();
        os_NewLine();
        os_PutStrFull("[del] remove it");
        os_NewLine();
        os_PutStrFull("[clear] keep it on");
        do {
            key = os_GetCSC();
        } while (key != sk_Del && key != sk_Clear);
        if (key == sk_Del) {
            resident_uninstall_hook();
            resident_teardown();
            os_ClrHome();
            os_PutStrFull("Resident mirror removed.");
        } else {
            os_ClrHome();
            os_PutStrFull("Still mirroring.");
        }
        os_NewLine();
        os_PutStrFull("Press any key.");
        wait_key();
        os_ClrHomeFull();
        return 0;
    }

    /* Not installed: bring USB up and arm the hook. halce_init() leaves
     * interrupts disabled; we re-enable them (the OS USB/timer-3 masks stay
     * set) only after the hook is armed, then return to TI-OS. */
    if (!resident_bringup()) {
        int_Enable();
        os_ClrHome();
        os_PutStrFull("USB init failed.");
        os_NewLine();
        os_PutStrFull("Is the dock connected?");
        os_NewLine();
        os_PutStrFull("Press any key.");
        wait_key();
        os_ClrHomeFull();
        return 1;
    }
    resident_install_hook();
    int_Enable();

    os_PutStrFull("calcScreen resident");
    os_NewLine();
    os_PutStrFull("mirror armed.");
    os_NewLine();
    os_NewLine();
    os_PutStrFull("Quit to the homescreen -");
    os_NewLine();
    os_PutStrFull("the dock keeps mirroring");
    os_NewLine();
    os_PutStrFull("and keys keep working.");
    os_NewLine();
    os_PutStrFull("Run again to remove.");
    os_NewLine();
    os_PutStrFull("Press any key.");
    wait_key();
    os_ClrHomeFull();
    return 0;
}
