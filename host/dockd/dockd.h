/*
 * dockd.h - state of the dock daemon shared by main.c (loop and modes),
 * hhal.c (the dock_hhal_* services for core/dock_host.c) and settings_ui.c.
 */
#ifndef DOCKD_H
#define DOCKD_H

#include <sys/types.h>

#include "audio_alsa.h"
#include "display.h"
#include "dock_host.h"
#include "dock_settings.h"
#include "dock_ui.h"
#include "dusb_session.h"
#include "input_evdev.h"
#include "net.h"
#include "overlay.h"
#include "sdnotify.h"
#include "settings_store.h"
#include "settings_ui.h"
#include "storage.h"
#include "unix_link.h"
#include "update.h"
#include "usb_link.h"

enum dockd_mode {
    MODE_WAITING,      /* no calculator (or FAST link without HELLO yet) */
    MODE_FAST,         /* DOCK driver connected and accepted */
    MODE_JOIN,         /* DOCK driver refused: show the join code */
    MODE_DUSB,         /* stock calculator mirrored over DUSB */
    MODE_DUSB_BLOCKED  /* stock calculator, but join codes are required */
};

typedef struct dockd_opts {
    const char *drives;
    const char *state;
    const char *display;
    const char *audio;
    const char *audio_device; /* overrides both outputs when set */
    const char *audio_hdmi;
    const char *audio_jack;
    const char *transport;
    const char *shell_cmd;
    const char *nmcli;
    const char *wifi_if;
    const char *input_dir;
    const char *usb_sysfs;
    long once_frames;
    bool perf_overlay; /* --perf-overlay: force the diagnostics overlay on */
    int perf_sections; /* --perf-sections N: force section bitmask (-1 unset) */
} dockd_opts_t;

typedef struct dockd {
    dockd_opts_t opt;

    settings_store_t store;
    dock_settings_t s;
    uint32_t seq;

    display_t *disp;
    input_t *in;
    usb_link_t *usb;     /* --transport usb */
    unix_link_t *ux;     /* --transport unix:PATH */
    storage_t *st;
    audio_t *au;
    net_t *net;
    sdnotify_t sd;

    dock_host_t host;
    uint8_t *rxbuf, *respbuf;
    uint32_t *mirror;
    uint32_t mirror_stride;
    dock_canvas_t ui;

    int mode;            /* enum dockd_mode */
    bool fast_link;      /* a FAST transport is connected */
    bool ui_dirty;
    bool settings_open;
    settings_ui_t sui;
    dusb_session_t dusb;

    bool shell_requested;
    bool shell_active;
    pid_t shell_pid;
    pid_t reboot_pid;

    overlay_t ovl;

    /* USB update: the applier child (pi/dock-update) and its reported
     * progress, shown on SCR_UPDATE_PROGRESS. */
    pid_t update_pid;
    int update_fd;            /* child stdout pipe, or 0 when none */
    char update_line[160];    /* partial stdout line being assembled */
    size_t update_line_len;
    int update_pct;           /* 0..100 */
    char update_step[48];     /* current step label */

    /* One-shot result banner from a finished update (read once at startup). */
    char update_result[128];

    long frames;
    bool quit;
} dockd_t;

/* Persist d->s and apply its side effects (audio, DUSB policy, redraw). */
void dockd_settings_changed(dockd_t *d);
void dockd_redraw(dockd_t *d);
void dockd_close_settings(dockd_t *d);
void dockd_request_shell(dockd_t *d);
void dockd_request_reboot(dockd_t *d);
/* Spawn the privileged applier (pi/dock-update stage ...) as a child whose
 * stdout is polled in the loop. Returns false if it could not be started. */
bool dockd_start_update(dockd_t *d, const update_pkg_t *pkg);
/* True when calculator programs may use the network. */
bool dockd_net_enabled(const dockd_t *d);

/* hhal.c: the calculator session ended; release its resources. */
void dockd_end_session(dockd_t *d);

#endif /* DOCKD_H */
