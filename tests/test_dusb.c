/* test_dusb.c - DUSB codec against byte vectors in libticalcs2's format. */
#include "../core/dusb.h"
#include "test.h"

/* ---- fake transport: records sent packets, replays a script ---- */

#define MAX_SENT 1024
#define MAX_SCRIPT 1024
#define SCRIPT_BYTES (200u * 1024u)

typedef struct fio {
    uint8_t sent[MAX_SENT][1100];
    size_t sent_len[MAX_SENT];
    int nsent;
    uint8_t script[SCRIPT_BYTES];
    size_t script_used;
    size_t off[MAX_SCRIPT], len[MAX_SCRIPT];
    int nscript, next;
    uint32_t timeouts[MAX_SCRIPT];
    int nrecv;
} fio_t;

static fio_t io;

static bool fio_send(void *user, const uint8_t *p, size_t n)
{
    fio_t *f = (fio_t *)user;
    if (f->nsent >= MAX_SENT || n > sizeof f->sent[0])
        return false;
    memcpy(f->sent[f->nsent], p, n);
    f->sent_len[f->nsent++] = n;
    return true;
}

static size_t fio_recv(void *user, uint8_t *p, size_t cap, uint32_t timeout_ms)
{
    fio_t *f = (fio_t *)user;
    size_t n;
    if (f->nrecv < MAX_SCRIPT)
        f->timeouts[f->nrecv] = timeout_ms;
    f->nrecv++;
    if (f->next >= f->nscript)
        return 0;
    n = f->len[f->next];
    if (n > cap)
        n = cap;
    memcpy(p, f->script + f->off[f->next], n);
    f->next++;
    return n;
}

static void fio_reset(void)
{
    memset(&io, 0, sizeof io);
}

/* Drop the (fully consumed) script and the sent log before a new scenario. */
static void script_clear(void)
{
    CHECK_EQ(io.next, io.nscript);
    io.nscript = io.next = 0;
    io.script_used = 0;
    io.nsent = 0;
}

/* Queue one raw packet from the calculator. */
static void script_raw(uint8_t type, const uint8_t *data, size_t n)
{
    uint8_t *p = io.script + io.script_used;
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
    p[4] = type;
    if (n)
        memcpy(p + 5, data, n);
    io.off[io.nscript] = io.script_used;
    io.len[io.nscript++] = n + 5;
    io.script_used += n + 5;
}

static void script_bytes(const uint8_t *b, size_t n)
{
    memcpy(io.script + io.script_used, b, n);
    io.off[io.nscript] = io.script_used;
    io.len[io.nscript++] = n;
    io.script_used += n;
}

static void script_ack(void)
{
    static const uint8_t a[2] = {0xE0, 0x00};
    script_raw(5, a, 2);
}

/* Queue a virtual packet split into raw packets of at most `raw` bytes. */
static void script_vpkt(uint16_t vtype, const uint8_t *data, uint32_t n, uint32_t raw)
{
    static uint8_t tmp[1100];
    uint32_t off = 0;
    size_t first = n + 6 <= raw ? n : raw - 6;
    tmp[0] = (uint8_t)(n >> 24);
    tmp[1] = (uint8_t)(n >> 16);
    tmp[2] = (uint8_t)(n >> 8);
    tmp[3] = (uint8_t)n;
    tmp[4] = (uint8_t)(vtype >> 8);
    tmp[5] = (uint8_t)vtype;
    if (first)
        memcpy(tmp + 6, data, first);
    off = (uint32_t)first;
    script_raw(off == n ? 4 : 3, tmp, first + 6);
    while (off < n) {
        uint32_t c = n - off < raw ? n - off : raw;
        script_raw(off + c == n && c < raw ? 4 : 3, data + off, c);
        off += c;
    }
    if (n + 6 > raw && (n - (raw - 6)) % raw == 0)
        script_raw(4, NULL, 0); /* libticalcs-style empty final fragment */
}

static dock_dusb_t d;
static uint8_t vbuf[DOCK_DUSB_SCREEN_BYTES + 64];

static void dusb_setup(size_t vcap)
{
    dock_dusb_io_t i;
    i.user = &io;
    i.send = fio_send;
    i.recv = fio_recv;
    dock_dusb_init(&d, &i, vbuf, vcap);
}

static const uint8_t ACK[7] = {0, 0, 0, 2, 5, 0xE0, 0x00};

#define CHECK_SENT(i, ...)                                                   \
    do {                                                                     \
        static const uint8_t want_[] = {__VA_ARGS__};                        \
        CHECK_EQ(io.sent_len[i], sizeof want_);                              \
        if (io.sent_len[i] == sizeof want_)                                  \
            CHECK_MEM(io.sent[i], want_, sizeof want_);                      \
    } while (0)

/* ---- tests ---- */

static void t_builders(void)
{
    uint8_t out[16];
    static const uint8_t data[3] = {0xAA, 0xBB, 0xCC};
    CHECK_EQ(dock_dusb_build_raw(out, 4, data, 3), 8);
    {
        static const uint8_t want[8] = {0, 0, 0, 3, 4, 0xAA, 0xBB, 0xCC};
        CHECK_MEM(out, want, 8);
    }
    CHECK_EQ(dock_dusb_build_virtual_header(out, 0x01020304u, 0xBB00), 6);
    {
        static const uint8_t want[6] = {1, 2, 3, 4, 0xBB, 0x00};
        CHECK_MEM(out, want, 6);
    }
}

static void do_handshake(uint32_t alloc)
{
    uint8_t a[4];
    a[0] = (uint8_t)(alloc >> 24);
    a[1] = (uint8_t)(alloc >> 16);
    a[2] = (uint8_t)(alloc >> 8);
    a[3] = (uint8_t)alloc;
    script_raw(2, a, 4);
    script_ack();
    script_vpkt(0x0012, NULL, 0, 1018);
    CHECK_EQ(dock_dusb_handshake(&d), DOCK_OK);
}

static void t_handshake(void)
{
    fio_reset();
    dusb_setup(sizeof vbuf);
    do_handshake(1018);
    CHECK_EQ(io.nsent, 3);
    /* Buffer Size Request, 1024 (DUSB_DFL_BUF_SIZE) */
    CHECK_SENT(0, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x04, 0x00);
    /* Ping / Set Mode: DUSB_MODE_NORMAL {3, 1, 0, 0, 0x07d0} */
    CHECK_SENT(1, 0x00, 0x00, 0x00, 0x10, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x00,
               0x01, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x07, 0xD0);
    CHECK_MEM(io.sent[2], ACK, 7); /* we ack the mode ack */
    CHECK_EQ(d.raw_size, 1018);

    /* The CE over-allocates: clamp to 1018 like libticalcs. */
    fio_reset();
    dusb_setup(sizeof vbuf);
    do_handshake(1024);
    CHECK_EQ(d.raw_size, 1018);

    /* Delay ack before the mode ack. */
    fio_reset();
    dusb_setup(sizeof vbuf);
    {
        static const uint8_t a[4] = {0, 0, 3, 0xFA};
        static const uint8_t delay[4] = {0x00, 0x00, 0x4E, 0x20}; /* 20000 us */
        script_raw(2, a, 4);
        script_ack();
        script_vpkt(0xBB00, delay, 4, 1018);
        script_vpkt(0x0012, NULL, 0, 1018);
        CHECK_EQ(dock_dusb_handshake(&d), DOCK_OK);
        CHECK_EQ(io.nsent, 4);
        CHECK_MEM(io.sent[2], ACK, 7);
        CHECK_MEM(io.sent[3], ACK, 7);
        /* waited timeout + 20 ms (+100 ms slack) for the packet after it */
        CHECK_EQ(io.timeouts[io.nrecv - 1], d.timeout_ms + 20u + 100u);
    }

    /* Wrong replies. */
    fio_reset();
    dusb_setup(sizeof vbuf);
    CHECK_EQ(dock_dusb_handshake(&d), DOCK_E_TIMEOUT);
    fio_reset();
    dusb_setup(sizeof vbuf);
    {
        static const uint8_t a[4] = {0, 0, 0, 4};
        script_raw(2, a, 4); /* absurdly small buffer */
        CHECK_EQ(dock_dusb_handshake(&d), DOCK_E_INVALID);
    }
    fio_reset();
    dusb_setup(sizeof vbuf);
    {
        static const uint8_t bad[9] = {0, 0, 0, 9, 2, 0, 0, 3, 0xFA}; /* size lies */
        script_bytes(bad, sizeof bad);
        CHECK_EQ(dock_dusb_handshake(&d), DOCK_E_INVALID);
    }
    fio_reset();
    dusb_setup(sizeof vbuf);
    {
        static const uint8_t a[4] = {0, 0, 3, 0xFA};
        static const uint8_t err[2] = {0x00, 0x12};
        script_raw(2, a, 4);
        script_ack();
        script_vpkt(0xEE00, err, 2, 1018);
        CHECK_EQ(dock_dusb_handshake(&d), DOCK_E_IO);
    }
}

static void t_send_key(void)
{
    static const uint8_t delay[4] = {0x00, 0x00, 0x27, 0x10}; /* 10 ms */
    fio_reset();
    dusb_setup(sizeof vbuf);
    do_handshake(1018);
    io.nsent = 0;

    /* ENTER (k_Enter = 0x05): one-byte key -> {0x00, 0x05} */
    script_ack();
    script_vpkt(0xBB00, delay, 4, 1018);
    script_vpkt(0xAA00, NULL, 0, 1018);
    CHECK_EQ(dock_dusb_send_key(&d, 0x0005), DOCK_OK);
    CHECK_EQ(io.nsent, 3);
    CHECK_SENT(0, 0x00, 0x00, 0x00, 0x0B, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00,
               0x11, 0x00, 0x00, 0x03, 0x00, 0x05);
    CHECK_MEM(io.sent[1], ACK, 7);
    CHECK_MEM(io.sent[2], ACK, 7);

    /* lowercase a (KEY83P_La = 0xFCE2): {keyExtend 0xE2, kbdKey 0xFC} */
    io.nsent = 0;
    script_ack();
    script_vpkt(0xBB00, delay, 4, 1018);
    script_vpkt(0xAA00, NULL, 0, 1018);
    CHECK_EQ(dock_dusb_send_key(&d, 0xFCE2), DOCK_OK);
    CHECK_SENT(0, 0x00, 0x00, 0x00, 0x0B, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00,
               0x11, 0x00, 0x00, 0x03, 0xE2, 0xFC);

    /* No DELAY_ACK at all is fine too. */
    io.nsent = 0;
    script_ack();
    script_vpkt(0xAA00, NULL, 0, 1018);
    CHECK_EQ(dock_dusb_send_key(&d, 0x0009), DOCK_OK);

    /* Calculator asks for a buffer size while we wait for the ack. */
    io.nsent = 0;
    {
        static const uint8_t req[4] = {0x00, 0x00, 0x03, 0xFA};
        script_raw(1, req, 4);
        script_ack();
        script_vpkt(0xAA00, NULL, 0, 1018);
        CHECK_EQ(dock_dusb_send_key(&d, 0x0009), DOCK_OK);
        CHECK_SENT(1, 0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0x03, 0xFA);
    }

    /* Wrong final reply type. */
    script_ack();
    script_vpkt(0x0012, NULL, 0, 1018);
    CHECK_EQ(dock_dusb_send_key(&d, 0x0009), DOCK_E_INVALID);
    /* No ack. */
    CHECK_EQ(dock_dusb_send_key(&d, 0x0009), DOCK_E_TIMEOUT);

    /* A tiny negotiated buffer forces fragmentation of our own packet. */
    io.nsent = 0;
    d.raw_size = 8;
    script_ack();
    script_ack();
    script_vpkt(0xAA00, NULL, 0, 1018);
    CHECK_EQ(dock_dusb_send_key(&d, 0x0005), DOCK_OK);
    CHECK_SENT(0, 0x00, 0x00, 0x00, 0x08, 0x03, 0x00, 0x00, 0x00, 0x05, 0x00,
               0x11, 0x00, 0x00);
    CHECK_SENT(1, 0x00, 0x00, 0x00, 0x03, 0x04, 0x03, 0x00, 0x05);
}

static uint8_t image[DOCK_DUSB_SCREEN_BYTES];
static uint8_t reply[7 + DOCK_DUSB_SCREEN_BYTES];
static uint8_t shot[DOCK_DUSB_SCREEN_BYTES];

static void make_reply(void)
{
    uint32_t i, seed = 99;
    for (i = 0; i < sizeof image; i++)
        image[i] = (uint8_t)t_rand(&seed);
    reply[0] = 0x00; /* count = 1 */
    reply[1] = 0x01;
    reply[2] = 0x00; /* PID_SCREENSHOT */
    reply[3] = 0x22;
    reply[4] = 0x00; /* ok */
    reply[5] = 0x58; /* size, truncated to 16 bits (153600 & 0xFFFF) */
    reply[6] = 0x00;
    memcpy(reply + 7, image, sizeof image);
}

static void t_screenshot(void)
{
    int i, raws;
    make_reply();
    fio_reset();
    dusb_setup(sizeof vbuf);
    do_handshake(1018);
    io.nsent = 0;

    script_ack();
    raws = io.nscript;
    script_vpkt(0x0008, reply, sizeof reply, 1018);
    raws = io.nscript - raws;
    CHECK(raws > 100);
    memset(shot, 0, sizeof shot);
    CHECK_EQ(dock_dusb_screenshot(&d, shot, sizeof shot), DOCK_OK);
    CHECK_MEM(shot, image, sizeof image);
    /* Parameter Request {count 1, PID_SCREENSHOT} */
    CHECK_SENT(0, 0x00, 0x00, 0x00, 0x0A, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00,
               0x07, 0x00, 0x01, 0x00, 0x22);
    CHECK_EQ(io.nsent, 1 + raws);
    for (i = 1; i < io.nsent; i++)
        CHECK_MEM(io.sent[i], ACK, 7);

    /* Different fragment size, preceded by a delay. */
    script_clear();
    {
        static const uint8_t delay[4] = {0, 0, 0, 0};
        script_ack();
        script_vpkt(0xBB00, delay, 4, 250);
        script_vpkt(0x0008, reply, sizeof reply, 250);
        memset(shot, 0, sizeof shot);
        CHECK_EQ(dock_dusb_screenshot(&d, shot, sizeof shot), DOCK_OK);
        CHECK_MEM(shot, image, sizeof image);
    }

    /* Destination too small. */
    CHECK_EQ(dock_dusb_screenshot(&d, shot, sizeof shot - 1), DOCK_E_TOO_BIG);

    /* Reassembly buffer too small: everything is still acked (drained). */
    script_clear();
    dusb_setup(1000);
    d.raw_size = 1018;
    script_ack();
    raws = io.nscript;
    script_vpkt(0x0008, reply, sizeof reply, 1018);
    raws = io.nscript - raws;
    CHECK_EQ(dock_dusb_screenshot(&d, shot, sizeof shot), DOCK_E_TOO_BIG);
    CHECK_EQ(io.nsent, 1 + raws);
    CHECK_EQ(io.next, io.nscript);

    /* Refused parameter / short image. */
    script_clear();
    dusb_setup(sizeof vbuf);
    d.raw_size = 1018;
    reply[4] = 0x01;
    script_ack();
    script_vpkt(0x0008, reply, sizeof reply, 1018);
    CHECK_EQ(dock_dusb_screenshot(&d, shot, sizeof shot), DOCK_E_INVALID);
    reply[4] = 0x00;
    script_clear();
    script_ack();
    script_vpkt(0x0008, reply, sizeof reply - 2, 1018);
    CHECK_EQ(dock_dusb_screenshot(&d, shot, sizeof shot), DOCK_E_INVALID);

    /* A fragment that is neither data nor a buffer request. */
    script_clear();
    script_ack();
    {
        static const uint8_t junk[2] = {0xE0, 0x00};
        script_raw(5, junk, 2);
        CHECK_EQ(dock_dusb_screenshot(&d, shot, sizeof shot), DOCK_E_INVALID);
    }
}

void suite_dusb(void)
{
    RUN(t_builders);
    RUN(t_handshake);
    RUN(t_send_key);
    RUN(t_screenshot);
}
