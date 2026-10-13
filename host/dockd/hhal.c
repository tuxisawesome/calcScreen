/*
 * hhal.c - the dock_hhal_* services (core/dock_host_hal.h) for the Linux
 * dock. `hal` is the dockd_t; every call delegates to a hal_linux module.
 */
#define _GNU_SOURCE
#include "dockd.h"
#include "usb_devices.h"
#include "util.h"

#include <string.h>

/* Wait this long for a slow HTTP server before answering DOCK_E_BUSY; the
 * calculator gives up on a request after 3 s. */
#define HTTP_WAIT_MS 2500

static dockd_t *D(void *hal)
{
    return (dockd_t *)hal;
}

/* Let a slow server catch up while the calculator waits, keeping audio fed. */
static void http_wait(dockd_t *d, uint8_t handle, bool want_data)
{
    uint64_t end = mono_ms() + HTTP_WAIT_MS;
    while (!net_http_ready(d->net, handle, want_data) && mono_ms() < end) {
        net_http_wait(d->net, handle, want_data, 20);
        audio_pump(d->au);
    }
}

void dockd_end_session(dockd_t *d)
{
    storage_close_all(d->st);
    audio_stop(d->au);
    audio_set_program_volume(d->au, 100);
    net_http_end_all(d->net);
}

/* ---- link ---- */

bool dock_hhal_tx(void *hal, const void *p, size_t n)
{
    dockd_t *d = D(hal);
    if (d->ux)
        return unix_link_tx(d->ux, p, n);
    if (d->usb)
        return usb_link_tx(d->usb, p, n);
    return false;
}

/* ---- session ---- */

void dock_hhal_event(void *hal, int event)
{
    dockd_t *d = D(hal);
    switch (event) {
    case DOCK_HEV_CONNECTED:
        logv("calc %08x connected (driver %u, flags %02x)", d->host.calc_id,
             d->host.driver_ver, d->host.hello_flags);
        d->mode = MODE_FAST;
        break;
    case DOCK_HEV_JOIN_NEEDED:
        logv("calc %08x needs the join code", d->host.calc_id);
        dockd_end_session(d);
        d->mode = MODE_JOIN;
        break;
    case DOCK_HEV_BYE:
        logv("calc said bye");
        dockd_end_session(d);
        d->mode = MODE_WAITING;
        break;
    case DOCK_HEV_FRAME:
        d->frames++;
        display_frame_end(d->disp);
        if (d->opt.once_frames > 0 && d->frames >= d->opt.once_frames)
            d->quit = true;
        return;
    default:
        return;
    }
    dockd_redraw(d);
}

int dock_hhal_join_check(void *hal, uint32_t calc_id, const char code[6])
{
    dockd_t *d = D(hal);
    static const char none[6] = {0};
    (void)calc_id;
    if (!d->s.join_required)
        return DOCK_OK;
    if (memcmp(code, none, 6) == 0)
        return DOCK_E_JOIN_REQUIRED;
    return dock_ct_equal((const uint8_t *)code, (const uint8_t *)d->s.join_code, 6)
               ? DOCK_OK
               : DOCK_E_JOIN_BAD;
}

uint32_t dock_hhal_dock_id(void *hal)
{
    return D(hal)->s.dock_id;
}

bool dockd_net_enabled(const dockd_t *d)
{
    return d->s.wifi_enabled && d->s.net_api_enabled;
}

uint16_t dock_hhal_caps(void *hal)
{
    dockd_t *d = D(hal);
    uint16_t caps = DOCK_CAP_AUDIO | DOCK_CAP_FS | DOCK_CAP_USB | DOCK_CAP_KEYS;
    if (dockd_net_enabled(d))
        caps |= DOCK_CAP_NET;
    return caps;
}

void dock_hhal_log(void *hal, const char *text)
{
    char line[128];
    size_t i;
    (void)hal;
    if (!log_is_verbose())
        return;
    str_copy(line, sizeof line, text);
    for (i = 0; line[i]; i++)
        if ((unsigned char)line[i] < 0x20 || (unsigned char)line[i] > 0x7E)
            line[i] = '?';
    logv("calc: %s", line);
}

/* ---- display ---- */

void dock_hhal_display_damage(void *hal, uint16_t y0, uint16_t count)
{
    display_damage(D(hal)->disp, DISPLAY_MIRROR, y0, count);
}

void dock_hhal_display_mode(void *hal, uint16_t width, uint16_t height)
{
    display_set_mirror_size(D(hal)->disp, width, height);
}

/* ---- audio ---- */

int dock_hhal_audio_open(void *hal, uint32_t rate, uint8_t fmt, uint8_t channels)
{
    return audio_stream_open(D(hal)->au, rate, fmt, channels);
}

int dock_hhal_audio_write(void *hal, const uint8_t *pcm, size_t n, size_t *accepted)
{
    return audio_stream_write(D(hal)->au, pcm, n, accepted);
}

int dock_hhal_audio_play_file(void *hal, const char *path)
{
    dockd_t *d = D(hal);
    int fd = -1, rc = storage_open_ro_fd(d->st, path, &fd);
    if (rc != DOCK_OK)
        return rc;
    return audio_play_wav_fd(d->au, fd);
}

int dock_hhal_audio_pos(void *hal, uint32_t *pos_ms, bool *playing)
{
    return audio_pos(D(hal)->au, pos_ms, playing);
}

int dock_hhal_audio_stop(void *hal)
{
    return audio_stop(D(hal)->au);
}

int dock_hhal_audio_volume(void *hal, uint8_t percent)
{
    return audio_set_program_volume(D(hal)->au, percent);
}

/* ---- storage ---- */

int dock_hhal_fs_drive(void *hal, uint8_t index, char *name, size_t cap)
{
    return storage_drive(D(hal)->st, index, name, cap);
}

int dock_hhal_fs_open(void *hal, const char *path, uint8_t mode, uint8_t *handle)
{
    return storage_fopen(D(hal)->st, path, mode, handle);
}

int dock_hhal_fs_read(void *hal, uint8_t handle, uint8_t *dst, size_t max, size_t *got)
{
    return storage_read(D(hal)->st, handle, dst, max, got);
}

int dock_hhal_fs_write(void *hal, uint8_t handle, const uint8_t *src, size_t n,
                       size_t *written)
{
    return storage_write(D(hal)->st, handle, src, n, written);
}

int dock_hhal_fs_seek(void *hal, uint8_t handle, int32_t offset, uint8_t whence,
                      uint32_t *pos)
{
    return storage_seek(D(hal)->st, handle, offset, whence, pos);
}

int dock_hhal_fs_close(void *hal, uint8_t handle)
{
    return storage_fclose(D(hal)->st, handle);
}

int dock_hhal_fs_stat(void *hal, const char *path, uint32_t *size, bool *is_dir)
{
    return storage_stat(D(hal)->st, path, size, is_dir);
}

int dock_hhal_fs_readdir(void *hal, const char *path, uint16_t index, char *name,
                         size_t cap, uint32_t *size, bool *is_dir)
{
    return storage_readdir(D(hal)->st, path, index, name, cap, size, is_dir);
}

int dock_hhal_fs_mkdir(void *hal, const char *path)
{
    return storage_mkdir(D(hal)->st, path);
}

int dock_hhal_fs_remove(void *hal, const char *path)
{
    return storage_remove(D(hal)->st, path);
}

/* ---- other USB devices ---- */

int dock_hhal_usb_device(void *hal, uint8_t index, uint16_t *vid, uint16_t *pid,
                         uint8_t *dev_class, char *name, size_t cap)
{
    return usb_devices_get(D(hal)->opt.usb_sysfs, index, vid, pid, dev_class, name, cap);
}

/* ---- network ---- */

int dock_hhal_net_status(void *hal, bool *up, char *ssid, size_t ssid_cap, char *ip,
                         size_t ip_cap)
{
    dockd_t *d = D(hal);
    if (!dockd_net_enabled(d))
        return DOCK_E_NET_DISABLED;
    net_get_status(d->net, up, ssid, ssid_cap, ip, ip_cap);
    return DOCK_OK;
}

int dock_hhal_http_begin(void *hal, uint8_t method, const char *url,
                         const char *headers, uint32_t body_len, uint8_t *handle)
{
    dockd_t *d = D(hal);
    if (!dockd_net_enabled(d))
        return DOCK_E_NET_DISABLED;
    return net_http_begin(d->net, method, url, headers, body_len, handle);
}

int dock_hhal_http_body(void *hal, uint8_t handle, const uint8_t *data, size_t n)
{
    dockd_t *d = D(hal);
    if (!dockd_net_enabled(d))
        return DOCK_E_NET_DISABLED;
    return net_http_body(d->net, handle, data, n);
}

int dock_hhal_http_response(void *hal, uint8_t handle, uint16_t *http_status,
                            uint32_t *content_len)
{
    dockd_t *d = D(hal);
    if (!dockd_net_enabled(d))
        return DOCK_E_NET_DISABLED;
    /* The calculator is blocked waiting for this answer anyway. */
    http_wait(d, handle, false);
    return net_http_response(d->net, handle, http_status, content_len);
}

int dock_hhal_http_read(void *hal, uint8_t handle, uint8_t *dst, size_t max, size_t *got)
{
    dockd_t *d = D(hal);
    *got = 0;
    if (!dockd_net_enabled(d))
        return DOCK_E_NET_DISABLED;
    http_wait(d, handle, true);
    return net_http_read(d->net, handle, dst, max, got);
}

int dock_hhal_http_end(void *hal, uint8_t handle)
{
    /* Always allowed, so handles never leak when the API is switched off. */
    return net_http_end(D(hal)->net, handle);
}
