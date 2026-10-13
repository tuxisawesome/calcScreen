/*
 * dock_api.h - public API for programs running on a docked device.
 *
 * This is the contract other calculator programs (and future MCU ports) code
 * against. Every call is synchronous: it sends one request to the dock and
 * waits for the answer while keeping the screen mirror running. Calls return
 * a dock_status (DOCK_OK == 0).
 *
 * On the TI-84 Plus CE, include calc/sdk/dock.h instead: it exposes exactly
 * these functions, routed to the resident DOCK driver.
 */
#ifndef DOCK_API_H
#define DOCK_API_H

#include "dock_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef dock_dev_t dock_t;

typedef struct dock_usb_info {
    uint16_t vid, pid;
    uint8_t dev_class;
    char name[40];
} dock_usb_info_t;

/* ---- session ---- */
/* DOCK_OK when connected and accepted, else DOCK_E_DISCONNECTED /
 * DOCK_E_JOIN_REQUIRED / DOCK_E_JOIN_BAD. */
int dock_status(dock_t *d);
/* Keep the link and mirror alive while your program is busy. */
void dock_service(dock_t *d);
/* Send screen changes right now (e.g. after drawing a frame). */
void dock_screen_push(dock_t *d);
void dock_screen_enable(dock_t *d, bool on);
/* Raw keyboard events from the dock's keyboard (full ASCII). */
bool dock_key_get(dock_t *d, dock_hidkey_t *k);

/* ---- audio ---- */
int dock_audio_open(dock_t *d, uint32_t rate, uint8_t fmt, uint8_t channels);
int dock_audio_write(dock_t *d, const void *pcm, size_t n, size_t *accepted);
int dock_audio_play_file(dock_t *d, const char *path);
int dock_audio_pos(dock_t *d, uint32_t *pos_ms, bool *playing);
int dock_audio_stop(dock_t *d);
int dock_audio_volume(dock_t *d, uint8_t percent);

/* ---- files on the dock's USB drives ----
 * Paths look like "DRIVE/dir/file.ext"; DRIVE is a name from dock_fs_drive().
 * A path without a drive name uses the first drive. */
int dock_fs_drive(dock_t *d, uint8_t index, char *name, size_t cap);
int dock_fs_open(dock_t *d, const char *path, uint8_t mode, uint8_t *handle);
int dock_fs_read(dock_t *d, uint8_t handle, void *dst, size_t max, size_t *got);
int dock_fs_write(dock_t *d, uint8_t handle, const void *src, size_t n,
                  size_t *written);
int dock_fs_seek(dock_t *d, uint8_t handle, int32_t offset, uint8_t whence,
                 uint32_t *pos);
int dock_fs_close(dock_t *d, uint8_t handle);
int dock_fs_stat(dock_t *d, const char *path, uint32_t *size, bool *is_dir);
int dock_fs_readdir(dock_t *d, const char *path, uint16_t index, char *name,
                    size_t cap, uint32_t *size, bool *is_dir);
int dock_fs_mkdir(dock_t *d, const char *path);
int dock_fs_remove(dock_t *d, const char *path);

/* ---- other USB devices plugged into the dock ---- */
int dock_usb_device(dock_t *d, uint8_t index, dock_usb_info_t *info);

/* ---- network (needs Wi-Fi and the network API enabled in dock settings) ---- */
int dock_net_status(dock_t *d, bool *up, char *ssid, size_t ssid_cap, char *ip,
                    size_t ip_cap);
int dock_http_begin(dock_t *d, uint8_t method, const char *url,
                    const char *headers, uint32_t body_len, uint8_t *handle);
int dock_http_write(dock_t *d, uint8_t handle, const void *data, size_t n);
int dock_http_response(dock_t *d, uint8_t handle, uint16_t *http_status,
                       uint32_t *content_len);
int dock_http_read(dock_t *d, uint8_t handle, void *dst, size_t max, size_t *got);
int dock_http_end(dock_t *d, uint8_t handle);
/* Convenience wrappers. */
int dock_http_get(dock_t *d, const char *url, void *dst, size_t max,
                  size_t *got, uint16_t *http_status);
int dock_http_put(dock_t *d, const char *url, const void *body, size_t n,
                  uint16_t *http_status);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_API_H */
