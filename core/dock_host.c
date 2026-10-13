/* dock_host.c - portable host engine. See dock_host.h. */
#include "dock_host.h"

/* ---- sending ---- */

static bool send_frame(dock_host_t *h, uint8_t type, uint8_t seq,
                       const uint8_t *payload, size_t len)
{
    uint8_t hdr[DOCK_HDR_LEN];
    dock_hdr_build(hdr, type, seq, (uint32_t)len);
    if (!dock_hhal_tx(h->hal, hdr, sizeof hdr))
        return false;
    return len == 0 || dock_hhal_tx(h->hal, payload, len);
}

/* Response layout in h->resp: [hdr 8][req_seq][req_type][status][fields...] */
#define RESP_BODY (DOCK_HDR_LEN + DOCK_RESP_HDR_LEN)

static void resp_writer(dock_host_t *h, dock_wr_t *w)
{
    dock_wr_init(w, h->resp + RESP_BODY, h->resp_cap - RESP_BODY);
}

static void send_resp(dock_host_t *h, const dock_frame_t *req, int status,
                      size_t body_len)
{
    uint8_t *p = h->resp;
    size_t len = DOCK_RESP_HDR_LEN + body_len;
    if (status != DOCK_OK && status != DOCK_E_END)
        len = DOCK_RESP_HDR_LEN; /* no fields on error */
    p[DOCK_HDR_LEN + 0] = req->seq;
    p[DOCK_HDR_LEN + 1] = req->type;
    p[DOCK_HDR_LEN + 2] = (uint8_t)status;
    dock_hdr_build(p, DOCK_T_RESP, req->seq, (uint32_t)len);
    dock_hhal_tx(h->hal, p, DOCK_HDR_LEN + len);
}

/* Largest bulk read that fits the response buffer. */
static size_t resp_room(const dock_host_t *h)
{
    return h->resp_cap - RESP_BODY;
}

/* ---- request handlers ---- */

static void handle_hello(dock_host_t *h, const dock_frame_t *f, dock_rd_t *r)
{
    uint8_t out[8];
    char code[6];
    int st;
    (void)dock_rd_u8(r); /* device protocol version */
    h->hello_flags = dock_rd_u8(r);
    h->calc_id = dock_rd_u32(r);
    h->driver_ver = dock_rd_u16(r);
    if (dock_rd_left(r) < 6) {
        h->bad_frames++;
        return;
    }
    memcpy(code, dock_rd_ptr(r), 6);
    st = dock_hhal_join_check(h->hal, h->calc_id, code);
    out[0] = DOCK_PROTO_VERSION;
    out[1] = (uint8_t)st;
    dock_put_u32(out + 2, dock_hhal_dock_id(h->hal));
    dock_put_u16(out + 6, dock_hhal_caps(h->hal));
    send_frame(h, DOCK_T_WELCOME, f->seq, out, sizeof out);
    h->accepted = st == DOCK_OK;
    dock_hhal_event(h->hal, h->accepted ? DOCK_HEV_CONNECTED : DOCK_HEV_JOIN_NEEDED);
}

static void handle_request(dock_host_t *h, const dock_frame_t *f, dock_rd_t *r)
{
    dock_wr_t w;
    int st = DOCK_E_UNSUPPORTED;
    resp_writer(h, &w);
    h->requests++;

    if (!h->accepted) {
        send_resp(h, f, DOCK_E_JOIN_REQUIRED, 0);
        return;
    }

    switch (f->type) {
    /* ---- audio ---- */
    case DOCK_T_AUD_OPEN: {
        uint32_t rate = dock_rd_u32(r);
        uint8_t fmt = dock_rd_u8(r), ch = dock_rd_u8(r);
        st = r->underflow ? DOCK_E_INVALID : dock_hhal_audio_open(h->hal, rate, fmt, ch);
        break;
    }
    case DOCK_T_AUD_WRITE: {
        size_t acc = 0;
        st = dock_hhal_audio_write(h->hal, dock_rd_ptr(r), dock_rd_left(r), &acc);
        dock_wr_u24(&w, (uint32_t)acc);
        break;
    }
    case DOCK_T_AUD_PLAY_FILE: {
        const char *path = dock_rd_str(r);
        st = r->underflow ? DOCK_E_INVALID : dock_hhal_audio_play_file(h->hal, path);
        break;
    }
    case DOCK_T_AUD_POS: {
        uint32_t pos = 0;
        bool playing = false;
        st = dock_hhal_audio_pos(h->hal, &pos, &playing);
        dock_wr_u32(&w, pos);
        dock_wr_u8(&w, playing ? 1u : 0u);
        break;
    }
    case DOCK_T_AUD_STOP:
        st = dock_hhal_audio_stop(h->hal);
        break;
    case DOCK_T_AUD_VOLUME: {
        uint8_t v = dock_rd_u8(r);
        st = r->underflow ? DOCK_E_INVALID : dock_hhal_audio_volume(h->hal, v);
        break;
    }

    /* ---- storage ---- */
    case DOCK_T_FS_DRIVES: {
        char name[64];
        uint8_t idx = dock_rd_u8(r);
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_fs_drive(h->hal, idx, name, sizeof name);
        if (st == DOCK_OK)
            dock_wr_str(&w, name);
        break;
    }
    case DOCK_T_FS_OPEN: {
        uint8_t mode = dock_rd_u8(r), handle = 0xFF;
        const char *path = dock_rd_str(r);
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_fs_open(h->hal, path, mode, &handle);
        dock_wr_u8(&w, handle);
        break;
    }
    case DOCK_T_FS_READ: {
        uint8_t handle = dock_rd_u8(r);
        size_t max = (size_t)dock_rd_u24(r), got = 0;
        if (max > resp_room(h))
            max = resp_room(h);
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_fs_read(h->hal, handle, w.p, max, &got);
        w.len = got;
        break;
    }
    case DOCK_T_FS_WRITE: {
        uint8_t handle = dock_rd_u8(r);
        size_t written = 0;
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_fs_write(h->hal, handle, dock_rd_ptr(r),
                                               dock_rd_left(r), &written);
        dock_wr_u24(&w, (uint32_t)written);
        break;
    }
    case DOCK_T_FS_SEEK: {
        uint8_t handle = dock_rd_u8(r), whence = dock_rd_u8(r);
        int32_t off = (int32_t)dock_rd_u32(r);
        uint32_t pos = 0;
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_fs_seek(h->hal, handle, off, whence, &pos);
        dock_wr_u32(&w, pos);
        break;
    }
    case DOCK_T_FS_CLOSE: {
        uint8_t handle = dock_rd_u8(r);
        st = r->underflow ? DOCK_E_INVALID : dock_hhal_fs_close(h->hal, handle);
        break;
    }
    case DOCK_T_FS_STAT: {
        const char *path = dock_rd_str(r);
        uint32_t size = 0;
        bool dir = false;
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_fs_stat(h->hal, path, &size, &dir);
        dock_wr_u32(&w, size);
        dock_wr_u8(&w, dir ? 1u : 0u);
        break;
    }
    case DOCK_T_FS_READDIR: {
        uint16_t idx = dock_rd_u16(r);
        const char *path = dock_rd_str(r);
        char name[256];
        uint32_t size = 0;
        bool dir = false;
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_fs_readdir(h->hal, path, idx, name,
                                                 sizeof name, &size, &dir);
        if (st == DOCK_OK) {
            dock_wr_u32(&w, size);
            dock_wr_u8(&w, dir ? 1u : 0u);
            dock_wr_str(&w, name);
        }
        break;
    }
    case DOCK_T_FS_MKDIR:
    case DOCK_T_FS_REMOVE: {
        const char *path = dock_rd_str(r);
        if (r->underflow)
            st = DOCK_E_INVALID;
        else if (f->type == DOCK_T_FS_MKDIR)
            st = dock_hhal_fs_mkdir(h->hal, path);
        else
            st = dock_hhal_fs_remove(h->hal, path);
        break;
    }

    /* ---- usb ---- */
    case DOCK_T_USB_LIST: {
        uint8_t idx = dock_rd_u8(r), cls = 0;
        uint16_t vid = 0, pid = 0;
        char name[40];
        name[0] = '\0';
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_usb_device(h->hal, idx, &vid, &pid, &cls,
                                                 name, sizeof name);
        if (st == DOCK_OK) {
            dock_wr_u16(&w, vid);
            dock_wr_u16(&w, pid);
            dock_wr_u8(&w, cls);
            dock_wr_str(&w, name);
        }
        break;
    }

    /* ---- network ---- */
    case DOCK_T_NET_STATUS: {
        bool up = false;
        char ssid[64], ip[48];
        ssid[0] = ip[0] = '\0';
        st = dock_hhal_net_status(h->hal, &up, ssid, sizeof ssid, ip, sizeof ip);
        dock_wr_u8(&w, up ? 1u : 0u);
        dock_wr_str(&w, ssid);
        dock_wr_str(&w, ip);
        break;
    }
    case DOCK_T_NET_HTTP_BEGIN: {
        uint8_t method = dock_rd_u8(r), handle = 0xFF;
        uint32_t body_len = dock_rd_u32(r);
        const char *url = dock_rd_str(r);
        const char *hdrs = dock_rd_str(r);
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_http_begin(h->hal, method, url, hdrs,
                                                 body_len, &handle);
        dock_wr_u8(&w, handle);
        break;
    }
    case DOCK_T_NET_HTTP_BODY: {
        uint8_t handle = dock_rd_u8(r);
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_http_body(h->hal, handle, dock_rd_ptr(r),
                                                dock_rd_left(r));
        break;
    }
    case DOCK_T_NET_HTTP_RESP: {
        uint8_t handle = dock_rd_u8(r);
        uint16_t hs = 0;
        uint32_t clen = 0xFFFFFFFFu;
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_http_response(h->hal, handle, &hs, &clen);
        dock_wr_u16(&w, hs);
        dock_wr_u32(&w, clen);
        break;
    }
    case DOCK_T_NET_HTTP_READ: {
        uint8_t handle = dock_rd_u8(r);
        size_t max = (size_t)dock_rd_u24(r), got = 0;
        if (max > resp_room(h))
            max = resp_room(h);
        st = r->underflow ? DOCK_E_INVALID
                          : dock_hhal_http_read(h->hal, handle, w.p, max, &got);
        w.len = got;
        break;
    }
    case DOCK_T_NET_HTTP_END: {
        uint8_t handle = dock_rd_u8(r);
        st = r->underflow ? DOCK_E_INVALID : dock_hhal_http_end(h->hal, handle);
        break;
    }
    default:
        st = DOCK_E_UNSUPPORTED;
        break;
    }
    if (w.overflow)
        st = DOCK_E_TOO_BIG;
    send_resp(h, f, st, w.len);
}

/* ---- frame dispatch ---- */

static void on_frame(void *user, const dock_frame_t *f, const uint8_t *buf,
                     size_t buf_len, size_t tail_len, bool truncated)
{
    dock_host_t *h = (dock_host_t *)user;
    dock_rd_t r;
    (void)tail_len;
    h->frames_rx++;
    h->bytes_rx += DOCK_HDR_LEN + f->len;
    if (truncated) {
        h->bad_frames++;
        if (f->type >= DOCK_T_AUD_OPEN)
            send_resp(h, f, DOCK_E_TOO_BIG, 0);
        return;
    }
    dock_rd_init(&r, buf, buf_len);

    switch (f->type) {
    case DOCK_T_HELLO:
        handle_hello(h, f, &r);
        break;
    case DOCK_T_PING:
        send_frame(h, DOCK_T_PONG, f->seq, NULL, 0);
        break;
    case DOCK_T_PONG:
        break;
    case DOCK_T_BYE:
        h->accepted = false;
        dock_hhal_event(h->hal, DOCK_HEV_BYE);
        break;
    case DOCK_T_LOG: {
        char line[128];
        size_t n = buf_len < sizeof line - 1 ? buf_len : sizeof line - 1;
        memcpy(line, buf, n);
        line[n] = '\0';
        dock_hhal_log(h->hal, line);
        break;
    }
    case DOCK_T_SCR_MODE:
        if (dock_scr_rx_mode(&h->scr, buf, buf_len) == DOCK_OK)
            dock_hhal_display_mode(h->hal, h->scr.width, h->scr.height);
        else
            h->bad_frames++;
        break;
    case DOCK_T_SCR_PALETTE:
        if (dock_scr_rx_palette(&h->scr, buf, buf_len) != DOCK_OK)
            h->bad_frames++;
        break;
    case DOCK_T_SCR_ROWS: {
        uint16_t y0 = 0, cnt = 0;
        bool end = false;
        if (dock_scr_rx_rows(&h->scr, buf, buf_len, &y0, &cnt, &end) == DOCK_OK) {
            dock_hhal_display_damage(h->hal, y0, cnt);
            if (end)
                dock_hhal_event(h->hal, DOCK_HEV_FRAME);
        } else {
            h->bad_frames++;
        }
        break;
    }
    default:
        if (f->type >= DOCK_T_AUD_OPEN)
            handle_request(h, f, &r);
        break;
    }
}

/* ---- public ---- */

void dock_host_init(dock_host_t *h, void *hal, uint8_t *rxbuf, size_t rxcap,
                    uint8_t *respbuf, size_t respcap, uint32_t *fb,
                    uint16_t fb_w, uint16_t fb_h, uint32_t fb_stride_px)
{
    memset(h, 0, sizeof *h);
    h->hal = hal;
    h->resp = respbuf;
    h->resp_cap = respcap;
    dock_rx_init(&h->rx, rxbuf, rxcap, NULL, on_frame, h);
    dock_scr_rx_init(&h->scr, fb, fb_w, fb_h, fb_stride_px);
}

void dock_host_reset(dock_host_t *h)
{
    dock_rx_reset(&h->rx);
    h->accepted = false;
}

void dock_host_feed(dock_host_t *h, const uint8_t *data, size_t n)
{
    dock_rx_feed(&h->rx, data, n);
}

bool dock_host_send_key(dock_host_t *h, uint16_t tikey, uint8_t scancode,
                        uint8_t flags)
{
    uint8_t p[4];
    dock_put_u16(p, tikey);
    p[2] = scancode;
    p[3] = flags;
    return send_frame(h, DOCK_T_KEY, h->seq++, p, sizeof p);
}

bool dock_host_send_hidkey(dock_host_t *h, uint8_t usage, uint8_t mods,
                           bool down, uint8_t ascii)
{
    uint8_t p[4];
    p[0] = usage;
    p[1] = mods;
    p[2] = down ? 1u : 0u;
    p[3] = ascii;
    return send_frame(h, DOCK_T_HIDKEY, h->seq++, p, sizeof p);
}

bool dock_host_request_refresh(dock_host_t *h)
{
    return send_frame(h, DOCK_T_SCR_REFRESH, h->seq++, NULL, 0);
}

bool dock_host_ping(dock_host_t *h)
{
    return send_frame(h, DOCK_T_PING, h->seq++, NULL, 0);
}
