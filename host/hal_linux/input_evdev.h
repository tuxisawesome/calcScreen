/*
 * input_evdev.h - every keyboard on the system (evdev), exclusively grabbed so
 * the Linux console never sees the keys, with hotplug via inotify. Keys are
 * reported as USB HID usage IDs plus the HID modifier byte.
 */
#ifndef HOST_INPUT_EVDEV_H
#define HOST_INPUT_EVDEV_H

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#define INPUT_UP 0
#define INPUT_DOWN 1
#define INPUT_REPEAT 2

/* mods is the modifier state *after* this event. */
typedef void (*input_key_fn)(void *user, uint8_t usage, uint8_t mods, int value);

typedef struct input input_t;

/* dir: device directory (normally "/dev/input"). Never fails for lack of
 * keyboards; returns NULL only on allocation failure. */
input_t *input_open(const char *dir, input_key_fn fn, void *user);
void input_close(input_t *in);

int input_pollfds(input_t *in, struct pollfd *pfd, int max);
void input_handle(input_t *in, const struct pollfd *pfd, int n);
/* Periodic work (rescan when inotify is unavailable). */
void input_tick(input_t *in, uint64_t now_ms);

/* Grab (default) or release every keyboard. Releasing also forgets the
 * modifier state. */
void input_set_grab(input_t *in, bool grab);

/* Linux KEY_* code -> HID usage (0 if unmapped). Exposed for tests. */
uint8_t input_key_to_hid(unsigned code);

#endif /* HOST_INPUT_EVDEV_H */
