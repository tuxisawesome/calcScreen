/* test_proto.c - framing, payload writer/reader and the stream parser. */
#include "../core/dock_proto.h"
#include "test.h"

/* ---- frame collector ---- */

#define MAXF 32

typedef struct got_frame {
    dock_frame_t f;
    uint8_t buf[512];
    size_t buf_len;
    size_t tail_len;
    bool truncated;
} got_frame_t;

typedef struct collector {
    got_frame_t fr[MAXF];
    int n;
    /* split-sink configuration */
    bool use_sink;
    size_t fixed_len;
    uint8_t *tail;
    size_t tail_cap;
} collector_t;

static void col_sink(void *user, const dock_frame_t *f, dock_sink_t *out)
{
    collector_t *c = (collector_t *)user;
    (void)f;
    if (c->use_sink) {
        out->fixed_len = c->fixed_len;
        out->tail = c->tail;
        out->tail_cap = c->tail_cap;
    }
}

static void col_frame(void *user, const dock_frame_t *f, const uint8_t *buf,
                      size_t buf_len, size_t tail_len, bool truncated)
{
    collector_t *c = (collector_t *)user;
    got_frame_t *g;
    if (c->n >= MAXF)
        return;
    g = &c->fr[c->n++];
    g->f = *f;
    g->buf_len = buf_len;
    memcpy(g->buf, buf, buf_len < sizeof g->buf ? buf_len : sizeof g->buf);
    g->tail_len = tail_len;
    g->truncated = truncated;
}

/* Append one frame to a stream buffer. */
static size_t put_frame(uint8_t *out, uint8_t type, uint8_t seq,
                        const uint8_t *payload, size_t n)
{
    dock_hdr_build(out, type, seq, (uint32_t)n);
    if (n)
        memcpy(out + DOCK_HDR_LEN, payload, n);
    return DOCK_HDR_LEN + n;
}

/* ---- tests ---- */

static void t_crc8_and_header(void)
{
    uint8_t hdr[DOCK_HDR_LEN];
    CHECK_EQ(dock_crc8((const uint8_t *)"123456789", 9), 0xF4); /* CRC-8/SMBUS */
    CHECK_EQ(dock_crc8(NULL, 0), 0);
    dock_hdr_build(hdr, 0x12, 0x34, 0x010203u);
    CHECK_EQ(hdr[0], 0xD0);
    CHECK_EQ(hdr[1], 0xC5);
    CHECK_EQ(hdr[2], 0x12);
    CHECK_EQ(hdr[3], 0x34);
    CHECK_EQ(hdr[4], 0x03);
    CHECK_EQ(hdr[5], 0x02);
    CHECK_EQ(hdr[6], 0x01);
    CHECK_EQ(hdr[7], dock_crc8(hdr, 7));
}

static void t_writer(void)
{
    uint8_t buf[8];
    dock_wr_t w;
    dock_wr_init(&w, buf, 5);
    dock_wr_u32(&w, 0x04030201u);
    CHECK_EQ(w.len, 4);
    CHECK(!w.overflow);
    CHECK_EQ(buf[0], 1);
    CHECK_EQ(buf[3], 4);
    dock_wr_u16(&w, 0xAAAA); /* does not fit */
    CHECK(w.overflow);
    CHECK_EQ(w.len, 4);
    dock_wr_u8(&w, 9); /* sticky: refused even though it would fit */
    CHECK_EQ(w.len, 4);

    dock_wr_init(&w, buf, sizeof buf);
    dock_wr_u24(&w, 0x123456u);
    dock_wr_str(&w, "ab");
    CHECK_EQ(w.len, 6);
    CHECK_EQ(buf[0], 0x56);
    CHECK_EQ(buf[2], 0x12);
    CHECK_EQ(buf[5], 0);
    dock_wr_str(&w, "xyz"); /* 4 bytes, only 2 left */
    CHECK(w.overflow);
    dock_wr_init(&w, buf, 0);
    dock_wr_bytes(&w, NULL, 0);
    CHECK(!w.overflow);
}

static void t_reader(void)
{
    static const uint8_t data[] = {1, 2, 3, 4, 5, 'h', 'i', 0, 'x'};
    dock_rd_t r;
    dock_rd_init(&r, data, sizeof data);
    CHECK_EQ(dock_rd_u8(&r), 1);
    CHECK_EQ(dock_rd_u16(&r), 0x0302);
    CHECK_EQ(dock_rd_u16(&r), 0x0504);
    CHECK_STR(dock_rd_str(&r), "hi");
    CHECK_EQ(dock_rd_left(&r), 1);
    CHECK(!r.underflow);
    CHECK_STR(dock_rd_str(&r), ""); /* no terminator */
    CHECK(r.underflow);
    CHECK_EQ(dock_rd_left(&r), 0);

    dock_rd_init(&r, data, 3);
    CHECK_EQ(dock_rd_u32(&r), 0); /* too short: zeros */
    CHECK(r.underflow);
    CHECK_EQ(dock_rd_u8(&r), 0); /* sticky */
    dock_rd_init(&r, data, 3);
    CHECK_EQ(dock_rd_u24(&r), 0x030201u);
    CHECK(!r.underflow);
}

static size_t sample_stream(uint8_t *s, uint8_t *p)
{
    size_t n = 0, i;
    for (i = 0; i < 300; i++)
        p[i] = (uint8_t)(i * 7u + 1u);
    n += put_frame(s + n, DOCK_T_PING, 1, NULL, 0);
    n += put_frame(s + n, DOCK_T_LOG, 2, (const uint8_t *)"hello", 5);
    n += put_frame(s + n, DOCK_T_SCR_ROWS, 3, p, 300);
    n += put_frame(s + n, DOCK_T_RESP, 4, p + 10, 1);
    return n;
}

static void check_sample(const collector_t *c, const uint8_t *p)
{
    CHECK_EQ(c->n, 4);
    if (c->n != 4)
        return;
    CHECK_EQ(c->fr[0].f.type, DOCK_T_PING);
    CHECK_EQ(c->fr[0].f.len, 0);
    CHECK_EQ(c->fr[1].f.type, DOCK_T_LOG);
    CHECK_EQ(c->fr[1].f.seq, 2);
    CHECK_EQ(c->fr[1].buf_len, 5);
    CHECK_MEM(c->fr[1].buf, "hello", 5);
    CHECK_EQ(c->fr[2].f.len, 300);
    CHECK_EQ(c->fr[2].buf_len, 300);
    CHECK_MEM(c->fr[2].buf, p, 300);
    CHECK(!c->fr[2].truncated);
    CHECK_EQ(c->fr[3].f.seq, 4);
    CHECK_EQ(c->fr[3].buf[0], p[10]);
}

static void t_parser_roundtrip(void)
{
    static uint8_t s[1024], p[300], rxbuf[400];
    static collector_t c;
    dock_rx_t rx;
    size_t n = sample_stream(s, p), i;

    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, col_sink, col_frame, &c);
    dock_rx_feed(&rx, s, n);
    check_sample(&c, p);
    CHECK_EQ(rx.resyncs, 0);

    /* byte by byte */
    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, col_sink, col_frame, &c);
    for (i = 0; i < n; i++)
        dock_rx_feed(&rx, s + i, 1);
    check_sample(&c, p);

    /* odd chunk sizes */
    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, NULL, col_frame, &c);
    for (i = 0; i < n; i += 13)
        dock_rx_feed(&rx, s + i, n - i < 13 ? n - i : 13);
    check_sample(&c, p);
}

static void t_parser_resync(void)
{
    static uint8_t s[2048], p[300], rxbuf[400];
    static collector_t c;
    static const uint8_t garbage[] = {0x00, 0xD0, 0xD0, 0xC5, 0x01, 0xD0,
                                      0xC5, 0x07, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0xFF, 0xD0};
    dock_rx_t rx;
    size_t n = 0;

    /* garbage (including fake magics with a bad CRC) before a valid stream */
    memcpy(s, garbage, sizeof garbage);
    n = sizeof garbage;
    n += sample_stream(s + n, p);
    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, NULL, col_frame, &c);
    dock_rx_feed(&rx, s, n);
    check_sample(&c, p);
    CHECK(rx.resyncs > 0);

    /* same, byte by byte */
    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, NULL, col_frame, &c);
    {
        size_t i;
        for (i = 0; i < n; i++)
            dock_rx_feed(&rx, s + i, 1);
    }
    check_sample(&c, p);
}

static void t_parser_corrupt_header(void)
{
    static uint8_t s[1024], rxbuf[64];
    static collector_t c;
    static const uint8_t pay[4] = {0x11, 0x22, 0x33, 0x44};
    dock_rx_t rx;
    size_t n = 0, f2;

    n += put_frame(s + n, DOCK_T_LOG, 1, pay, 4);
    f2 = n;
    n += put_frame(s + n, DOCK_T_LOG, 2, pay, 4);
    n += put_frame(s + n, DOCK_T_LOG, 3, pay, 4);
    s[f2 + 7] ^= 0x5A; /* break frame 2's header CRC */

    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, NULL, col_frame, &c);
    dock_rx_feed(&rx, s, n);
    CHECK_EQ(c.n, 2);
    CHECK_EQ(c.fr[0].f.seq, 1);
    CHECK_EQ(c.fr[1].f.seq, 3);
    CHECK_MEM(c.fr[1].buf, pay, 4);

    /* A header whose CRC is fine but whose length is absurd. */
    n = 0;
    {
        uint8_t hdr[DOCK_HDR_LEN];
        dock_hdr_build(hdr, DOCK_T_LOG, 9, DOCK_MAX_PAYLOAD + 1u);
        memcpy(s, hdr, sizeof hdr);
        n = sizeof hdr;
    }
    n += put_frame(s + n, DOCK_T_LOG, 10, pay, 4);
    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, NULL, col_frame, &c);
    dock_rx_feed(&rx, s, n);
    CHECK_EQ(c.n, 1);
    CHECK_EQ(c.fr[0].f.seq, 10);

    /* Corrupted header where the stray bytes contain the next magic. */
    n = 0;
    s[n++] = DOCK_MAGIC0;
    s[n++] = DOCK_MAGIC1;
    s[n++] = 0x99;
    n += put_frame(s + n, DOCK_T_LOG, 11, pay, 4);
    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, NULL, col_frame, &c);
    dock_rx_feed(&rx, s, n);
    CHECK_EQ(c.n, 1);
    CHECK_EQ(c.fr[0].f.seq, 11);
}

static void t_parser_oversize_buffer(void)
{
    static uint8_t s[512], p[200], rxbuf[16];
    static collector_t c;
    dock_rx_t rx;
    size_t n = 0, i;
    for (i = 0; i < sizeof p; i++)
        p[i] = (uint8_t)i;
    n += put_frame(s + n, DOCK_T_LOG, 1, p, 200);  /* bigger than rxbuf */
    n += put_frame(s + n, DOCK_T_LOG, 2, p, 16);   /* exactly fits */
    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, NULL, col_frame, &c);
    dock_rx_feed(&rx, s, n);
    CHECK_EQ(c.n, 2);
    CHECK(c.fr[0].truncated);
    CHECK_EQ(c.fr[0].f.len, 200);
    CHECK_EQ(c.fr[0].buf_len, 16);
    CHECK_MEM(c.fr[0].buf, p, 16);
    CHECK(!c.fr[1].truncated);
    CHECK_EQ(c.fr[1].buf_len, 16);
}

static void t_parser_split_sink(void)
{
    static uint8_t s[512], p[64], rxbuf[32], tail[10];
    static collector_t c;
    dock_rx_t rx;
    size_t n = 0, i, pass;
    for (i = 0; i < sizeof p; i++)
        p[i] = (uint8_t)(0xA0u + i);
    n += put_frame(s + n, DOCK_T_RESP, 1, p, 12); /* 4 fixed + 8 tail */
    n += put_frame(s + n, DOCK_T_RESP, 2, p, 19); /* 4 fixed + 15 > 10 */
    n += put_frame(s + n, DOCK_T_RESP, 3, p, 3);  /* shorter than fixed */

    for (pass = 0; pass < 2; pass++) {
        memset(&c, 0, sizeof c);
        c.use_sink = true;
        c.fixed_len = 4;
        c.tail = tail;
        c.tail_cap = sizeof tail;
        memset(tail, 0, sizeof tail);
        dock_rx_init(&rx, rxbuf, sizeof rxbuf, col_sink, col_frame, &c);
        if (pass == 0) {
            dock_rx_feed(&rx, s, n);
        } else {
            for (i = 0; i < n; i++)
                dock_rx_feed(&rx, s + i, 1);
        }
        CHECK_EQ(c.n, 3);
        CHECK_EQ(c.fr[0].buf_len, 4);
        CHECK_MEM(c.fr[0].buf, p, 4);
        CHECK_EQ(c.fr[0].tail_len, 8);
        CHECK(!c.fr[0].truncated);
        CHECK_EQ(c.fr[1].buf_len, 4);
        CHECK_EQ(c.fr[1].tail_len, 10);
        CHECK(c.fr[1].truncated);
        CHECK_MEM(tail, p + 4, 10); /* frame 2 overwrote frame 1's tail */
        CHECK_EQ(c.fr[2].buf_len, 3);
        CHECK_EQ(c.fr[2].tail_len, 0);
        CHECK(!c.fr[2].truncated);
    }

    /* fixed_len larger than the parser buffer is clamped */
    memset(&c, 0, sizeof c);
    c.use_sink = true;
    c.fixed_len = 1000;
    c.tail = tail;
    c.tail_cap = sizeof tail;
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, col_sink, col_frame, &c);
    n = put_frame(s, DOCK_T_RESP, 5, p, 40);
    dock_rx_feed(&rx, s, n);
    CHECK_EQ(c.n, 1);
    CHECK_EQ(c.fr[0].buf_len, 32);
    CHECK_EQ(c.fr[0].tail_len, 8);
    CHECK_MEM(tail, p + 32, 8);
}

static void t_parser_reset(void)
{
    static uint8_t s[64], rxbuf[32];
    static collector_t c;
    dock_rx_t rx;
    size_t n = put_frame(s, DOCK_T_LOG, 1, (const uint8_t *)"abcdef", 6);
    memset(&c, 0, sizeof c);
    dock_rx_init(&rx, rxbuf, sizeof rxbuf, NULL, col_frame, &c);
    dock_rx_feed(&rx, s, n - 2); /* partial frame, then the link restarts */
    dock_rx_reset(&rx);
    dock_rx_feed(&rx, s, n);
    CHECK_EQ(c.n, 1);
    CHECK_MEM(c.fr[0].buf, "abcdef", 6);
}

void suite_proto(void)
{
    RUN(t_crc8_and_header);
    RUN(t_writer);
    RUN(t_reader);
    RUN(t_parser_roundtrip);
    RUN(t_parser_resync);
    RUN(t_parser_corrupt_header);
    RUN(t_parser_oversize_buffer);
    RUN(t_parser_split_sink);
    RUN(t_parser_reset);
}
