/* dock_dev.c - portable device engine. See dock_dev.h. */
#include "dock_dev.h"

#define HELLO_RETRY_MS 1000u
#define RX_CHUNKS_PER_POLL 8u

/* ------------------------------------------------------------------ */
/* transmit                                                            */

static void tx_pump(dock_dev_t *d)
{
    uint8_t guard = 8;
    while (d->tx_busy && guard--) {
        size_t left, n, max;
        bool last;
        if (!dock_dhal_tx_idle(d->hal))
            return;
        while (d->tx_piece < 2 && d->tx_off >= d->tx_len[d->tx_piece]) {
            d->tx_piece++;
            d->tx_off = 0;
        }
        if (d->tx_piece >= 2) {
            d->tx_busy = false;
            d->frames_sent++;
            return;
        }
        left = d->tx_len[d->tx_piece] - d->tx_off;
        max = dock_dhal_tx_max(d->hal);
        n = left < max ? left : max;
        last = (n == left) && (d->tx_piece == 1 || d->tx_len[1] == 0);
        if (!dock_dhal_tx(d->hal, d->tx_ptr[d->tx_piece] + d->tx_off, n, last)) {
            if (!dock_dhal_link_up(d->hal))
                d->tx_busy = false;
            return;
        }
        d->tx_off += n;
        d->bytes_sent += (uint32_t)n;
    }
}

/* Start a frame whose fixed fields are already in scratch[DOCK_HDR_LEN..]. */
static void tx_start(dock_dev_t *d, uint8_t type, uint8_t seq, size_t fixed_len,
                     const void *tail, size_t tail_len)
{
    dock_hdr_build(d->scratch, type, seq, (uint32_t)(fixed_len + tail_len));
    d->tx_ptr[0] = d->scratch;
    d->tx_len[0] = DOCK_HDR_LEN + fixed_len;
    d->tx_ptr[1] = (const uint8_t *)tail;
    d->tx_len[1] = tail ? tail_len : 0;
    d->tx_piece = 0;
    d->tx_off = 0;
    d->tx_busy = true;
    tx_pump(d);
}

static void fixed_writer(dock_dev_t *d, dock_wr_t *w)
{
    dock_wr_init(w, d->scratch + DOCK_HDR_LEN, DOCK_DEV_SCRATCH - DOCK_HDR_LEN);
}

static void send_hello(dock_dev_t *d)
{
    dock_wr_t w;
    fixed_writer(d, &w);
    dock_wr_u8(&w, DOCK_PROTO_VERSION);
    dock_wr_u8(&w, d->hello_flags);
    dock_wr_u32(&w, d->calc_id);
    dock_wr_u16(&w, d->driver_ver);
    dock_wr_bytes(&w, d->join_code, DOCK_JOIN_LEN);
    tx_start(d, DOCK_T_HELLO, d->seq++, w.len, NULL, 0);
    d->hellos_sent++;
    d->last_hello_ms = dock_dhal_ms(d->hal);
}

/* ------------------------------------------------------------------ */
/* receive                                                             */

static void on_sink(void *user, const dock_frame_t *f, dock_sink_t *out)
{
    dock_dev_t *d = (dock_dev_t *)user;
    if (f->type == DOCK_T_RESP && d->req_active && !d->req_done &&
        f->seq == d->req_seq && d->resp_tail) {
        out->fixed_len = DOCK_RESP_HDR_LEN + d->resp_fixed;
        out->tail = d->resp_tail;
        out->tail_cap = d->resp_tail_cap;
    }
}

static void key_push(dock_dev_t *d, const dock_key_t *k)
{
    uint8_t next = (uint8_t)((d->key_head + 1u) & (DOCK_DEV_KEYQ - 1u));
    if (next == d->key_tail)
        return; /* full: drop */
    d->keyq[d->key_head] = *k;
    d->key_head = next;
}

static void hid_push(dock_dev_t *d, const dock_hidkey_t *k)
{
    uint8_t next = (uint8_t)((d->hid_head + 1u) & (DOCK_DEV_KEYQ - 1u));
    if (next == d->hid_tail)
        return;
    d->hidq[d->hid_head] = *k;
    d->hid_head = next;
}

static void screen_restart(dock_dev_t *d)
{
    dock_scr_tx_refresh(&d->scr);
    d->scr.scan_y = 0;
    d->pass_done = false;
    d->pass_start_ms = dock_dhal_ms(d->hal);
    d->last_full_ms = d->pass_start_ms;
}

static void on_frame(void *user, const dock_frame_t *f, const uint8_t *buf,
                     size_t buf_len, size_t tail_len, bool truncated)
{
    dock_dev_t *d = (dock_dev_t *)user;
    dock_rd_t r;
    dock_rd_init(&r, buf, buf_len);

    switch (f->type) {
    case DOCK_T_WELCOME: {
        uint8_t status;
        (void)dock_rd_u8(&r); /* host protocol version */
        status = dock_rd_u8(&r);
        d->dock_id = dock_rd_u32(&r);
        d->host_caps = dock_rd_u16(&r);
        if (r.underflow)
            break;
        d->last_welcome_status = status;
        if (status == DOCK_OK) {
            d->state = DOCK_DEV_RUNNING;
            screen_restart(d);
        } else {
            d->state = DOCK_DEV_NEED_JOIN;
        }
        break;
    }
    case DOCK_T_PING:
        /* Answered by dock_dev_poll() as soon as the pipe is free. */
        d->pong_seq = f->seq;
        d->pong_pending = true;
        break;
    case DOCK_T_SCR_REFRESH:
        screen_restart(d);
        break;
    case DOCK_T_KEY: {
        dock_key_t k;
        k.tikey = dock_rd_u16(&r);
        k.scancode = dock_rd_u8(&r);
        k.flags = dock_rd_u8(&r);
        if (!r.underflow)
            key_push(d, &k);
        break;
    }
    case DOCK_T_HIDKEY: {
        dock_hidkey_t k;
        k.usage = dock_rd_u8(&r);
        k.mods = dock_rd_u8(&r);
        k.down = dock_rd_u8(&r);
        k.ascii = dock_rd_u8(&r);
        if (!r.underflow)
            hid_push(d, &k);
        break;
    }
    case DOCK_T_RESP: {
        uint8_t req_seq, req_type, status;
        size_t n;
        if (!d->req_active || d->req_done || f->seq != d->req_seq)
            break;
        req_seq = dock_rd_u8(&r);
        req_type = dock_rd_u8(&r);
        status = dock_rd_u8(&r);
        if (r.underflow || req_seq != d->req_seq || req_type != d->req_type)
            break;
        n = dock_rd_left(&r);
        if (n > sizeof d->resp) {
            n = sizeof d->resp;
            truncated = true;
        }
        memcpy(d->resp, dock_rd_ptr(&r), n);
        d->resp_len = n;
        d->resp_tail_len = tail_len;
        d->req_status = truncated && status == DOCK_OK ? (uint8_t)DOCK_E_TOO_BIG : status;
        d->req_done = true;
        break;
    }
    default:
        break; /* unknown types are ignored for forward compatibility */
    }
}

/* ------------------------------------------------------------------ */
/* screen                                                              */

static void screen_step(dock_dev_t *d)
{
    dock_fb_t fb;
    dock_wr_t w;
    uint32_t now;
    uint16_t pal_n, y0, cnt;

    if (d->tx_busy || !dock_dhal_fb(d->hal, &fb))
        return;
    dock_scr_tx_set_mode(&d->scr, fb.width, fb.height, fb.stride, fb.fmt, fb.flags);
    pal_n = dock_palette_size(d->scr.fmt);
    if (pal_n && fb.palette_le)
        dock_scr_tx_check_palette(&d->scr, fb.palette_le, pal_n);

    if (d->scr.mode_dirty) {
        fixed_writer(d, &w);
        dock_wr_u16(&w, d->scr.width);
        dock_wr_u16(&w, d->scr.height);
        dock_wr_u8(&w, d->scr.fmt);
        dock_wr_u8(&w, d->scr.flags);
        d->scr.mode_dirty = false;
        tx_start(d, DOCK_T_SCR_MODE, d->seq++, w.len, NULL, 0);
        return;
    }
    if (d->scr.pal_dirty) {
        d->scr.pal_dirty = false;
        if (pal_n && fb.palette_le) {
            fixed_writer(d, &w);
            dock_wr_u8(&w, 0);
            dock_wr_u16(&w, pal_n);
            tx_start(d, DOCK_T_SCR_PALETTE, d->seq++, w.len, fb.palette_le,
                     (size_t)pal_n * 2u);
            return;
        }
    }

    /* Send changed rows as soon as we know about them. */
    if (dock_scr_tx_next_band(&d->scr, d->band_rows_max, &y0, &cnt)) {
        bool end = d->pass_done && !dock_scr_tx_any_dirty(&d->scr);
        uint16_t rb = dock_row_bytes(d->scr.width, d->scr.fmt);
        fixed_writer(d, &w);
        dock_wr_u16(&w, y0);
        dock_wr_u16(&w, cnt);
        dock_wr_u16(&w, rb);
        dock_wr_u8(&w, end ? 1u : 0u);
        /* Rows are sent with stride == row bytes; when the framebuffer has
         * padding we must send row by row. */
        if (fb.stride != rb && cnt > 1) {
            /* put the rest back and send one row */
            uint16_t y;
            for (y = (uint16_t)(y0 + 1u); y < (uint16_t)(y0 + cnt); y++)
                d->scr.dirty[y >> 3] |= (uint8_t)(1u << (y & 7u));
            cnt = 1;
            dock_put_u16(d->scratch + DOCK_HDR_LEN + 2, 1);
            d->scratch[DOCK_HDR_LEN + 6] = 0;
        }
        tx_start(d, DOCK_T_SCR_ROWS, d->seq++, w.len,
                 fb.pixels + (uint32_t)y0 * fb.stride, (size_t)cnt * rb);
        return;
    }

    now = dock_dhal_ms(d->hal);
    if (!d->pass_done) {
        if (dock_scr_tx_scan(&d->scr, fb.pixels, d->scan_rows_per_poll))
            d->pass_done = true;
        return;
    }
    if ((uint32_t)(now - d->pass_start_ms) >= d->frame_interval_ms) {
        if (d->full_refresh_ms &&
            (uint32_t)(now - d->last_full_ms) >= d->full_refresh_ms) {
            d->last_full_ms = now;
            d->scr.force_all = true;
        }
        d->pass_done = false;
        d->pass_start_ms = now;
    }
}

/* ------------------------------------------------------------------ */
/* public                                                              */

void dock_dev_init(dock_dev_t *d, void *hal, uint32_t calc_id)
{
    memset(d, 0, sizeof *d);
    d->hal = hal;
    d->calc_id = calc_id;
    d->driver_ver = 1;
    d->scan_rows_per_poll = 24;
    d->band_rows_max = 240;
    d->frame_interval_ms = 33;
    d->full_refresh_ms = 10000;
    d->timeout_ms = 3000;
    d->screen_enabled = true;
    dock_rx_init(&d->rx, d->rxbuf, sizeof d->rxbuf, on_sink, on_frame, d);
    dock_scr_tx_init(&d->scr, dock_dhal_row_hash);
}

void dock_dev_set_join_code(dock_dev_t *d, const char *code)
{
    memset(d->join_code, 0, sizeof d->join_code);
    if (code) {
        size_t n = strlen(code);
        memcpy(d->join_code, code, n < DOCK_JOIN_LEN ? n : DOCK_JOIN_LEN);
    }
    if (d->state != DOCK_DEV_OFFLINE) {
        d->state = DOCK_DEV_HELLO;
        d->last_hello_ms = dock_dhal_ms(d->hal) - HELLO_RETRY_MS; /* now */
    }
}

static void link_down(dock_dev_t *d)
{
    d->state = DOCK_DEV_OFFLINE;
    d->tx_busy = false;
    d->pong_pending = false;
    dock_rx_reset(&d->rx);
    if (d->req_active && !d->req_done) {
        d->req_status = DOCK_E_DISCONNECTED;
        d->req_done = true;
    }
}

void dock_dev_poll(dock_dev_t *d)
{
    uint8_t i;
    bool up;

    dock_dhal_service(d->hal);
    up = dock_dhal_link_up(d->hal);
    if (up != d->link_was_up) {
        d->link_was_up = up;
        if (up) {
            d->state = DOCK_DEV_HELLO;
            d->tx_busy = false;
            dock_rx_reset(&d->rx);
            d->last_hello_ms = dock_dhal_ms(d->hal) - HELLO_RETRY_MS;
        } else {
            link_down(d);
        }
    }
    if (!up)
        return;

    for (i = 0; i < RX_CHUNKS_PER_POLL; i++) {
        size_t n = dock_dhal_rx(d->hal, d->rxchunk, sizeof d->rxchunk);
        if (!n)
            break;
        dock_rx_feed(&d->rx, d->rxchunk, n);
    }

    tx_pump(d);
    if (d->tx_busy)
        return;

    if (d->pong_pending) {
        d->pong_pending = false;
        tx_start(d, DOCK_T_PONG, d->pong_seq, 0, NULL, 0);
        return;
    }
    if (d->state == DOCK_DEV_HELLO &&
        (uint32_t)(dock_dhal_ms(d->hal) - d->last_hello_ms) >= HELLO_RETRY_MS) {
        send_hello(d);
        return;
    }
    if (d->state == DOCK_DEV_RUNNING && d->screen_enabled)
        screen_step(d);
}

void dock_dev_bye(dock_dev_t *d)
{
    uint32_t t0 = dock_dhal_ms(d->hal);
    if (!dock_dhal_link_up(d->hal))
        return;
    while (d->tx_busy && (uint32_t)(dock_dhal_ms(d->hal) - t0) < 100u) {
        dock_dhal_service(d->hal);
        tx_pump(d);
    }
    tx_start(d, DOCK_T_BYE, d->seq++, 0, NULL, 0);
    while (d->tx_busy && (uint32_t)(dock_dhal_ms(d->hal) - t0) < 200u) {
        dock_dhal_service(d->hal);
        tx_pump(d);
    }
    d->tx_busy = false;
}

bool dock_dev_key_pop(dock_dev_t *d, dock_key_t *k)
{
    if (d->key_tail == d->key_head)
        return false;
    *k = d->keyq[d->key_tail];
    d->key_tail = (uint8_t)((d->key_tail + 1u) & (DOCK_DEV_KEYQ - 1u));
    return true;
}

bool dock_dev_hid_pop(dock_dev_t *d, dock_hidkey_t *k)
{
    if (d->hid_tail == d->hid_head)
        return false;
    *k = d->hidq[d->hid_tail];
    d->hid_tail = (uint8_t)((d->hid_tail + 1u) & (DOCK_DEV_KEYQ - 1u));
    return true;
}

void dock_dev_screen_refresh(dock_dev_t *d)
{
    screen_restart(d);
}

void dock_dev_screen_push(dock_dev_t *d)
{
    uint32_t t0 = dock_dhal_ms(d->hal);
    if (d->state != DOCK_DEV_RUNNING)
        return;
    d->scr.scan_y = 0;
    d->pass_done = false;
    d->pass_start_ms = t0;
    while ((uint32_t)(dock_dhal_ms(d->hal) - t0) < 1000u &&
           d->state == DOCK_DEV_RUNNING) {
        dock_dev_poll(d);
        if (d->pass_done && !d->tx_busy && !dock_scr_tx_any_dirty(&d->scr) &&
            !d->scr.mode_dirty && !d->scr.pal_dirty)
            break;
    }
}

int dock_dev_request(dock_dev_t *d, uint8_t type, const void *fixed,
                     size_t fixed_len, const void *tail, size_t tail_len,
                     uint8_t resp_fixed, void *resp_tail, size_t resp_tail_cap)
{
    uint32_t t0 = dock_dhal_ms(d->hal);
    dock_wr_t w;

    if (d->state == DOCK_DEV_NEED_JOIN)
        return DOCK_E_JOIN_REQUIRED;
    if (d->state != DOCK_DEV_RUNNING)
        return DOCK_E_DISCONNECTED;
    if (d->req_active)
        return DOCK_E_BUSY;
    if (fixed_len > DOCK_DEV_SCRATCH - DOCK_HDR_LEN ||
        tail_len > DOCK_MAX_PAYLOAD - fixed_len)
        return DOCK_E_TOO_BIG;

    /* Wait for the pipe (a screen band may be in flight). */
    while (d->tx_busy) {
        if ((uint32_t)(dock_dhal_ms(d->hal) - t0) >= d->timeout_ms)
            return DOCK_E_TIMEOUT;
        dock_dhal_service(d->hal);
        tx_pump(d);
        if (!dock_dhal_link_up(d->hal)) {
            link_down(d);
            return DOCK_E_DISCONNECTED;
        }
    }

    d->req_active = true;
    d->req_done = false;
    d->req_seq = d->seq++;
    d->req_type = type;
    d->req_status = DOCK_E_TIMEOUT;
    d->resp_len = 0;
    d->resp_tail = (uint8_t *)resp_tail;
    d->resp_tail_cap = resp_tail ? resp_tail_cap : 0;
    d->resp_tail_len = 0;
    d->resp_fixed = resp_fixed;

    fixed_writer(d, &w);
    dock_wr_bytes(&w, fixed, fixed_len);
    tx_start(d, type, d->req_seq, w.len, tail, tail_len);

    t0 = dock_dhal_ms(d->hal);
    while (!d->req_done) {
        if ((uint32_t)(dock_dhal_ms(d->hal) - t0) >= d->timeout_ms)
            break;
        dock_dev_poll(d);
    }
    /* Never return while the request is still being sent from the caller's
     * buffer. */
    t0 = dock_dhal_ms(d->hal);
    while (d->tx_busy && d->tx_ptr[1] == (const uint8_t *)tail && tail &&
           (uint32_t)(dock_dhal_ms(d->hal) - t0) < d->timeout_ms) {
        dock_dhal_service(d->hal);
        tx_pump(d);
    }
    if (d->tx_busy && d->tx_ptr[1] == (const uint8_t *)tail && tail)
        d->tx_busy = false; /* give up on it; the link is unusable anyway */
    d->req_active = false;
    d->resp_tail = NULL;
    return d->req_done ? d->req_status : DOCK_E_TIMEOUT;
}
