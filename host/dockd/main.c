/*
 * dockd - the calcScreen dock daemon (Raspberry Pi side).
 *
 * One single-threaded poll() loop drives everything: the calculator link
 * (USB via libusb, or a Unix socket for the simulator), keyboards (evdev),
 * the display (DRM), audio (ALSA), Wi-Fi/HTTP (nmcli/libcurl) and systemd.
 * The portable protocol engine lives in core/dock_host.c; this file owns the
 * mode state machine and keyboard routing.
 *
 *   WAITING ---DOCK device--->  (HELLO) --ok--> FAST
 *      |                                 \--refused--> JOIN
 *      \--stock calc (0451:E008)--> DUSB  (or DUSB_BLOCKED when join codes
 *                                          are required)
 *   The settings page is an overlay over any mode; keys go to it while open.
 */
#define _GNU_SOURCE
#include "dockd.h"
#include "dock_keymap.h"
#include "util.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PFD 96
#define RESP_BUF (64u * 1024u + 64u)
#define LOOP_MAX_WAIT_MS 500
#define DISPLAY_RETRY_MS 3000u

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ------------------------------------------------------------------ */
/* options                                                             */

static void usage(FILE *f)
{
    fputs("usage: dockd [options]\n"
          "  -v, --verbose          log to stderr\n"
          "  --drives DIR           USB drive mount root (/media/dock)\n"
          "  --state DIR            settings directory (/var/lib/dockd)\n"
          "  --display SPEC         drm | drm:/dev/dri/cardN | none | ppm:PATH (drm)\n"
          "  --audio NAME           alsa | null (alsa)\n"
          "  --audio-device PCM     ALSA device for both outputs\n"
          "  --audio-hdmi PCM       HDMI output (hdmi:CARD=vc4hdmi0,DEV=0)\n"
          "  --audio-jack PCM       headphone output (plughw:CARD=Headphones,DEV=0)\n"
          "  --transport T          usb | unix:/path/to/socket (usb)\n"
          "  --shell-cmd CMD        drop-to-shell command\n"
          "                         (openvt -s -w -- /bin/login -f dock)\n"
          "  --nmcli PATH|none      NetworkManager CLI (nmcli)\n"
          "  --wifi-if IFACE        Wi-Fi interface (wlan0)\n"
          "  --input-dir DIR        evdev directory (/dev/input)\n"
          "  --usb-sysfs DIR        USB device list (/sys/bus/usb/devices)\n"
          "  --once-frames N        exit after N received frames (tests)\n",
          f);
}

static bool parse_opts(int argc, char **argv, dockd_opts_t *o)
{
    enum {
        O_DRIVES = 256, O_STATE, O_DISPLAY, O_AUDIO, O_AUDIO_DEV, O_AUDIO_HDMI,
        O_AUDIO_JACK, O_TRANSPORT, O_SHELL, O_NMCLI, O_WIFI_IF, O_INPUT, O_USB_SYSFS,
        O_ONCE
    };
    static const struct option lo[] = {
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {"drives", required_argument, NULL, O_DRIVES},
        {"state", required_argument, NULL, O_STATE},
        {"display", required_argument, NULL, O_DISPLAY},
        {"audio", required_argument, NULL, O_AUDIO},
        {"audio-device", required_argument, NULL, O_AUDIO_DEV},
        {"audio-hdmi", required_argument, NULL, O_AUDIO_HDMI},
        {"audio-jack", required_argument, NULL, O_AUDIO_JACK},
        {"transport", required_argument, NULL, O_TRANSPORT},
        {"shell-cmd", required_argument, NULL, O_SHELL},
        {"nmcli", required_argument, NULL, O_NMCLI},
        {"wifi-if", required_argument, NULL, O_WIFI_IF},
        {"input-dir", required_argument, NULL, O_INPUT},
        {"usb-sysfs", required_argument, NULL, O_USB_SYSFS},
        {"once-frames", required_argument, NULL, O_ONCE},
        {NULL, 0, NULL, 0}};
    int c;

    o->drives = "/media/dock";
    o->state = "/var/lib/dockd";
    o->display = "drm";
    o->audio = "alsa";
    o->audio_device = NULL;
    o->audio_hdmi = "hdmi:CARD=vc4hdmi0,DEV=0";
    o->audio_jack = "plughw:CARD=Headphones,DEV=0";
    o->transport = "usb";
    o->shell_cmd = "openvt -s -w -- /bin/login -f dock";
    o->nmcli = "nmcli";
    o->wifi_if = "wlan0";
    o->input_dir = "/dev/input";
    o->usb_sysfs = "/sys/bus/usb/devices";
    o->once_frames = 0;

    while ((c = getopt_long(argc, argv, "vh", lo, NULL)) != -1) {
        switch (c) {
        case 'v': log_set_verbose(true); break;
        case 'h': usage(stdout); exit(0);
        case O_DRIVES: o->drives = optarg; break;
        case O_STATE: o->state = optarg; break;
        case O_DISPLAY: o->display = optarg; break;
        case O_AUDIO: o->audio = optarg; break;
        case O_AUDIO_DEV: o->audio_device = optarg; break;
        case O_AUDIO_HDMI: o->audio_hdmi = optarg; break;
        case O_AUDIO_JACK: o->audio_jack = optarg; break;
        case O_TRANSPORT: o->transport = optarg; break;
        case O_SHELL: o->shell_cmd = optarg; break;
        case O_NMCLI: o->nmcli = optarg; break;
        case O_WIFI_IF: o->wifi_if = optarg; break;
        case O_INPUT: o->input_dir = optarg; break;
        case O_USB_SYSFS: o->usb_sysfs = optarg; break;
        case O_ONCE: o->once_frames = strtol(optarg, NULL, 10); break;
        default: usage(stderr); return false;
        }
    }
    if (optind != argc) {
        usage(stderr);
        return false;
    }
    if (strcmp(o->transport, "usb") != 0 && strncmp(o->transport, "unix:", 5) != 0) {
        loge("unknown transport '%s'", o->transport);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* shared helpers (dockd.h)                                            */

static const char *audio_device(const dockd_t *d)
{
    if (d->opt.audio_device)
        return d->opt.audio_device;
    return d->s.audio_out == DOCK_AUDIO_JACK ? d->opt.audio_jack : d->opt.audio_hdmi;
}

void dockd_redraw(dockd_t *d)
{
    d->ui_dirty = true;
}

void dockd_settings_changed(dockd_t *d)
{
    if (!dock_settings_save(&d->store.io, &d->s, &d->seq))
        loge("settings could not be saved");
    audio_set_device(d->au, audio_device(d));
    audio_set_master_volume(d->au, d->s.volume);
    if (d->usb)
        usb_link_set_dusb_allowed(d->usb, !d->s.join_required);
    if (!dockd_net_enabled(d))
        net_http_end_all(d->net);
    dockd_redraw(d);
}

static void open_settings(dockd_t *d)
{
    d->settings_open = true;
    settings_ui_open(&d->sui);
    dockd_redraw(d);
}

void dockd_close_settings(dockd_t *d)
{
    d->settings_open = false;
    settings_ui_wipe(&d->sui);
    dockd_redraw(d);
}

void dockd_request_shell(dockd_t *d)
{
    d->shell_requested = true;
}

void dockd_request_reboot(dockd_t *d)
{
    char *argv[] = {"systemctl", "reboot", NULL};
    if (d->reboot_pid > 0)
        return;
    d->reboot_pid = spawn_argv(argv, NULL, false);
    if (d->reboot_pid < 0)
        loge("cannot run systemctl reboot: %s", strerror(errno));
}

/* ------------------------------------------------------------------ */
/* link callbacks                                                      */

static void on_fast_up(void *user)
{
    dockd_t *d = user;
    d->fast_link = true;
    dock_host_reset(&d->host);
    dockd_end_session(d);
    d->mode = MODE_WAITING; /* until HELLO */
    dockd_redraw(d);
}

static void on_fast_rx(void *user, const uint8_t *p, size_t n)
{
    dockd_t *d = user;
    dock_host_feed(&d->host, p, n);
}

static void on_fast_down(void *user)
{
    dockd_t *d = user;
    d->fast_link = false;
    dock_host_reset(&d->host);
    dockd_end_session(d);
    d->mode = MODE_WAITING;
    dockd_redraw(d);
}

static void on_dusb_up(void *user)
{
    dockd_t *d = user;
    dusb_session_start(&d->dusb);
    d->mode = MODE_DUSB;
    dockd_redraw(d);
}

static void on_dusb_down(void *user)
{
    dockd_t *d = user;
    dusb_session_stop(&d->dusb);
    if (d->mode == MODE_DUSB)
        d->mode = MODE_WAITING;
    dockd_redraw(d);
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */

static void on_key(void *user, uint8_t usage, uint8_t mods, int value)
{
    dockd_t *d = user;
    bool press = value != INPUT_UP;
    dock_ti_key_t k;

    if (d->shell_active)
        return;
    if (press) {
        switch (dock_keymap_hotkey(usage, mods)) {
        case DOCK_HOTKEY_SETTINGS:
            if (value == INPUT_DOWN) {
                if (d->settings_open)
                    dockd_close_settings(d);
                else
                    open_settings(d);
            }
            return;
        case DOCK_HOTKEY_REFRESH:
            if (d->mode == MODE_FAST)
                dock_host_request_refresh(&d->host);
            return;
        default:
            break;
        }
    }
    if (d->settings_open) {
        if (press)
            settings_ui_key(&d->sui, usage, mods, dock_keymap_ascii(usage, mods));
        return;
    }
    if (d->mode == MODE_FAST && d->host.accepted) {
        if (press && dock_keymap_translate(usage, mods, &k))
            dock_host_send_key(&d->host, k.tikey, k.scancode, k.flags);
        /* raw events for dock-aware programs */
        dock_host_send_hidkey(&d->host, usage, mods, press,
                              press ? dock_keymap_ascii(usage, mods) : 0);
    } else if (d->mode == MODE_DUSB && press) {
        if (dock_keymap_translate(usage, mods, &k) && k.tikey)
            dusb_session_queue_key(&d->dusb, k.tikey);
    }
}

/* ------------------------------------------------------------------ */
/* screens                                                             */

static void render_ui(dockd_t *d)
{
    const dock_canvas_t *c = &d->ui;
    char body[160];
    if (d->settings_open) {
        settings_ui_render(&d->sui, c);
    } else {
        switch (d->mode) {
        case MODE_JOIN:
            dock_ui_screen_join(c, d->s.join_code);
            break;
        case MODE_DUSB_BLOCKED:
            snprintf(body, sizeof body,
                     "This dock requires a join code.\n\nStart DOCK on the calculator\n"
                     "and enter the code %s.",
                     d->s.join_code);
            dock_ui_screen_message(c, "Join code required", body);
            break;
        case MODE_DUSB:
            dock_ui_screen_waiting(c, "Calculator found", "Connecting over the TI link...");
            break;
        default:
            dock_ui_screen_waiting(c,
                                   d->fast_link ? "Calculator connected, starting DOCK..."
                                                : "Connect your TI-84 Plus CE",
                                   "Settings: Ctrl+Alt+S or F12");
            break;
        }
    }
    display_damage(d->disp, DISPLAY_UI, 0, DISPLAY_H);
}

static int wanted_view(const dockd_t *d)
{
    if (d->settings_open)
        return DISPLAY_UI;
    if (d->mode == MODE_FAST || (d->mode == MODE_DUSB && d->dusb.have_frame))
        return DISPLAY_MIRROR;
    return DISPLAY_UI;
}

/* A stock calculator is plugged in but DUSB is not allowed. */
static void update_idle_mode(dockd_t *d)
{
    bool blocked;
    int m;
    if (d->mode != MODE_WAITING && d->mode != MODE_DUSB_BLOCKED)
        return;
    blocked = d->usb && !d->fast_link && d->s.join_required &&
              usb_link_dusb_present(d->usb);
    m = blocked ? MODE_DUSB_BLOCKED : MODE_WAITING;
    if (m != d->mode) {
        d->mode = m;
        dockd_redraw(d);
    }
}

/* Point every writer of the mirror/UI at the display's buffers. */
static void bind_display(dockd_t *d)
{
    uint32_t stride;
    d->mirror = display_buffer(d->disp, DISPLAY_MIRROR, &d->mirror_stride);
    d->host.scr.fb = d->mirror;
    d->host.scr.fb_stride = d->mirror_stride;
    d->dusb.conv.fb = d->mirror;
    d->dusb.conv.fb_stride = d->mirror_stride;
    d->ui.px = display_buffer(d->disp, DISPLAY_UI, &stride);
    d->ui.w = DISPLAY_W;
    d->ui.h = DISPLAY_H;
    d->ui.stride = stride;
}

/* The monitor was not ready at start-up: keep trying to open DRM. */
static void retry_display(dockd_t *d, bool *fallback, uint64_t *next_ms, uint64_t now)
{
    display_t *nd;
    uint32_t os, ns, y;
    uint32_t *om, *nm;
    if (!*fallback || now < *next_ms)
        return;
    *next_ms = now + DISPLAY_RETRY_MS;
    nd = display_open(d->opt.display);
    if (!nd)
        return;
    om = display_buffer(d->disp, DISPLAY_MIRROR, &os);
    nm = display_buffer(nd, DISPLAY_MIRROR, &ns);
    for (y = 0; y < DISPLAY_H; y++)
        memcpy(nm + (size_t)y * ns, om + (size_t)y * os, DISPLAY_W * 4u);
    display_close(d->disp);
    d->disp = nd;
    bind_display(d);
    display_damage(d->disp, DISPLAY_MIRROR, 0, DISPLAY_H);
    *fallback = false;
    dockd_redraw(d);
    logv("display: now on %s", d->opt.display);
}

/* ------------------------------------------------------------------ */
/* drop to shell                                                       */

static void pause_everything(dockd_t *d, bool pause)
{
    if (d->usb)
        usb_link_set_paused(d->usb, pause);
    if (d->ux)
        unix_link_set_paused(d->ux, pause);
    input_set_grab(d->in, !pause);
    if (pause) {
        audio_stop(d->au);
        display_release(d->disp);
    } else {
        display_acquire(d->disp);
        display_damage(d->disp, DISPLAY_MIRROR, 0, DISPLAY_H);
        dockd_redraw(d);
    }
}

static void start_shell(dockd_t *d)
{
    char buf[512], *argv[32];
    d->shell_requested = false;
    str_copy(buf, sizeof buf, d->opt.shell_cmd);
    if (split_words(buf, argv, 32) == 0)
        return;
    dockd_close_settings(d);
    pause_everything(d, true);
    d->shell_pid = spawn_argv(argv, NULL, true);
    if (d->shell_pid < 0) {
        loge("cannot run %s: %s", argv[0], strerror(errno));
        pause_everything(d, false);
        return;
    }
    d->shell_active = true;
    logv("shell started (pid %d)", (int)d->shell_pid);
}

static void reap_children(dockd_t *d)
{
    int st;
    if (d->shell_active && waitpid(d->shell_pid, &st, WNOHANG) == d->shell_pid) {
        logv("shell finished");
        d->shell_active = false;
        d->shell_pid = 0;
        pause_everything(d, false);
    }
    if (d->reboot_pid > 0 && waitpid(d->reboot_pid, &st, WNOHANG) == d->reboot_pid)
        d->reboot_pid = 0;
}

/* ------------------------------------------------------------------ */
/* loop                                                                */

static void tmin(int *t, int v)
{
    if (v >= 0 && (*t < 0 || v < *t))
        *t = v;
}

static int loop_timeout(dockd_t *d, uint64_t now)
{
    int t = LOOP_MAX_WAIT_MS;
    if (d->usb)
        tmin(&t, usb_link_timeout_ms(d->usb, now));
    tmin(&t, net_timeout_ms(d->net, now));
    tmin(&t, audio_timeout_ms(d->au));
    tmin(&t, sdnotify_timeout_ms(&d->sd, now));
    if (d->settings_open)
        tmin(&t, settings_ui_timeout_ms(&d->sui));
    if (d->shell_active || d->reboot_pid > 0)
        tmin(&t, 200);
    if (d->mode == MODE_DUSB && !d->settings_open && !d->shell_active)
        t = 0; /* the DUSB step itself is the (bounded) wait */
    return t;
}

static void run(dockd_t *d, bool display_fallback)
{
    uint64_t display_retry_ms = 0;
    while (!g_quit && !d->quit) {
        struct pollfd pfd[MAX_PFD];
        int n = 0, in_n, usb_n = 0, ux_n = 0, net_n;
        int in_0, usb_0, ux_0, net_0;
        uint64_t now = mono_ms();

        in_0 = n;
        n += in_n = input_pollfds(d->in, pfd + n, MAX_PFD - n);
        usb_0 = n;
        if (d->usb)
            n += usb_n = usb_link_pollfds(d->usb, pfd + n, MAX_PFD - n);
        ux_0 = n;
        if (d->ux)
            n += ux_n = unix_link_pollfds(d->ux, pfd + n, MAX_PFD - n);
        net_0 = n;
        n += net_n = net_pollfds(d->net, pfd + n, MAX_PFD - n);

        if (poll(pfd, (nfds_t)n, loop_timeout(d, now)) < 0 && errno != EINTR) {
            loge("poll: %s", strerror(errno));
            break;
        }
        now = mono_ms();
        (void)usb_0;
        (void)usb_n;

        input_handle(d->in, pfd + in_0, in_n);
        input_tick(d->in, now);
        if (d->usb)
            usb_link_handle(d->usb, now);
        if (d->ux)
            unix_link_handle(d->ux, pfd + ux_0, ux_n);
        net_handle(d->net, pfd + net_0, net_n);
        net_tick(d->net, now);
        audio_pump(d->au);

        if (d->mode == MODE_DUSB && !d->settings_open && !d->shell_active &&
            dusb_session_step(&d->dusb)) {
            display_damage(d->disp, DISPLAY_MIRROR, 0, DISPLAY_H);
            display_frame_end(d->disp);
            dockd_redraw(d); /* the view may switch to the mirror */
        }
        if (d->settings_open && settings_ui_tick(&d->sui, now))
            dockd_redraw(d);
        if (d->shell_requested)
            start_shell(d);
        reap_children(d);
        update_idle_mode(d);
        retry_display(d, &display_fallback, &display_retry_ms, now);

        if (!d->shell_active) {
            if (d->ui_dirty) {
                d->ui_dirty = false;
                render_ui(d);
            }
            if (wanted_view(d) != display_shown(d->disp))
                display_show(d->disp, wanted_view(d));
        }
        display_flush(d->disp);
        if (d->usb)
            usb_link_flush(d->usb);
        if (d->ux)
            unix_link_flush(d->ux);
        sdnotify_tick(&d->sd, now);
    }
}

/* ------------------------------------------------------------------ */
/* setup / teardown                                                    */

static bool open_transport(dockd_t *d)
{
    if (strcmp(d->opt.transport, "usb") == 0) {
        usb_link_cb_t cb = {d, on_fast_up, on_fast_rx, on_fast_down, on_dusb_up, on_dusb_down};
        d->usb = usb_link_open(&cb);
        if (!d->usb)
            return false;
        usb_link_set_dusb_allowed(d->usb, !d->s.join_required);
    } else {
        unix_link_cb_t cb = {d, on_fast_up, on_fast_rx, on_fast_down};
        d->ux = unix_link_open(d->opt.transport + 5, &cb);
        if (!d->ux)
            return false;
    }
    return true;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

static void teardown(dockd_t *d)
{
    if (d->in)
        input_close(d->in);
    if (d->usb)
        usb_link_close(d->usb);
    if (d->ux)
        unix_link_close(d->ux);
    dusb_session_free(&d->dusb);
    if (d->net)
        net_close(d->net);
    if (d->au)
        audio_close(d->au);
    if (d->st)
        storage_close(d->st);
    if (d->disp)
        display_close(d->disp);
    sdnotify_close(&d->sd);
    free(d->rxbuf);
    free(d->respbuf);
}

int main(int argc, char **argv)
{
    static dockd_t dd;
    dockd_t *d = &dd;
    bool display_fallback = false;
    int rc = 1;

    if (!parse_opts(argc, argv, &d->opt))
        return 2;
    install_signals();
    sdnotify_init(&d->sd);

    if (!settings_store_init(&d->store, d->opt.state))
        loge("state directory unusable; settings will not persist");
    settings_store_load(&d->store, &d->s, &d->seq);

    d->disp = display_open(d->opt.display);
    if (!d->disp && strncmp(d->opt.display, "drm", 3) == 0) {
        /* No monitor yet: run headless and keep retrying. */
        display_fallback = true;
        d->disp = display_open("none");
    }
    d->st = storage_open(d->opt.drives);
    d->au = audio_open(d->opt.audio, audio_device(d));
    d->net = net_open(d->opt.nmcli, d->opt.wifi_if);
    d->rxbuf = malloc(DOCK_HDR_LEN + DOCK_MAX_PAYLOAD);
    d->respbuf = malloc(RESP_BUF);
    if (!d->disp || !d->st || !d->au || !d->net || !d->rxbuf || !d->respbuf)
        goto out;
    audio_set_master_volume(d->au, d->s.volume);

    d->mirror = display_buffer(d->disp, DISPLAY_MIRROR, &d->mirror_stride);
    dock_host_init(&d->host, d, d->rxbuf, DOCK_HDR_LEN + DOCK_MAX_PAYLOAD, d->respbuf,
                   RESP_BUF, d->mirror, DISPLAY_W, DISPLAY_H, d->mirror_stride);
    settings_ui_init(&d->sui, d);
    d->in = input_open(d->opt.input_dir, on_key, d);
    if (!d->in || !open_transport(d) ||
        !dusb_session_init(&d->dusb, d->usb, d->mirror, d->mirror_stride))
        goto out;
    bind_display(d);
    if (net_control_available(d->net))
        net_set_radio(d->net, d->s.wifi_enabled);

    d->mode = MODE_WAITING;
    dockd_redraw(d);
    sdnotify_ready(&d->sd);
    logv("dockd ready (dock id %08x, transport %s)", d->s.dock_id, d->opt.transport);
    run(d, display_fallback);
    sdnotify_send(&d->sd, "STOPPING=1");
    rc = 0;
out:
    if (rc)
        loge("initialisation failed");
    teardown(d);
    return rc;
}
