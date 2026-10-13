/*
 * dusb.c - minimal DUSB host (screenshots and key presses on a stock
 * TI-84 Plus CE). Byte layouts follow libticalcs2:
 *
 * Raw packet (dusb_rpkt.cc, one bulk transfer):
 *   u32 BE size (of data), u8 type, data[size]
 *   1 BUF_SIZE_REQ   u32 BE size     host offers its buffer size
 *   2 BUF_SIZE_ALLOC u32 BE size     peer's answer = max raw data size
 *   3 VIRT_DATA      virtual packet fragment, more follow
 *   4 VIRT_DATA_LAST last fragment
 *   5 VIRT_DATA_ACK  u16 BE 0xE000  sent for every fragment received
 *
 * Virtual packet (dusb_vpkt.cc): the first fragment starts with
 *   u32 BE size (of payload), u16 BE type, then payload bytes; the following
 *   fragments carry payload only. Every fragment is acknowledged before the
 *   next one is sent.
 *
 * Commands (dusb_cmd.cc, calc_84p.cc), all fields big-endian:
 *   handshake : BUF_SIZE_REQ(1024) -> BUF_SIZE_ALLOC (CE: clamp to 1018);
 *               vpkt 0x0001 {3,1,0,0,0x07D0} (DUSB_MODE_NORMAL) -> 0x0012
 *   screenshot: vpkt 0x0007 {u16 1, u16 0x0022} -> 0x0008 {u16 count,
 *               u16 id, u8 status (0 = ok), u16 size (truncated), data}
 *               with data = 320x240 RGB565 little-endian (calc_84p.cc)
 *   key       : vpkt 0x0011 {u8 0 (folder len), u8 0 (name len),
 *               u8 3 (DUSB_EID_KEY), key} -> 0xBB00 (delay) then 0xAA00.
 *               key: 1-byte codes as {0, code}; 2-byte codes as
 *               {keyExtend, kbdKey} i.e. low byte first (dusb_cmd_s_execute)
 *   0xBB00 DELAY_ACK {u32 BE microseconds} may precede any reply: wait and
 *               read again (CATCH_DELAY). 0xEE00 ERROR {u16 BE code}.
 */
#include "dusb.h"

#define RPKT_BUF_SIZE_REQ 1u
#define RPKT_BUF_SIZE_ALLOC 2u
#define RPKT_VIRT_DATA 3u
#define RPKT_VIRT_DATA_LAST 4u
#define RPKT_VIRT_DATA_ACK 5u

#define VPKT_PING 0x0001u /* "set mode" */
#define VPKT_PARM_REQ 0x0007u
#define VPKT_PARM_DATA 0x0008u
#define VPKT_EXECUTE 0x0011u
#define VPKT_MODE_SET 0x0012u
#define VPKT_DATA_ACK 0xAA00u
#define VPKT_DELAY_ACK 0xBB00u
#define VPKT_ERROR 0xEE00u

#define RAW_HDR 5u
#define VIRT_HDR 6u
#define DFL_BUF_SIZE 1024u   /* DUSB_DFL_BUF_SIZE */
#define CE_MAX_RAW 1018u     /* "the 83PCE/84+CE allocate more than they support" */
#define PID_SCREENSHOT 0x0022u
#define EID_KEY 0x03u
#define MAX_DELAYS 16u
#define MAX_DELAY_US 400000u

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

size_t dock_dusb_build_raw(uint8_t *out, uint8_t type, const uint8_t *data,
                           size_t n)
{
    put_be32(out, (uint32_t)n);
    out[4] = type;
    if (n)
        memmove(out + RAW_HDR, data, n);
    return RAW_HDR + n;
}

size_t dock_dusb_build_virtual_header(uint8_t *out, uint32_t size,
                                      uint16_t vtype)
{
    put_be32(out, size);
    out[4] = (uint8_t)(vtype >> 8);
    out[5] = (uint8_t)vtype;
    return VIRT_HDR;
}

void dock_dusb_init(dock_dusb_t *d, const dock_dusb_io_t *io, uint8_t *vbuf,
                    size_t vcap)
{
    memset(d, 0, sizeof *d);
    d->io = *io;
    d->vbuf = vbuf;
    d->vcap = vbuf ? vcap : 0;
    d->raw_size = 250u; /* safe minimum until negotiated */
    d->timeout_ms = 2000u;
}

/* ---- raw packets ---- */

static int raw_send(dock_dusb_t *d, uint8_t type, const uint8_t *data, size_t n)
{
    size_t len = dock_dusb_build_raw(d->pkt, type, data, n);
    return d->io.send(d->io.user, d->pkt, len) ? DOCK_OK : DOCK_E_IO;
}

static int raw_send_u32(dock_dusb_t *d, uint8_t type, uint32_t v)
{
    uint8_t b[4];
    put_be32(b, v);
    return raw_send(d, type, b, sizeof b);
}

/* Receive one raw packet into d->pkt. */
static int raw_recv(dock_dusb_t *d, uint32_t timeout_ms, uint8_t *type,
                    size_t *len)
{
    size_t n = d->io.recv(d->io.user, d->pkt, sizeof d->pkt, timeout_ms);
    if (n == 0)
        return DOCK_E_TIMEOUT;
    if (n < RAW_HDR || n > sizeof d->pkt || get_be32(d->pkt) != n - RAW_HDR)
        return DOCK_E_INVALID;
    *type = d->pkt[4];
    *len = n - RAW_HDR;
    return DOCK_OK;
}

static int send_ack(dock_dusb_t *d)
{
    static const uint8_t ack[2] = {0xE0, 0x00};
    return raw_send(d, RPKT_VIRT_DATA_ACK, ack, sizeof ack);
}

static uint32_t clamp_raw(uint32_t size)
{
    if (size > CE_MAX_RAW)
        size = CE_MAX_RAW;
    if (size > DOCK_DUSB_RAW_MAX)
        size = DOCK_DUSB_RAW_MAX;
    return size;
}

/* The calculator may ask for a buffer size at any time; answer like
 * libticalcs (dusb_recv_acknowledge -> dusb_send_buf_size_alloc). */
static int answer_buf_size_req(dock_dusb_t *d, size_t len)
{
    uint32_t want;
    if (len != 4)
        return DOCK_E_INVALID;
    want = clamp_raw(get_be32(d->pkt + RAW_HDR));
    if (want <= VIRT_HDR)
        return DOCK_E_INVALID;
    d->raw_size = want;
    return raw_send_u32(d, RPKT_BUF_SIZE_ALLOC, want);
}

static int recv_ack(dock_dusb_t *d)
{
    uint8_t type;
    size_t len;
    int rc = raw_recv(d, d->timeout_ms, &type, &len);
    if (rc == DOCK_OK && type == RPKT_BUF_SIZE_REQ) {
        rc = answer_buf_size_req(d, len);
        if (rc == DOCK_OK)
            rc = raw_recv(d, d->timeout_ms, &type, &len);
    }
    if (rc != DOCK_OK)
        return rc;
    if (type != RPKT_VIRT_DATA_ACK || len != 2 || d->pkt[RAW_HDR] != 0xE0u)
        return DOCK_E_INVALID;
    return DOCK_OK;
}

/* CE quirk (dusb_vpkt.cc workaround_send): a zero-length bulk write must
 * follow a final fragment whose transfer is a multiple of 64 bytes. */
static int send_last(dock_dusb_t *d, size_t len)
{
    if (!d->io.send(d->io.user, d->pkt, len))
        return DOCK_E_IO;
    if (len % 64u == 0 && !d->io.send(d->io.user, d->pkt, 0))
        return DOCK_E_IO;
    return DOCK_OK;
}

/* ---- virtual packets ---- */

static int send_vpkt(dock_dusb_t *d, uint16_t vtype, const uint8_t *data,
                     uint32_t n)
{
    uint32_t max = d->raw_size, off, chunk;
    int rc;

    if (n <= max - VIRT_HDR) {
        dock_dusb_build_virtual_header(d->pkt + RAW_HDR, n, vtype);
        if (n)
            memcpy(d->pkt + RAW_HDR + VIRT_HDR, data, (size_t)n);
        put_be32(d->pkt, n + VIRT_HDR);
        d->pkt[4] = RPKT_VIRT_DATA_LAST;
        rc = send_last(d, RAW_HDR + VIRT_HDR + (size_t)n);
        return rc == DOCK_OK ? recv_ack(d) : rc;
    }

    /* First fragment: full, with the virtual header. */
    dock_dusb_build_virtual_header(d->pkt + RAW_HDR, n, vtype);
    memcpy(d->pkt + RAW_HDR + VIRT_HDR, data, (size_t)(max - VIRT_HDR));
    put_be32(d->pkt, max);
    d->pkt[4] = RPKT_VIRT_DATA;
    if (!d->io.send(d->io.user, d->pkt, RAW_HDR + (size_t)max))
        return DOCK_E_IO;
    rc = recv_ack(d);
    off = max - VIRT_HDR;
    /* Full continuation fragments, then a final one that may be empty
     * (libticalcs sends the remainder, even 0 bytes, as VIRT_DATA_LAST). */
    while (rc == DOCK_OK) {
        bool last = n - off < max;
        chunk = last ? n - off : max;
        dock_dusb_build_raw(d->pkt, last ? (uint8_t)RPKT_VIRT_DATA_LAST
                                         : (uint8_t)RPKT_VIRT_DATA,
                            data + off, (size_t)chunk);
        off += chunk;
        if (last) {
            rc = send_last(d, RAW_HDR + (size_t)chunk);
            return rc == DOCK_OK ? recv_ack(d) : rc;
        }
        if (!d->io.send(d->io.user, d->pkt, RAW_HDR + (size_t)chunk))
            return DOCK_E_IO;
        rc = recv_ack(d);
    }
    return rc;
}

/*
 * Receive one virtual packet into d->vbuf, acknowledging every fragment.
 * head[] gets the first payload bytes even when vbuf is too small.
 */
static int recv_vpkt(dock_dusb_t *d, uint32_t timeout_ms, uint16_t *vtype,
                     uint32_t *vsize, uint8_t head[8])
{
    uint32_t got = 0;
    bool first = true, overflow = false;
    uint8_t type;
    size_t len;
    int rc;

    memset(head, 0, 8);
    for (;;) {
        const uint8_t *p;
        size_t n;
        rc = raw_recv(d, first ? timeout_ms : d->timeout_ms, &type, &len);
        if (rc != DOCK_OK)
            return rc;
        if (type == RPKT_BUF_SIZE_REQ) {
            rc = answer_buf_size_req(d, len);
            if (rc != DOCK_OK)
                return rc;
            continue;
        }
        if (type != RPKT_VIRT_DATA && type != RPKT_VIRT_DATA_LAST)
            return DOCK_E_INVALID;
        p = d->pkt + RAW_HDR;
        n = len;
        if (first) {
            if (len < VIRT_HDR)
                return DOCK_E_INVALID;
            *vtype = get_be16(p + 4);
            p += VIRT_HDR;
            n -= VIRT_HDR;
            first = false;
        }
        if (got < 8u) {
            size_t h = 8u - (size_t)got < n ? 8u - (size_t)got : n;
            memcpy(head + got, p, h);
        }
        if (!overflow && (size_t)got + n <= d->vcap)
            memcpy(d->vbuf + got, p, n);
        else
            overflow = true;
        got += (uint32_t)n;
        rc = send_ack(d);
        if (rc != DOCK_OK)
            return rc;
        if (type == RPKT_VIRT_DATA_LAST)
            break;
    }
    /* libticalcs' varsize receive (used for screenshots) trusts the actual
     * byte count, not the declared one; so do we. */
    *vsize = got;
    return overflow ? DOCK_E_TOO_BIG : DOCK_OK;
}

/* Receive the reply to a command, riding out DELAY_ACKs. */
static int recv_reply(dock_dusb_t *d, uint16_t *vtype, uint32_t *vsize)
{
    uint32_t timeout = d->timeout_ms;
    uint8_t head[8];
    uint8_t i;
    for (i = 0; i < MAX_DELAYS; i++) {
        int rc = recv_vpkt(d, timeout, vtype, vsize, head);
        if (rc == DOCK_E_TOO_BIG && *vtype != VPKT_PARM_DATA)
            rc = DOCK_OK; /* small control packets: head[] is enough */
        if (rc != DOCK_OK)
            return rc;
        if (*vtype == VPKT_ERROR)
            return DOCK_E_IO;
        if (*vtype != VPKT_DELAY_ACK)
            return DOCK_OK;
        /* The calculator is busy for `delay` microseconds: rather than
         * sleeping (core/ has no clock), wait that much longer for the next
         * packet. */
        {
            uint32_t delay = *vsize >= 4 ? get_be32(head) : 0;
            if (delay > MAX_DELAY_US)
                delay = MAX_DELAY_US;
            timeout = d->timeout_ms + delay / 1000u + 100u;
        }
    }
    return DOCK_E_TIMEOUT;
}

/* ---- commands ---- */

int dock_dusb_handshake(dock_dusb_t *d)
{
    /* DUSB_MODE_NORMAL = {3, 1, 0, 0, 0x07d0}, sent big-endian. */
    static const uint8_t mode[10] = {0x00, 0x03, 0x00, 0x01, 0x00,
                                     0x00, 0x00, 0x00, 0x07, 0xD0};
    uint8_t type;
    size_t len;
    uint16_t vtype = 0;
    uint32_t vsize = 0, alloc;
    int rc;

    d->raw_size = 250u;
    rc = raw_send_u32(d, RPKT_BUF_SIZE_REQ, DFL_BUF_SIZE);
    if (rc != DOCK_OK)
        return rc;
    rc = raw_recv(d, d->timeout_ms, &type, &len);
    if (rc != DOCK_OK)
        return rc;
    if (type != RPKT_BUF_SIZE_ALLOC || len != 4)
        return DOCK_E_INVALID;
    alloc = clamp_raw(get_be32(d->pkt + RAW_HDR));
    if (alloc <= VIRT_HDR)
        return DOCK_E_INVALID;
    d->raw_size = alloc;

    rc = send_vpkt(d, VPKT_PING, mode, sizeof mode);
    if (rc == DOCK_OK)
        rc = recv_reply(d, &vtype, &vsize);
    if (rc == DOCK_OK && vtype != VPKT_MODE_SET)
        rc = DOCK_E_INVALID;
    return rc;
}

int dock_dusb_screenshot(dock_dusb_t *d, uint8_t *dst, size_t cap)
{
    static const uint8_t req[4] = {0x00, 0x01, 0x00, 0x22}; /* 1 x PID_SCREENSHOT */
    uint16_t vtype = 0;
    uint32_t vsize = 0;
    int rc;

    if (cap < DOCK_DUSB_SCREEN_BYTES)
        return DOCK_E_TOO_BIG;
    rc = send_vpkt(d, VPKT_PARM_REQ, req, sizeof req);
    if (rc == DOCK_OK)
        rc = recv_reply(d, &vtype, &vsize);
    if (rc != DOCK_OK)
        return rc;
    /* {u16 count = 1, u16 id = 0x0022, u8 0 = ok, u16 size, data} */
    if (vtype != VPKT_PARM_DATA || vsize != 7u + DOCK_DUSB_SCREEN_BYTES ||
        get_be16(d->vbuf) != 1u || get_be16(d->vbuf + 2) != PID_SCREENSHOT ||
        d->vbuf[4] != 0)
        return DOCK_E_INVALID;
    memcpy(dst, d->vbuf + 7, DOCK_DUSB_SCREEN_BYTES);
    return DOCK_OK;
}

int dock_dusb_send_key(dock_dusb_t *d, uint16_t key)
{
    uint8_t b[5];
    uint16_t vtype = 0;
    uint32_t vsize = 0;
    int rc;

    b[0] = 0;       /* folder name length */
    b[1] = 0;       /* program name length */
    b[2] = EID_KEY; /* action */
    if ((key >> 8) == 0) {
        b[3] = 0;
        b[4] = (uint8_t)key;
    } else {
        b[3] = (uint8_t)key;        /* keyExtend */
        b[4] = (uint8_t)(key >> 8); /* kbdKey (prefix) */
    }
    rc = send_vpkt(d, VPKT_EXECUTE, b, sizeof b);
    if (rc == DOCK_OK)
        rc = recv_reply(d, &vtype, &vsize);
    if (rc == DOCK_OK && vtype != VPKT_DATA_ACK)
        rc = DOCK_E_INVALID;
    return rc;
}
