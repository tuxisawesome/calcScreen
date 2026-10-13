/* dock_proto.c - framing, payload helpers and the stream parser. */
#include "dock_proto.h"

uint8_t dock_crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    while (n--) {
        uint8_t i;
        crc ^= *p++;
        for (i = 0; i < 8; i++)
            crc = (uint8_t)((crc & 0x80u) ? (uint8_t)(crc << 1) ^ 0x07u : (uint8_t)(crc << 1));
    }
    return crc;
}

void dock_hdr_build(uint8_t hdr[DOCK_HDR_LEN], uint8_t type, uint8_t seq,
                    uint32_t len)
{
    hdr[0] = DOCK_MAGIC0;
    hdr[1] = DOCK_MAGIC1;
    hdr[2] = type;
    hdr[3] = seq;
    dock_put_u24(hdr + 4, len);
    hdr[7] = dock_crc8(hdr, 7);
}

/* ---- writer ---- */

void dock_wr_init(dock_wr_t *w, uint8_t *buf, size_t cap)
{
    w->p = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

void dock_wr_bytes(dock_wr_t *w, const void *src, size_t n)
{
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    if (n)
        memcpy(w->p + w->len, src, n);
    w->len += n;
}

void dock_wr_u8(dock_wr_t *w, uint8_t v)
{
    dock_wr_bytes(w, &v, 1);
}

void dock_wr_u16(dock_wr_t *w, uint16_t v)
{
    uint8_t b[2];
    dock_put_u16(b, v);
    dock_wr_bytes(w, b, 2);
}

void dock_wr_u24(dock_wr_t *w, uint32_t v)
{
    uint8_t b[3];
    dock_put_u24(b, v);
    dock_wr_bytes(w, b, 3);
}

void dock_wr_u32(dock_wr_t *w, uint32_t v)
{
    uint8_t b[4];
    dock_put_u32(b, v);
    dock_wr_bytes(w, b, 4);
}

void dock_wr_str(dock_wr_t *w, const char *s)
{
    dock_wr_bytes(w, s, strlen(s) + 1);
}

/* ---- reader ---- */

void dock_rd_init(dock_rd_t *r, const uint8_t *buf, size_t len)
{
    r->p = buf;
    r->len = len;
    r->pos = 0;
    r->underflow = false;
}

static const uint8_t *rd_take(dock_rd_t *r, size_t n)
{
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    const uint8_t *q;
    if (r->underflow || n > r->len - r->pos) {
        r->underflow = true;
        return zeros;
    }
    q = r->p + r->pos;
    r->pos += n;
    return q;
}

uint8_t dock_rd_u8(dock_rd_t *r)
{
    return *rd_take(r, 1);
}

uint16_t dock_rd_u16(dock_rd_t *r)
{
    return dock_get_u16(rd_take(r, 2));
}

uint32_t dock_rd_u24(dock_rd_t *r)
{
    return dock_get_u24(rd_take(r, 3));
}

uint32_t dock_rd_u32(dock_rd_t *r)
{
    return dock_get_u32(rd_take(r, 4));
}

const char *dock_rd_str(dock_rd_t *r)
{
    size_t i;
    if (r->underflow)
        return "";
    for (i = r->pos; i < r->len; i++) {
        if (r->p[i] == 0) {
            const char *s = (const char *)(r->p + r->pos);
            r->pos = i + 1;
            return s;
        }
    }
    r->underflow = true;
    return "";
}

size_t dock_rd_left(const dock_rd_t *r)
{
    return r->underflow ? 0 : r->len - r->pos;
}

const uint8_t *dock_rd_ptr(const dock_rd_t *r)
{
    return r->p + r->pos;
}

/* ---- stream parser ---- */

void dock_rx_init(dock_rx_t *rx, uint8_t *buf, size_t cap, dock_sink_fn on_sink,
                  dock_frame_fn on_frame, void *user)
{
    rx->buf = buf;
    rx->cap = cap;
    rx->on_sink = on_sink;
    rx->on_frame = on_frame;
    rx->user = user;
    rx->resyncs = 0;
    dock_rx_reset(rx);
}

void dock_rx_reset(dock_rx_t *rx)
{
    rx->hdr_have = 0;
    rx->in_payload = false;
    rx->got = 0;
    rx->buf_have = 0;
    rx->tail_have = 0;
    rx->truncated = false;
}

static void rx_finish(dock_rx_t *rx)
{
    rx->in_payload = false;
    rx->hdr_have = 0;
    rx->on_frame(rx->user, &rx->cur, rx->buf, rx->buf_have, rx->tail_have,
                 rx->truncated);
}

static void rx_begin(dock_rx_t *rx)
{
    rx->cur.type = rx->hdr[2];
    rx->cur.seq = rx->hdr[3];
    rx->cur.len = dock_get_u24(rx->hdr + 4);
    rx->got = 0;
    rx->buf_have = 0;
    rx->tail_have = 0;
    rx->truncated = false;
    rx->sink.fixed_len = 0;
    rx->sink.tail = NULL;
    rx->sink.tail_cap = 0;
    if (rx->on_sink)
        rx->on_sink(rx->user, &rx->cur, &rx->sink);
    if (rx->sink.tail == NULL)
        rx->sink.fixed_len = rx->cap; /* everything into the buffer */
    if (rx->sink.fixed_len > rx->cap)
        rx->sink.fixed_len = rx->cap;
    if (rx->cur.len == 0) {
        rx_finish(rx);
        return;
    }
    rx->in_payload = true;
}

void dock_rx_feed(dock_rx_t *rx, const uint8_t *data, size_t n)
{
    while (n) {
        if (!rx->in_payload) {
            uint8_t b = *data++;
            n--;
            /* Hunt for magic, then collect the rest of the header. */
            if (rx->hdr_have == 0 && b != DOCK_MAGIC0) {
                rx->resyncs++;
                continue;
            }
            if (rx->hdr_have == 1 && b != DOCK_MAGIC1) {
                rx->resyncs++;
                rx->hdr_have = (b == DOCK_MAGIC0) ? 1 : 0;
                continue;
            }
            rx->hdr[rx->hdr_have++] = b;
            if (rx->hdr_have < DOCK_HDR_LEN)
                continue;
            if (dock_crc8(rx->hdr, 7) != rx->hdr[7] ||
                dock_get_u24(rx->hdr + 4) > DOCK_MAX_PAYLOAD) {
                /* Bad header: drop the first magic byte and re-scan the
                 * other seven (bounded recursion: each level consumes at
                 * least one byte, so depth <= 7). */
                uint8_t again[DOCK_HDR_LEN - 1];
                rx->resyncs++;
                memcpy(again, rx->hdr + 1, sizeof again);
                rx->hdr_have = 0;
                dock_rx_feed(rx, again, sizeof again);
                continue;
            }
            rx_begin(rx);
            continue;
        }

        /* Payload bytes. */
        {
            uint32_t remain = rx->cur.len - rx->got;
            size_t chunk = n;
            if ((uint32_t)chunk > remain)
                chunk = (size_t)remain;

            /* Part 1: fixed part into the buffer. */
            if (rx->got < rx->sink.fixed_len) {
                size_t room = rx->sink.fixed_len - (size_t)rx->got;
                size_t take = chunk < room ? chunk : room;
                memcpy(rx->buf + rx->buf_have, data, take);
                rx->buf_have += take;
                rx->got += (uint32_t)take;
                data += take;
                n -= take;
                chunk -= take;
            }
            /* Part 2: tail (or overflow of a whole-buffer frame). */
            if (chunk) {
                if (rx->sink.tail) {
                    size_t room = rx->sink.tail_cap - rx->tail_have;
                    size_t take = chunk < room ? chunk : room;
                    if (take) {
                        memcpy(rx->sink.tail + rx->tail_have, data, take);
                        rx->tail_have += take;
                    }
                    if (take < chunk)
                        rx->truncated = true;
                } else {
                    rx->truncated = true;
                }
                rx->got += (uint32_t)chunk;
                data += chunk;
                n -= chunk;
            }
            if (rx->got == rx->cur.len)
                rx_finish(rx);
        }
    }
}
