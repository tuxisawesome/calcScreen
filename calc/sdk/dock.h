/*
 * dock.h - calcScreen dock API for TI-84 Plus CE programs.
 *
 * A program that includes this and links the SDK (see dock.mk) becomes
 * "dock-aware": it drives the dock's screen mirror, keyboard, audio, USB
 * drives and network directly. The USB device driver is embedded in the
 * program itself (calc/hal_ce) - nothing is installed on the calculator and
 * no flash is written. Running the program through arTIfiCE (or any asm
 * launcher) is enough.
 *
 *   #include "dock.h"
 *   dock_t *d = dock_open();
 *   if (!d || dock_status(d) != DOCK_OK) { ...not docked... }
 *   dock_audio_play_file(d, "song.wav");
 *   ... dock_service(d) in your loops ...
 *   dock_close(d);
 *
 * Every data call is in core/dock_api.h and behaves the same on every
 * platform. Calls block until the dock answers while keeping the mirror live.
 */
#ifndef DOCK_H
#define DOCK_H

#include "../../core/dock_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Take over the USB port and connect to the dock. Returns a session handle
 * (statically owned - do not free), or NULL if USB could not be initialised.
 * A non-NULL return still needs dock_status(d) == DOCK_OK before use; call
 * dock_service(d) in a loop until it is connected (with a timeout of your
 * choosing).
 */
dock_t *dock_open(void);

/* Set the saved 6-digit join code (or NULL to clear) before/after open. It is
 * persisted in an archived AppVar so it survives RAM clears. */
void dock_set_join_code(dock_t *d, const char *code);

/* Release the USB port back to TI-OS. Always call before returning to the OS. */
void dock_close(dock_t *d);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_H */
