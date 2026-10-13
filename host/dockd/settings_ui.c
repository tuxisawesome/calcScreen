/* settings_ui.c - see settings_ui.h. */
#define _GNU_SOURCE
#include "settings_ui.h"
#include "dockd.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

/* HID usages used for navigation. */
#define HID_ENTER 0x28
#define HID_ESC 0x29
#define HID_BACKSPACE 0x2A
#define HID_RIGHT 0x4F
#define HID_LEFT 0x50
#define HID_DOWN 0x51
#define HID_UP 0x52
#define HID_KP_ENTER 0x58

#define MAX_TRIES 3
#define LOCKOUT_MS 30000u
#define LIST_ROWS 10   /* list entries visible at once */
#define VOLUME_STEP 5

enum screen {
    SCR_PASSWORD,
    SCR_LOCKED,
    SCR_MENU,
    SCR_SCANNING,
    SCR_WIFI_LIST,
    SCR_WIFI_PW,
    SCR_CONNECTING,
    SCR_JOIN,
    SCR_PW_NEW1,
    SCR_PW_NEW2,
    SCR_CONFIRM_REBOOT,
    SCR_UPDATE_SCAN,
    SCR_UPDATE_CONFIRM,
    SCR_UPDATE_PROGRESS,
    SCR_DIAG,
    SCR_MESSAGE
};

enum item {
    IT_WIFI,
    IT_WIFI_CHOOSE,
    IT_NET_API,
    IT_JOIN_REQ,
    IT_JOIN_SHOW,
    IT_JOIN_NEW,
    IT_AUDIO_OUT,
    IT_VOLUME,
    IT_PASSWORD,
    IT_SHELL,
    IT_REBOOT,
    IT_UPDATE,
    IT_DIAG,
    IT_CLOSE,
    IT_COUNT
};

/* Diagnostics sub-menu rows (SCR_DIAG). */
enum diag_row {
    DIAG_OVERLAY,
    DIAG_FPS,
    DIAG_CONN,
    DIAG_SYS,
    DIAG_COUNT
};

static void wipe(char *p, size_t n)
{
    explicit_bzero(p, n);
}

void settings_ui_wipe(settings_ui_t *u)
{
    wipe(u->text, sizeof u->text);
    wipe(u->pw1, sizeof u->pw1);
}

void settings_ui_init(settings_ui_t *u, struct dockd *d)
{
    memset(u, 0, sizeof *u);
    u->d = d;
    u->screen = SCR_MENU;
}

static void go(settings_ui_t *u, int screen)
{
    u->screen = screen;
    dockd_redraw(u->d);
}

static void message(settings_ui_t *u, const char *title, const char *body, int ret)
{
    str_copy(u->msg_title, sizeof u->msg_title, title);
    str_copy(u->msg_body, sizeof u->msg_body, body);
    u->msg_return = ret;
    go(u, SCR_MESSAGE);
}

static void prompt(settings_ui_t *u, int screen)
{
    wipe(u->text, sizeof u->text);
    go(u, screen);
}

void settings_ui_message(settings_ui_t *u, const char *title, const char *body)
{
    message(u, title, body, SCR_MENU);
}

void settings_ui_open(settings_ui_t *u)
{
    settings_ui_wipe(u);
    u->sel = 0;
    u->top = 0;
    if (!u->d->s.admin_set)
        go(u, SCR_MENU);
    else if (mono_ms() < u->lock_until_ms)
        go(u, SCR_LOCKED);
    else
        prompt(u, SCR_PASSWORD);
}

/* Text entry; returns true when Enter was pressed. Esc is handled by the
 * caller. */
static bool edit_text(settings_ui_t *u, uint8_t usage, uint8_t ascii)
{
    size_t len = strlen(u->text);
    if (usage == HID_ENTER || usage == HID_KP_ENTER)
        return true;
    if (usage == HID_BACKSPACE) {
        if (len)
            u->text[len - 1] = '\0';
    } else if (ascii >= 0x20 && ascii < 0x7F && len + 1 < sizeof u->text) {
        u->text[len] = (char)ascii;
        u->text[len + 1] = '\0';
    }
    dockd_redraw(u->d);
    return false;
}

/* ------------------------------------------------------------------ */
/* menu actions                                                        */

static void toggle(dockd_t *d, uint8_t *v)
{
    *v = !*v;
    dockd_settings_changed(d);
}

static void start_scan(settings_ui_t *u)
{
    dockd_t *d = u->d;
    if (!d->s.wifi_enabled) {
        message(u, "Wi-Fi is off", "Turn Wi-Fi on first.", SCR_MENU);
        return;
    }
    if (!net_control_available(d->net)) {
        message(u, "Wi-Fi", "Wi-Fi control is not available\n(NetworkManager missing).",
                SCR_MENU);
        return;
    }
    net_scan_start(d->net);
    u->sel = u->top = 0;
    go(u, SCR_SCANNING);
}

static void show_join(settings_ui_t *u)
{
    go(u, SCR_JOIN);
}

static void change_volume(dockd_t *d, int delta)
{
    int v = (int)d->s.volume + delta;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v != d->s.volume) {
        d->s.volume = (uint8_t)v;
        dockd_settings_changed(d);
    }
}

/* Scan the USB drives for an update, verify it and move to confirm, or show a
 * message. Runs synchronously (local-filesystem reads only). */
static void start_update_scan(settings_ui_t *u)
{
    dockd_t *d = u->d;
    go(u, SCR_UPDATE_SCAN);
    if (!update_scan(d->opt.drives, &u->upkg)) {
        message(u, "No update found",
                "Put calcscreen-update.tar.gz\nand .manifest on a FAT32\nUSB stick.",
                SCR_MENU);
        return;
    }
    if (update_verify(&u->upkg) != DOCK_OK) {
        message(u, "Update", "Update file is corrupt.", SCR_MENU);
        return;
    }
    go(u, SCR_UPDATE_CONFIRM);
}

static void activate(settings_ui_t *u, int item, int dir)
{
    dockd_t *d = u->d;
    switch (item) {
    case IT_WIFI:
        toggle(d, &d->s.wifi_enabled);
        net_set_radio(d->net, d->s.wifi_enabled);
        break;
    case IT_WIFI_CHOOSE:
        start_scan(u);
        break;
    case IT_NET_API:
        toggle(d, &d->s.net_api_enabled);
        break;
    case IT_JOIN_REQ:
        toggle(d, &d->s.join_required);
        break;
    case IT_JOIN_SHOW:
        show_join(u);
        break;
    case IT_JOIN_NEW:
        dock_settings_new_join_code(&d->s, random_u32());
        dockd_settings_changed(d);
        show_join(u);
        break;
    case IT_AUDIO_OUT:
        d->s.audio_out = d->s.audio_out == DOCK_AUDIO_HDMI ? DOCK_AUDIO_JACK : DOCK_AUDIO_HDMI;
        dockd_settings_changed(d);
        break;
    case IT_VOLUME:
        change_volume(d, dir < 0 ? -VOLUME_STEP : VOLUME_STEP);
        break;
    case IT_PASSWORD:
        prompt(u, SCR_PW_NEW1);
        break;
    case IT_SHELL:
        dockd_request_shell(d);
        break;
    case IT_REBOOT:
        go(u, SCR_CONFIRM_REBOOT);
        break;
    case IT_UPDATE:
        start_update_scan(u);
        break;
    case IT_DIAG:
        u->sel = 0;
        go(u, SCR_DIAG);
        break;
    case IT_CLOSE:
        dockd_close_settings(d);
        break;
    default:
        break;
    }
}

static void menu_key(settings_ui_t *u, uint8_t usage)
{
    switch (usage) {
    case HID_UP:
        u->sel = (u->sel + IT_COUNT - 1) % IT_COUNT;
        break;
    case HID_DOWN:
        u->sel = (u->sel + 1) % IT_COUNT;
        break;
    case HID_LEFT:
    case HID_RIGHT:
        if (u->sel == IT_VOLUME || u->sel == IT_AUDIO_OUT)
            activate(u, u->sel, usage == HID_LEFT ? -1 : 1);
        break;
    case HID_ENTER:
    case HID_KP_ENTER:
        activate(u, u->sel, 1);
        return;
    case HID_ESC:
        dockd_close_settings(u->d);
        return;
    default:
        return;
    }
    dockd_redraw(u->d);
}

/* ------------------------------------------------------------------ */
/* other screens                                                       */

static void password_key(settings_ui_t *u, uint8_t usage, uint8_t ascii)
{
    dockd_t *d = u->d;
    if (usage == HID_ESC) {
        dockd_close_settings(d);
        return;
    }
    if (!edit_text(u, usage, ascii))
        return;
    if (dock_settings_check_password(&d->s, u->text)) {
        u->bad_tries = 0;
        wipe(u->text, sizeof u->text);
        go(u, SCR_MENU);
        return;
    }
    wipe(u->text, sizeof u->text);
    if (++u->bad_tries >= MAX_TRIES) {
        u->bad_tries = 0;
        u->lock_until_ms = mono_ms() + LOCKOUT_MS;
        go(u, SCR_LOCKED);
    } else {
        message(u, "Wrong password", "Try again.", SCR_PASSWORD);
    }
}

static void wifi_list_key(settings_ui_t *u, uint8_t usage)
{
    const net_ap_t *aps;
    int n = 0;
    net_scan_result(u->d->net, &aps, &n);
    switch (usage) {
    case HID_UP:
        if (u->sel > 0)
            u->sel--;
        break;
    case HID_DOWN:
        if (u->sel + 1 < n)
            u->sel++;
        break;
    case HID_ESC:
        u->sel = IT_WIFI_CHOOSE;
        go(u, SCR_MENU);
        return;
    case HID_ENTER:
    case HID_KP_ENTER:
        if (u->sel >= n)
            return;
        str_copy(u->ssid, sizeof u->ssid, aps[u->sel].ssid);
        if (aps[u->sel].secure) {
            prompt(u, SCR_WIFI_PW);
        } else {
            net_connect_start(u->d->net, u->ssid, NULL);
            go(u, SCR_CONNECTING);
        }
        return;
    default:
        return;
    }
    if (u->sel < u->top)
        u->top = u->sel;
    if (u->sel >= u->top + LIST_ROWS)
        u->top = u->sel - LIST_ROWS + 1;
    dockd_redraw(u->d);
}

static void wifi_pw_key(settings_ui_t *u, uint8_t usage, uint8_t ascii)
{
    dockd_t *d = u->d;
    if (usage == HID_ESC) {
        wipe(u->text, sizeof u->text);
        go(u, SCR_WIFI_LIST);
        return;
    }
    if (!edit_text(u, usage, ascii))
        return;
    net_connect_start(d->net, u->ssid, u->text);
    wipe(u->text, sizeof u->text);
    str_copy(d->s.wifi_ssid, sizeof d->s.wifi_ssid, u->ssid);
    dockd_settings_changed(d);
    go(u, SCR_CONNECTING);
}

static void new_password_key(settings_ui_t *u, uint8_t usage, uint8_t ascii)
{
    dockd_t *d = u->d;
    uint8_t salt[DOCK_PW_SALT_LEN];
    if (usage == HID_ESC) {
        settings_ui_wipe(u);
        go(u, SCR_MENU);
        return;
    }
    if (!edit_text(u, usage, ascii))
        return;
    if (u->screen == SCR_PW_NEW1) {
        memcpy(u->pw1, u->text, sizeof u->pw1);
        prompt(u, SCR_PW_NEW2);
        return;
    }
    if (strcmp(u->pw1, u->text) != 0) {
        settings_ui_wipe(u);
        message(u, "Not changed", "The two passwords did not match.", SCR_MENU);
        return;
    }
    random_bytes(salt, sizeof salt);
    dock_settings_set_password(&d->s, u->text, salt);
    dockd_settings_changed(d);
    message(u, "Admin password", u->text[0] ? "Password set." : "Password removed.",
            SCR_MENU);
    settings_ui_wipe(u);
}

/* Toggle the diagnostics row under the cursor. */
static void diag_activate(settings_ui_t *u)
{
    dockd_t *d = u->d;
    switch (u->sel) {
    case DIAG_OVERLAY:
        d->s.perf_overlay = !d->s.perf_overlay;
        break;
    case DIAG_FPS:
        d->s.perf_overlay_sections ^= DOCK_OVL_FPS;
        break;
    case DIAG_CONN:
        d->s.perf_overlay_sections ^= DOCK_OVL_CONN;
        break;
    case DIAG_SYS:
        d->s.perf_overlay_sections ^= DOCK_OVL_SYS;
        break;
    default:
        return;
    }
    dockd_settings_changed(d);
}

static void diag_key(settings_ui_t *u, uint8_t usage)
{
    switch (usage) {
    case HID_UP:
        u->sel = (u->sel + DIAG_COUNT - 1) % DIAG_COUNT;
        break;
    case HID_DOWN:
        u->sel = (u->sel + 1) % DIAG_COUNT;
        break;
    case HID_LEFT:
    case HID_RIGHT:
    case HID_ENTER:
    case HID_KP_ENTER:
        diag_activate(u);
        break;
    case HID_ESC:
        u->sel = IT_DIAG;
        go(u, SCR_MENU);
        return;
    default:
        return;
    }
    dockd_redraw(u->d);
}

/* Confirm screen: Enter stages the update (reboots when the overlay is on, or
 * applies live), Esc returns to the menu. */
static void update_confirm_key(settings_ui_t *u, uint8_t usage)
{
    if (usage == HID_ENTER || usage == HID_KP_ENTER) {
        if (dockd_start_update(u->d, &u->upkg))
            go(u, SCR_UPDATE_PROGRESS);
        else
            message(u, "Update",
                    "The updater is not installed.\nRun pi/install.sh first.",
                    SCR_MENU);
    } else if (usage == HID_ESC) {
        go(u, SCR_MENU);
    }
}

void settings_ui_key(settings_ui_t *u, uint8_t usage, uint8_t mods, uint8_t ascii)
{
    (void)mods;
    switch (u->screen) {
    case SCR_PASSWORD:
        password_key(u, usage, ascii);
        break;
    case SCR_LOCKED:
        if (usage == HID_ESC)
            dockd_close_settings(u->d);
        break;
    case SCR_MENU:
        menu_key(u, usage);
        break;
    case SCR_SCANNING:
    case SCR_CONNECTING:
        if (usage == HID_ESC)
            go(u, SCR_MENU); /* the job finishes in the background */
        break;
    case SCR_WIFI_LIST:
        wifi_list_key(u, usage);
        break;
    case SCR_WIFI_PW:
        wifi_pw_key(u, usage, ascii);
        break;
    case SCR_PW_NEW1:
    case SCR_PW_NEW2:
        new_password_key(u, usage, ascii);
        break;
    case SCR_CONFIRM_REBOOT:
        if (usage == HID_ENTER || usage == HID_KP_ENTER)
            dockd_request_reboot(u->d);
        else if (usage == HID_ESC)
            go(u, SCR_MENU);
        break;
    case SCR_UPDATE_SCAN:
    case SCR_UPDATE_PROGRESS:
        break; /* busy: ignore keys ("do not remove power") */
    case SCR_UPDATE_CONFIRM:
        update_confirm_key(u, usage);
        break;
    case SCR_DIAG:
        diag_key(u, usage);
        break;
    case SCR_JOIN:
    case SCR_MESSAGE:
        if (usage == HID_ENTER || usage == HID_KP_ENTER || usage == HID_ESC)
            go(u, u->screen == SCR_JOIN ? SCR_MENU : u->msg_return);
        break;
    default:
        go(u, SCR_MENU);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */

static void render_menu(settings_ui_t *u, const dock_canvas_t *c)
{
    dockd_t *d = u->d;
    static const char *const labels[IT_COUNT] = {
        "Wi-Fi", "Choose Wi-Fi network", "Network API for calc programs",
        "Require join code", "Show join code", "Regenerate join code",
        "Audio output", "Volume", "Change admin password", "Drop to shell",
        "Reboot", "Update from USB", "Diagnostics", "Close"};
    dock_ui_item_t it[IT_COUNT];
    char vol[16], footer[128], ssid[64], ip[48];
    bool up;
    int i;

    for (i = 0; i < IT_COUNT; i++) {
        it[i].label = labels[i];
        it[i].value = NULL;
    }
    snprintf(vol, sizeof vol, "< %u%% >", d->s.volume);
    it[IT_WIFI].value = d->s.wifi_enabled ? "On" : "Off";
    it[IT_WIFI_CHOOSE].value = d->s.wifi_ssid[0] ? d->s.wifi_ssid : NULL;
    it[IT_NET_API].value = d->s.net_api_enabled ? "On" : "Off";
    it[IT_JOIN_REQ].value = d->s.join_required ? "On" : "Off";
    it[IT_JOIN_SHOW].value = d->s.join_code;
    it[IT_AUDIO_OUT].value = d->s.audio_out == DOCK_AUDIO_JACK ? "< Headphones >" : "< HDMI >";
    it[IT_VOLUME].value = vol;
    it[IT_PASSWORD].value = d->s.admin_set ? "Set" : "None";
    it[IT_DIAG].value = d->s.perf_overlay ? "Overlay on" : "Overlay off";

    net_get_status(d->net, &up, ssid, sizeof ssid, ip, sizeof ip);
    if (up)
        snprintf(footer, sizeof footer, "%s  %s", ssid, ip);
    else if (!d->s.admin_set)
        snprintf(footer, sizeof footer, "Tip: set an admin password");
    else
        snprintf(footer, sizeof footer, "Enter: change   Esc: close");
    dock_ui_screen_menu(c, "Dock settings", it, IT_COUNT, u->sel, footer);
}

static void render_wifi_list(settings_ui_t *u, const dock_canvas_t *c)
{
    const net_ap_t *aps;
    dock_ui_item_t it[LIST_ROWS];
    char val[LIST_ROWS][16];
    int n = 0, i, shown = 0;
    net_scan_result(u->d->net, &aps, &n);
    for (i = u->top; i < n && shown < LIST_ROWS; i++, shown++) {
        snprintf(val[shown], sizeof val[shown], "%u%%%s", aps[i].signal,
                 aps[i].secure ? " *" : "");
        it[shown].label = aps[i].ssid;
        it[shown].value = val[shown];
    }
    dock_ui_screen_menu(c, "Choose Wi-Fi network", it, shown, u->sel - u->top,
                        "Enter: connect   Esc: back   * = password");
}

static void render_locked(settings_ui_t *u, const dock_canvas_t *c)
{
    char body[96];
    uint64_t now = mono_ms();
    unsigned left = u->lock_until_ms > now ? (unsigned)((u->lock_until_ms - now + 999) / 1000) : 0;
    snprintf(body, sizeof body, "Too many wrong passwords.\nTry again in %u s.", left);
    dock_ui_screen_message(c, "Settings locked", body);
}

static void render_update_confirm(settings_ui_t *u, const dock_canvas_t *c)
{
    char body[192];
    int cmp = update_version_cmp(u->upkg.version, DOCKD_VERSION);
    snprintf(body, sizeof body,
             "Found v%s (current %s).%s\n\nInstall and reboot?\n\n"
             "Enter: install   Esc: cancel",
             u->upkg.version, DOCKD_VERSION,
             cmp <= 0 ? "\n(same/older - reinstall?)" : "");
    dock_ui_screen_message(c, "Update from USB", body);
}

/* The progress bar: a background track with a filled portion, a percentage
 * and the current step label, driven by the applier's reported percent. */
static void render_update_progress(settings_ui_t *u, const dock_canvas_t *c)
{
    dockd_t *d = u->d;
    int w = (int)c->w, h = (int)c->h;
    int bx = 24, bw = w - 48, bh = 18, by = h / 2 - bh / 2;
    int pct = d->update_pct;
    char pctbuf[16];
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    dock_ui_screen_message(c, "Updating", NULL);
    dock_ui_text_centered(c, by - 40, d->update_step[0] ? d->update_step : "Working...",
                          DOCK_UI_FG, 1);
    dock_ui_fill(c, bx, by, bw, bh, DOCK_UI_SEL_BG);
    dock_ui_fill(c, bx, by, bw * pct / 100, bh, DOCK_UI_ACCENT);
    snprintf(pctbuf, sizeof pctbuf, "%d%%", pct);
    dock_ui_text_centered(c, by + bh + 12, pctbuf, DOCK_UI_FG, 2);
    dock_ui_text_centered(c, h - 28, "Do not remove power", DOCK_UI_WARN, 1);
}

static void render_diag(settings_ui_t *u, const dock_canvas_t *c)
{
    dockd_t *d = u->d;
    char lines[OVERLAY_MAXLINES][OVERLAY_COL];
    dock_ui_item_t it[DIAG_COUNT];
    static const char *const labels[DIAG_COUNT] = {
        "Performance overlay", "  FPS & throughput", "  Connection details",
        "  System stats"};
    uint8_t sec = d->s.perf_overlay_sections;
    int i, n, y;

    for (i = 0; i < DIAG_COUNT; i++)
        it[i].label = labels[i];
    it[DIAG_OVERLAY].value = d->s.perf_overlay ? "On" : "Off";
    it[DIAG_FPS].value = (sec & DOCK_OVL_FPS) ? "On" : "Off";
    it[DIAG_CONN].value = (sec & DOCK_OVL_CONN) ? "On" : "Off";
    it[DIAG_SYS].value = (sec & DOCK_OVL_SYS) ? "On" : "Off";
    dock_ui_screen_menu(c, "Diagnostics", it, DIAG_COUNT, u->sel,
                        "Enter/<>: toggle   Esc: back");

    /* Live read-only stats block below the toggles. */
    n = overlay_lines(d, lines, OVERLAY_MAXLINES);
    y = 20 + DIAG_COUNT * 14 + 10;
    for (i = 0; i < n; i++)
        dock_ui_text(c, 16, y + i * 10, lines[i], DOCK_UI_DIM, 1);
}

void settings_ui_render(settings_ui_t *u, const dock_canvas_t *c)
{
    dockd_t *d = u->d;
    char title[64];
    switch (u->screen) {
    case SCR_PASSWORD:
        dock_ui_screen_prompt(c, "Admin password", u->text, true,
                              "Enter: unlock   Esc: cancel");
        break;
    case SCR_LOCKED:
        render_locked(u, c);
        break;
    case SCR_MENU:
        render_menu(u, c);
        break;
    case SCR_SCANNING:
        dock_ui_screen_message(c, "Wi-Fi", "Scanning for networks...");
        break;
    case SCR_WIFI_LIST:
        render_wifi_list(u, c);
        break;
    case SCR_WIFI_PW:
        snprintf(title, sizeof title, "Password for %s", u->ssid);
        dock_ui_screen_prompt(c, title, u->text, true, "Enter: connect   Esc: back");
        break;
    case SCR_CONNECTING:
        snprintf(title, sizeof title, "Connecting to\n%s ...", u->ssid);
        dock_ui_screen_message(c, "Wi-Fi", title);
        break;
    case SCR_JOIN:
        dock_ui_screen_join(c, d->s.join_code);
        break;
    case SCR_PW_NEW1:
        dock_ui_screen_prompt(c, "New admin password", u->text, true,
                              "Empty = no password   Esc: cancel");
        break;
    case SCR_PW_NEW2:
        dock_ui_screen_prompt(c, "Repeat the new password", u->text, true,
                              "Enter: save   Esc: cancel");
        break;
    case SCR_CONFIRM_REBOOT:
        dock_ui_screen_message(c, "Reboot", "Reboot the dock now?\n\nEnter: yes   Esc: no");
        break;
    case SCR_UPDATE_SCAN:
        dock_ui_screen_message(c, "Update from USB", "Checking USB drives...");
        break;
    case SCR_UPDATE_CONFIRM:
        render_update_confirm(u, c);
        break;
    case SCR_UPDATE_PROGRESS:
        render_update_progress(u, c);
        break;
    case SCR_DIAG:
        render_diag(u, c);
        break;
    case SCR_MESSAGE:
    default:
        dock_ui_screen_message(c, u->msg_title, u->msg_body);
        break;
    }
}

bool settings_ui_tick(settings_ui_t *u, uint64_t now_ms)
{
    const net_ap_t *aps;
    int n = 0, r;
    switch (u->screen) {
    case SCR_LOCKED:
        if (now_ms >= u->lock_until_ms) {
            prompt(u, SCR_PASSWORD);
            return true;
        }
        if (now_ms / 1000u != u->last_draw_s) {
            u->last_draw_s = now_ms / 1000u;
            return true;
        }
        return false;
    case SCR_SCANNING:
        r = net_scan_result(u->d->net, &aps, &n);
        if (r == NET_RESULT_OK && n > 0)
            go(u, SCR_WIFI_LIST);
        else if (r == NET_RESULT_OK)
            message(u, "Wi-Fi", "No networks found.", SCR_MENU);
        else if (r == NET_RESULT_FAIL)
            message(u, "Wi-Fi", "Scan failed.", SCR_MENU);
        return r == NET_RESULT_OK || r == NET_RESULT_FAIL;
    case SCR_CONNECTING:
        r = net_connect_result(u->d->net);
        if (r == NET_RESULT_OK)
            message(u, "Wi-Fi", "Connected.", SCR_MENU);
        else if (r == NET_RESULT_FAIL)
            message(u, "Wi-Fi", "Could not connect.\nCheck the password.", SCR_MENU);
        return r == NET_RESULT_OK || r == NET_RESULT_FAIL;
    case SCR_DIAG:
        overlay_update_stats(u->d, now_ms);
        return true; /* refresh the live stats block */
    case SCR_UPDATE_PROGRESS:
        return true; /* keep the bar moving; main.c updates the percent */
    default:
        return false;
    }
}

int settings_ui_timeout_ms(const settings_ui_t *u)
{
    switch (u->screen) {
    case SCR_LOCKED:
    case SCR_DIAG:
        return 250;
    case SCR_SCANNING:
    case SCR_CONNECTING:
    case SCR_UPDATE_PROGRESS:
        return 200;
    default:
        return -1;
    }
}
