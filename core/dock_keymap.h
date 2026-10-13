/*
 * dock_keymap.h - PC keyboard (USB HID usage IDs) to TI-84 Plus CE keys.
 *
 * Input is the HID usage of a key that went down plus the HID modifier byte,
 * so any platform with a USB keyboard can use it (Linux evdev codes are mapped
 * to HID usages in host/hal_linux).
 *
 * Output is a TI key event: the GetKey code TI-OS should see (ti/getkey.h
 * k* values, 2-byte keys as 0xPPXX with the 0xFE/0xFC/0xFB prefix in the high
 * byte) and, when one exists, the physical GetCSC scan code (ti/getcsc.h
 * sk_*).
 */
#ifndef DOCK_KEYMAP_H
#define DOCK_KEYMAP_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HID modifier bits (USB HID boot protocol byte). */
#define DOCK_MOD_LCTRL 0x01u
#define DOCK_MOD_LSHIFT 0x02u
#define DOCK_MOD_LALT 0x04u
#define DOCK_MOD_LGUI 0x08u
#define DOCK_MOD_RCTRL 0x10u
#define DOCK_MOD_RSHIFT 0x20u
#define DOCK_MOD_RALT 0x40u
#define DOCK_MOD_RGUI 0x80u
#define DOCK_MOD_CTRL (DOCK_MOD_LCTRL | DOCK_MOD_RCTRL)
#define DOCK_MOD_SHIFT (DOCK_MOD_LSHIFT | DOCK_MOD_RSHIFT)
#define DOCK_MOD_ALT (DOCK_MOD_LALT | DOCK_MOD_RALT)

typedef struct dock_ti_key {
    uint16_t tikey;   /* GetKey code, 0 = none */
    uint8_t scancode; /* GetCSC code, 0 = none */
    uint8_t flags;    /* DOCK_KEYF_* (dock_proto.h) */
} dock_ti_key_t;

/* Dock-level actions bound to key chords (never forwarded to the calc). */
enum dock_hotkey {
    DOCK_HOTKEY_NONE = 0,
    DOCK_HOTKEY_SETTINGS, /* Ctrl+Alt+S, or F12 */
    DOCK_HOTKEY_REFRESH   /* Ctrl+Alt+R: request a full screen refresh */
};

/* Translate a key press. Returns true and fills *out when the key maps to a
 * TI key. Bindings (documented in README):
 *   letters/digits/symbols -> direct key codes (letters as uppercase A-Z,
 *   Shift+letter as lowercase where TI-OS has it), Enter -> ENTER,
 *   Backspace/Delete -> DEL, Esc -> CLEAR, arrows -> arrows,
 *   F1..F5 -> Y= WINDOW ZOOM TRACE GRAPH, F6 -> MODE, F7 -> STAT,
 *   F8 -> MATH, F9 -> APPS, F10 -> PRGM, F11 -> VARS, Tab -> ALPHA,
 *   Ctrl+key -> 2nd function of key, Alt+key -> ALPHA function,
 *   Home -> 2nd+LEFT, End -> 2nd+RIGHT, Insert -> INS (2nd+DEL),
 *   Pause/Break -> ON (DOCK_KEYF_ON), PgUp/PgDn -> ALPHA+UP / ALPHA+DOWN
 *   (page up/down; 2nd+UP/DOWN only change the brightness on the CE),
 *   Shift+'-' -> (-) negation, '=' '<' '>' '!' -> TEST/PRB tokens.
 * 2-byte GetKey codes are 0xPPXX with the prefix (0xFE/0xFC/0xFB) in the
 * high byte, as in libticalcs (the CE toolchain's k_* swap the bytes).
 * Hotkeys (dock_keymap_hotkey) take precedence: they translate to nothing. */
bool dock_keymap_translate(uint8_t hid_usage, uint8_t mods, dock_ti_key_t *out);

/* ASCII for a key press (0 if none), for HIDKEY events. */
uint8_t dock_keymap_ascii(uint8_t hid_usage, uint8_t mods);

/* Dock hotkey for a key press, DOCK_HOTKEY_NONE otherwise. */
int dock_keymap_hotkey(uint8_t hid_usage, uint8_t mods);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_KEYMAP_H */
