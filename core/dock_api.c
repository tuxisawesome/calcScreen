/* dock_api.c - public API implemented on top of the device engine. */
#include "dock_api.h"

#define PATH_MAX_LEN 255u

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!dst || !cap)
        return;
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Build "u8 prefix (optional) + path\0" requests. */
static int path_req(dock_t *d, uint8_t type, const uint8_t *pre, size_t pre_len,
                    const char *path, uint8_t resp_fixed)
{
    uint8_t buf[8 + PATH_MAX_LEN + 1];
    dock_wr_t w;
    if (!path || strlen(path) > PATH_MAX_LEN)
        return DOCK_E_INVALID;
    dock_wr_init(&w, buf, sizeof buf);
    dock_wr_bytes(&w, pre, pre_len);
    dock_wr_str(&w, path);
    if (w.overflow)
        return DOCK_E_INVALID;
    return dock_dev_request(d, type, buf, w.len, NULL, 0, resp_fixed, NULL, 0);
}

static dock_rd_t resp_reader(dock_t *d)
{
    dock_rd_t r;
    dock_rd_init(&r, d->resp, d->resp_len);
    return r;
}

/* ---- session ---- */

int dock_status(dock_t *d)
{
    switch (d->state) {
    case DOCK_DEV_RUNNING:
        return DOCK_OK;
    case DOCK_DEV_NEED_JOIN:
        return d->last_welcome_status == DOCK_E_JOIN_BAD ? DOCK_E_JOIN_BAD
                                                         : DOCK_E_JOIN_REQUIRED;
    default:
        return DOCK_E_DISCONNECTED;
    }
}

void dock_service(dock_t *d)
{
    dock_dev_poll(d);
}

void dock_screen_push(dock_t *d)
{
    dock_dev_screen_push(d);
}

void dock_screen_enable(dock_t *d, bool on)
{
    if (on && !d->screen_enabled)
        dock_dev_screen_refresh(d);
    d->screen_enabled = on;
}

bool dock_key_get(dock_t *d, dock_hidkey_t *k)
{
    return dock_dev_hid_pop(d, k);
}

/* ---- audio ---- */

int dock_audio_open(dock_t *d, uint32_t rate, uint8_t fmt, uint8_t channels)
{
    uint8_t b[6];
    dock_put_u32(b, rate);
    b[4] = fmt;
    b[5] = channels;
    return dock_dev_request(d, DOCK_T_AUD_OPEN, b, sizeof b, NULL, 0, 0, NULL, 0);
}

int dock_audio_write(dock_t *d, const void *pcm, size_t n, size_t *accepted)
{
    int rc = dock_dev_request(d, DOCK_T_AUD_WRITE, NULL, 0, pcm, n, 0, NULL, 0);
    if (accepted) {
        dock_rd_t r = resp_reader(d);
        uint32_t v = dock_rd_u24(&r);
        *accepted = (rc == DOCK_OK && !r.underflow) ? (size_t)v : 0;
    }
    return rc;
}

int dock_audio_play_file(dock_t *d, const char *path)
{
    return path_req(d, DOCK_T_AUD_PLAY_FILE, NULL, 0, path, 0);
}

int dock_audio_pos(dock_t *d, uint32_t *pos_ms, bool *playing)
{
    int rc = dock_dev_request(d, DOCK_T_AUD_POS, NULL, 0, NULL, 0, 0, NULL, 0);
    dock_rd_t r = resp_reader(d);
    uint32_t pos = dock_rd_u32(&r);
    uint8_t pl = dock_rd_u8(&r);
    if (rc == DOCK_OK && r.underflow)
        rc = DOCK_E_INVALID;
    if (pos_ms)
        *pos_ms = rc == DOCK_OK ? pos : 0;
    if (playing)
        *playing = rc == DOCK_OK && pl;
    return rc;
}

int dock_audio_stop(dock_t *d)
{
    return dock_dev_request(d, DOCK_T_AUD_STOP, NULL, 0, NULL, 0, 0, NULL, 0);
}

int dock_audio_volume(dock_t *d, uint8_t percent)
{
    if (percent > 100)
        percent = 100;
    return dock_dev_request(d, DOCK_T_AUD_VOLUME, &percent, 1, NULL, 0, 0, NULL, 0);
}

/* ---- files ---- */

int dock_fs_drive(dock_t *d, uint8_t index, char *name, size_t cap)
{
    int rc = dock_dev_request(d, DOCK_T_FS_DRIVES, &index, 1, NULL, 0, 0, NULL, 0);
    if (rc == DOCK_OK) {
        dock_rd_t r = resp_reader(d);
        copy_str(name, cap, dock_rd_str(&r));
    }
    return rc;
}

int dock_fs_open(dock_t *d, const char *path, uint8_t mode, uint8_t *handle)
{
    int rc = path_req(d, DOCK_T_FS_OPEN, &mode, 1, path, 0);
    if (handle) {
        dock_rd_t r = resp_reader(d);
        *handle = rc == DOCK_OK ? dock_rd_u8(&r) : 0xFF;
    }
    return rc;
}

int dock_fs_read(dock_t *d, uint8_t handle, void *dst, size_t max, size_t *got)
{
    uint8_t b[4];
    int rc;
    if (max > 0xFFFFFFu)
        max = 0xFFFFFFu;
    b[0] = handle;
    dock_put_u24(b + 1, (uint32_t)max);
    rc = dock_dev_request(d, DOCK_T_FS_READ, b, sizeof b, NULL, 0, 0, dst, max);
    if (got)
        *got = rc == DOCK_OK ? d->resp_tail_len : 0;
    return rc;
}

int dock_fs_write(dock_t *d, uint8_t handle, const void *src, size_t n,
                  size_t *written)
{
    int rc = dock_dev_request(d, DOCK_T_FS_WRITE, &handle, 1, src, n, 0, NULL, 0);
    if (written) {
        dock_rd_t r = resp_reader(d);
        uint32_t v = dock_rd_u24(&r);
        *written = (rc == DOCK_OK && !r.underflow) ? (size_t)v : 0;
    }
    return rc;
}

int dock_fs_seek(dock_t *d, uint8_t handle, int32_t offset, uint8_t whence,
                 uint32_t *pos)
{
    uint8_t b[6];
    int rc;
    b[0] = handle;
    b[1] = whence;
    dock_put_u32(b + 2, (uint32_t)offset);
    rc = dock_dev_request(d, DOCK_T_FS_SEEK, b, sizeof b, NULL, 0, 0, NULL, 0);
    if (pos) {
        dock_rd_t r = resp_reader(d);
        *pos = rc == DOCK_OK ? dock_rd_u32(&r) : 0;
    }
    return rc;
}

int dock_fs_close(dock_t *d, uint8_t handle)
{
    return dock_dev_request(d, DOCK_T_FS_CLOSE, &handle, 1, NULL, 0, 0, NULL, 0);
}

int dock_fs_stat(dock_t *d, const char *path, uint32_t *size, bool *is_dir)
{
    int rc = path_req(d, DOCK_T_FS_STAT, NULL, 0, path, 0);
    dock_rd_t r = resp_reader(d);
    uint32_t sz = dock_rd_u32(&r);
    uint8_t dir = dock_rd_u8(&r);
    if (size)
        *size = rc == DOCK_OK ? sz : 0;
    if (is_dir)
        *is_dir = rc == DOCK_OK && dir;
    return rc;
}

int dock_fs_readdir(dock_t *d, const char *path, uint16_t index, char *name,
                    size_t cap, uint32_t *size, bool *is_dir)
{
    uint8_t pre[2];
    int rc;
    dock_put_u16(pre, index);
    rc = path_req(d, DOCK_T_FS_READDIR, pre, 2, path, 0);
    if (rc == DOCK_OK) {
        dock_rd_t r = resp_reader(d);
        uint32_t sz = dock_rd_u32(&r);
        uint8_t dir = dock_rd_u8(&r);
        copy_str(name, cap, dock_rd_str(&r));
        if (size)
            *size = sz;
        if (is_dir)
            *is_dir = dir != 0;
    }
    return rc;
}

int dock_fs_mkdir(dock_t *d, const char *path)
{
    return path_req(d, DOCK_T_FS_MKDIR, NULL, 0, path, 0);
}

int dock_fs_remove(dock_t *d, const char *path)
{
    return path_req(d, DOCK_T_FS_REMOVE, NULL, 0, path, 0);
}

/* ---- usb ---- */

int dock_usb_device(dock_t *d, uint8_t index, dock_usb_info_t *info)
{
    int rc = dock_dev_request(d, DOCK_T_USB_LIST, &index, 1, NULL, 0, 0, NULL, 0);
    if (rc == DOCK_OK && info) {
        dock_rd_t r = resp_reader(d);
        info->vid = dock_rd_u16(&r);
        info->pid = dock_rd_u16(&r);
        info->dev_class = dock_rd_u8(&r);
        copy_str(info->name, sizeof info->name, dock_rd_str(&r));
    }
    return rc;
}

/* ---- network ---- */

int dock_net_status(dock_t *d, bool *up, char *ssid, size_t ssid_cap, char *ip,
                    size_t ip_cap)
{
    int rc = dock_dev_request(d, DOCK_T_NET_STATUS, NULL, 0, NULL, 0, 0, NULL, 0);
    dock_rd_t r = resp_reader(d);
    uint8_t u = dock_rd_u8(&r);
    const char *s = dock_rd_str(&r);
    const char *a = dock_rd_str(&r);
    if (up)
        *up = rc == DOCK_OK && u;
    copy_str(ssid, ssid_cap, rc == DOCK_OK ? s : "");
    copy_str(ip, ip_cap, rc == DOCK_OK ? a : "");
    return rc;
}

int dock_http_begin(dock_t *d, uint8_t method, const char *url,
                    const char *headers, uint32_t body_len, uint8_t *handle)
{
    uint8_t buf[DOCK_DEV_SCRATCH - DOCK_HDR_LEN];
    dock_wr_t w;
    int rc;
    if (!url)
        return DOCK_E_INVALID;
    dock_wr_init(&w, buf, sizeof buf);
    dock_wr_u8(&w, method);
    dock_wr_u32(&w, body_len);
    dock_wr_str(&w, url);
    dock_wr_str(&w, headers ? headers : "");
    if (w.overflow)
        return DOCK_E_TOO_BIG;
    rc = dock_dev_request(d, DOCK_T_NET_HTTP_BEGIN, buf, w.len, NULL, 0, 0, NULL, 0);
    if (handle) {
        dock_rd_t r = resp_reader(d);
        *handle = rc == DOCK_OK ? dock_rd_u8(&r) : 0xFF;
    }
    return rc;
}

int dock_http_write(dock_t *d, uint8_t handle, const void *data, size_t n)
{
    return dock_dev_request(d, DOCK_T_NET_HTTP_BODY, &handle, 1, data, n, 0, NULL, 0);
}

int dock_http_response(dock_t *d, uint8_t handle, uint16_t *http_status,
                       uint32_t *content_len)
{
    int rc = dock_dev_request(d, DOCK_T_NET_HTTP_RESP, &handle, 1, NULL, 0, 0,
                              NULL, 0);
    dock_rd_t r = resp_reader(d);
    uint16_t st = dock_rd_u16(&r);
    uint32_t len = dock_rd_u32(&r);
    if (http_status)
        *http_status = rc == DOCK_OK ? st : 0;
    if (content_len)
        *content_len = rc == DOCK_OK ? len : 0xFFFFFFFFu;
    return rc;
}

int dock_http_read(dock_t *d, uint8_t handle, void *dst, size_t max, size_t *got)
{
    uint8_t b[4];
    int rc;
    if (max > 0xFFFFFFu)
        max = 0xFFFFFFu;
    b[0] = handle;
    dock_put_u24(b + 1, (uint32_t)max);
    rc = dock_dev_request(d, DOCK_T_NET_HTTP_READ, b, sizeof b, NULL, 0, 0, dst, max);
    if (got)
        *got = rc == DOCK_OK ? d->resp_tail_len : 0;
    return rc;
}

int dock_http_end(dock_t *d, uint8_t handle)
{
    return dock_dev_request(d, DOCK_T_NET_HTTP_END, &handle, 1, NULL, 0, 0, NULL, 0);
}

int dock_http_get(dock_t *d, const char *url, void *dst, size_t max,
                  size_t *got, uint16_t *http_status)
{
    uint8_t h;
    size_t total = 0;
    int rc = dock_http_begin(d, DOCK_HTTP_GET, url, NULL, 0, &h);
    if (rc != DOCK_OK)
        return rc;
    rc = dock_http_response(d, h, http_status, NULL);
    while (rc == DOCK_OK && total < max) {
        size_t n = 0;
        rc = dock_http_read(d, h, (uint8_t *)dst + total, max - total, &n);
        if (rc != DOCK_OK || n == 0)
            break;
        total += n;
    }
    if (rc == DOCK_E_END)
        rc = DOCK_OK;
    dock_http_end(d, h);
    if (got)
        *got = total;
    return rc;
}

int dock_http_put(dock_t *d, const char *url, const void *body, size_t n,
                  uint16_t *http_status)
{
    uint8_t h;
    int rc = dock_http_begin(d, DOCK_HTTP_PUT, url, NULL, (uint32_t)n, &h);
    if (rc != DOCK_OK)
        return rc;
    if (n)
        rc = dock_http_write(d, h, body, n);
    if (rc == DOCK_OK)
        rc = dock_http_response(d, h, http_status, NULL);
    dock_http_end(d, h);
    return rc;
}
