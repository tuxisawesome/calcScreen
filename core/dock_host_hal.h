/*
 * dock_host_hal.h - services a *dock* platform provides to the portable host
 * engine (dock_host.c). The Raspberry Pi implements these in host/hal_linux;
 * a microcontroller dock would implement the same set.
 *
 * All functions get the opaque `hal` pointer passed to dock_host_init().
 * Functions returning int return an enum dock_status.
 */
#ifndef DOCK_HOST_HAL_H
#define DOCK_HOST_HAL_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- link ---- */
/* Queue bytes towards the device. May block briefly; returns false if the
 * link is gone. */
bool dock_hhal_tx(void *hal, const void *p, size_t n);

/* ---- session ---- */
enum dock_host_event {
    DOCK_HEV_CONNECTED = 1,  /* device accepted (WELCOME OK sent) */
    DOCK_HEV_JOIN_NEEDED,    /* device refused: show the join code */
    DOCK_HEV_BYE,            /* device released the link */
    DOCK_HEV_FRAME           /* a full screen pass finished */
};
void dock_hhal_event(void *hal, int event);
/* Decide whether a device may connect. code is 6 bytes, all '\0' if none.
 * Return DOCK_OK, DOCK_E_JOIN_REQUIRED or DOCK_E_JOIN_BAD. */
int dock_hhal_join_check(void *hal, uint32_t calc_id, const char code[6]);
uint32_t dock_hhal_dock_id(void *hal);
uint16_t dock_hhal_caps(void *hal); /* DOCK_CAP_* currently enabled */
void dock_hhal_log(void *hal, const char *text);

/* ---- display: rows y0..y0+count-1 of the XRGB8888 buffer changed ---- */
void dock_hhal_display_damage(void *hal, uint16_t y0, uint16_t count);
void dock_hhal_display_mode(void *hal, uint16_t width, uint16_t height);

/* ---- audio ---- */
int dock_hhal_audio_open(void *hal, uint32_t rate, uint8_t fmt, uint8_t channels);
int dock_hhal_audio_write(void *hal, const uint8_t *pcm, size_t n, size_t *accepted);
int dock_hhal_audio_play_file(void *hal, const char *path);
int dock_hhal_audio_pos(void *hal, uint32_t *pos_ms, bool *playing);
int dock_hhal_audio_stop(void *hal);
int dock_hhal_audio_volume(void *hal, uint8_t percent);

/* ---- storage (paths are relative to the dock's drive root; the HAL must
 * reject anything escaping it) ---- */
int dock_hhal_fs_drive(void *hal, uint8_t index, char *name, size_t cap);
int dock_hhal_fs_open(void *hal, const char *path, uint8_t mode, uint8_t *handle);
int dock_hhal_fs_read(void *hal, uint8_t handle, uint8_t *dst, size_t max, size_t *got);
int dock_hhal_fs_write(void *hal, uint8_t handle, const uint8_t *src, size_t n,
                       size_t *written);
int dock_hhal_fs_seek(void *hal, uint8_t handle, int32_t offset, uint8_t whence,
                      uint32_t *pos);
int dock_hhal_fs_close(void *hal, uint8_t handle);
int dock_hhal_fs_stat(void *hal, const char *path, uint32_t *size, bool *is_dir);
int dock_hhal_fs_readdir(void *hal, const char *path, uint16_t index, char *name,
                         size_t cap, uint32_t *size, bool *is_dir);
int dock_hhal_fs_mkdir(void *hal, const char *path);
int dock_hhal_fs_remove(void *hal, const char *path);

/* ---- other USB devices ---- */
int dock_hhal_usb_device(void *hal, uint8_t index, uint16_t *vid, uint16_t *pid,
                         uint8_t *dev_class, char *name, size_t cap);

/* ---- network ---- */
int dock_hhal_net_status(void *hal, bool *up, char *ssid, size_t ssid_cap,
                         char *ip, size_t ip_cap);
int dock_hhal_http_begin(void *hal, uint8_t method, const char *url,
                         const char *headers, uint32_t body_len, uint8_t *handle);
int dock_hhal_http_body(void *hal, uint8_t handle, const uint8_t *data, size_t n);
int dock_hhal_http_response(void *hal, uint8_t handle, uint16_t *http_status,
                            uint32_t *content_len);
int dock_hhal_http_read(void *hal, uint8_t handle, uint8_t *dst, size_t max,
                        size_t *got);
int dock_hhal_http_end(void *hal, uint8_t handle);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_HOST_HAL_H */
