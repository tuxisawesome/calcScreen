/*
 * settings_ui.h - the dock's settings page (keyboard driven, drawn with
 * core/dock_ui into the UI buffer). Optional admin password with lockout,
 * Wi-Fi, network API, join code, audio, password change, shell, reboot.
 */
#ifndef DOCKD_SETTINGS_UI_H
#define DOCKD_SETTINGS_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "dock_ui.h"

struct dockd;

#define SUI_TEXT_MAX 64

typedef struct settings_ui {
    struct dockd *d;
    int screen;
    int sel, top;              /* menu / list cursor */
    char text[SUI_TEXT_MAX];   /* text being typed */
    char pw1[SUI_TEXT_MAX];    /* first entry of a new password */
    int bad_tries;
    uint64_t lock_until_ms;
    char msg_title[48];
    char msg_body[192];
    int msg_return;            /* screen after the message */
    char ssid[33];             /* network being joined */
    uint64_t last_draw_s;      /* countdown redraws */
} settings_ui_t;

void settings_ui_init(settings_ui_t *u, struct dockd *d);
/* The page was opened (asks for the password first when one is set). */
void settings_ui_open(settings_ui_t *u);
void settings_ui_key(settings_ui_t *u, uint8_t usage, uint8_t mods, uint8_t ascii);
void settings_ui_render(settings_ui_t *u, const dock_canvas_t *c);
/* Background progress (scan/connect results, lockout countdown). Returns
 * true when a redraw is needed. */
bool settings_ui_tick(settings_ui_t *u, uint64_t now_ms);
int settings_ui_timeout_ms(const settings_ui_t *u);
/* Forget typed secrets (called when the page closes). */
void settings_ui_wipe(settings_ui_t *u);

#endif /* DOCKD_SETTINGS_UI_H */
