/*
 * dock_proto.h - framing and message catalogue of the dock protocol.
 *
 * Wire format (both directions, over any reliable byte stream: USB bulk,
 * UART, TCP, a pty...):
 *
 *   offset 0  u8   0xD0          magic
 *          1  u8   0xC5          magic
 *          2  u8   type          enum dock_msg_type
 *          3  u8   seq           sender's sequence number / request id
 *          4  u24  len           payload length, little-endian
 *          7  u8   crc8          CRC-8 (poly 0x07) over bytes 0..6
 *          8  ...  payload       len bytes, no trailer
 *
 * The payload has no checksum: USB already CRCs every packet, and leaving it
 * out lets the calculator DMA screen rows straight out of VRAM. The magic plus
 * the header CRC let a receiver resynchronise after joining mid-stream.
 *
 * A sender may emit a frame as several pieces (header+fixed fields from a
 * scratch buffer, then a "tail" straight from user memory). Receivers treat
 * the link as a byte stream, so that is invisible to them.
 *
 * Requests are sent by the calculator; the host answers each one with a single
 * DOCK_T_RESP frame whose payload starts with {req_seq, req_type, status}.
 * See docs/PROTOCOL.md for the per-message field tables (mirrored below).
 */
#ifndef DOCK_PROTO_H
#define DOCK_PROTO_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOCK_MAGIC0 0xD0u
#define DOCK_MAGIC1 0xC5u
#define DOCK_HDR_LEN 8u
#define DOCK_MAX_PAYLOAD 0x40000ul /* 256 KiB, > one full 16bpp frame */

enum dock_msg_type {
    /* ---- session ---- */
    DOCK_T_HELLO = 0x01,      /* calc->host  u8 proto, u8 flags, u32 calc_id,
                                 u16 driver_ver, char join_code[6] ('\0'*6 = none) */
    DOCK_T_WELCOME = 0x02,    /* host->calc  u8 proto, u8 status, u32 dock_id,
                                 u16 caps (DOCK_CAP_*) */
    DOCK_T_PING = 0x03,       /* either way, empty; answered with PONG */
    DOCK_T_PONG = 0x04,
    DOCK_T_BYE = 0x05,        /* calc->host: driver is releasing the port */
    DOCK_T_LOG = 0x06,        /* calc->host: UTF-8 text for the debug log */
    DOCK_T_RESP = 0x07,       /* host->calc: u8 req_seq, u8 req_type, u8 status, ... */

    /* ---- screen (calc->host unless noted) ---- */
    DOCK_T_SCR_MODE = 0x10,   /* u16 width, u16 height, u8 fmt (dock_pix_fmt), u8 flags */
    DOCK_T_SCR_PALETTE = 0x11,/* u8 first, u16 count, count * u16 raw entries */
    DOCK_T_SCR_ROWS = 0x12,   /* u16 y0, u16 count, u16 stride, u8 frame_end, then
                                 count*stride raw row bytes */
    DOCK_T_SCR_REFRESH = 0x13,/* host->calc: resend mode, palette and all rows */

    /* ---- input (host->calc) ---- */
    DOCK_T_KEY = 0x20,        /* u16 tikey (GetKey code, 0 = none), u8 scancode
                                 (GetCSC code, 0 = none), u8 flags (DOCK_KEYF_*) */
    DOCK_T_HIDKEY = 0x21,     /* u8 hid_usage, u8 modifiers, u8 down, u8 ascii */

    /* ---- audio (calc->host requests) ---- */
    DOCK_T_AUD_OPEN = 0x30,   /* u32 rate, u8 fmt (DOCK_AUD_*), u8 channels */
    DOCK_T_AUD_WRITE = 0x31,  /* raw PCM bytes -> RESP u24 accepted */
    DOCK_T_AUD_PLAY_FILE = 0x32, /* path\0 (WAV, PCM only) */
    DOCK_T_AUD_POS = 0x33,    /* -> RESP u32 pos_ms, u8 playing */
    DOCK_T_AUD_STOP = 0x34,
    DOCK_T_AUD_VOLUME = 0x35, /* u8 percent */

    /* ---- files on the dock's USB drives ---- */
    DOCK_T_FS_DRIVES = 0x40,  /* u8 index -> RESP name\0 | E_END */
    DOCK_T_FS_OPEN = 0x41,    /* u8 mode (DOCK_FS_*), path\0 -> RESP u8 handle */
    DOCK_T_FS_READ = 0x42,    /* u8 handle, u24 max -> RESP data (len = payload-3) */
    DOCK_T_FS_WRITE = 0x43,   /* u8 handle, data -> RESP u24 written */
    DOCK_T_FS_SEEK = 0x44,    /* u8 handle, u8 whence, u32 offset (signed) -> RESP u32 pos */
    DOCK_T_FS_CLOSE = 0x45,   /* u8 handle */
    DOCK_T_FS_STAT = 0x46,    /* path\0 -> RESP u32 size, u8 is_dir */
    DOCK_T_FS_READDIR = 0x47, /* u16 index, path\0 -> RESP u32 size, u8 is_dir, name\0 */
    DOCK_T_FS_MKDIR = 0x48,   /* path\0 */
    DOCK_T_FS_REMOVE = 0x49,  /* path\0 */

    /* ---- generic USB devices plugged into the dock ---- */
    DOCK_T_USB_LIST = 0x50,   /* u8 index -> RESP u16 vid, u16 pid, u8 class, name\0 */

    /* ---- network (reserved, implemented in a later milestone) ---- */
    DOCK_T_NET_STATUS = 0x60, /* -> RESP u8 up, ssid\0, ip\0 */
    DOCK_T_NET_HTTP_BEGIN = 0x61, /* u8 method (DOCK_HTTP_*), u32 body_len,
                                     url\0, headers\0 -> RESP u8 handle */
    DOCK_T_NET_HTTP_BODY = 0x62,  /* u8 handle, data */
    DOCK_T_NET_HTTP_RESP = 0x63,  /* u8 handle -> RESP u16 http_status, u32 content_len
                                     (0xFFFFFFFF = unknown) */
    DOCK_T_NET_HTTP_READ = 0x64,  /* u8 handle, u24 max -> RESP data, empty = EOF */
    DOCK_T_NET_HTTP_END = 0x65    /* u8 handle */
};

/* HELLO flags */
#define DOCK_HELLOF_RESIDENT 0x01u /* driver lives inside TI-OS (hooks) */

/* WELCOME caps */
#define DOCK_CAP_AUDIO 0x0001u
#define DOCK_CAP_FS 0x0002u
#define DOCK_CAP_USB 0x0004u
#define DOCK_CAP_NET 0x0008u
#define DOCK_CAP_KEYS 0x0010u

/* KEY flags */
#define DOCK_KEYF_ON 0x01u /* press the ON key (break) */

/* Audio sample formats */
#define DOCK_AUD_U8 1u
#define DOCK_AUD_S16LE 2u

/* FS open modes (bit set) */
#define DOCK_FS_READ 0x01u
#define DOCK_FS_WRITE 0x02u
#define DOCK_FS_CREATE 0x04u /* create if missing */
#define DOCK_FS_TRUNC 0x08u
#define DOCK_FS_APPEND 0x10u

#define DOCK_SEEK_SET 0u
#define DOCK_SEEK_CUR 1u
#define DOCK_SEEK_END 2u

#define DOCK_HTTP_GET 1u
#define DOCK_HTTP_PUT 2u

/* Bytes before the type-specific fields of a RESP payload. */
#define DOCK_RESP_HDR_LEN 3u

/* ---------------------------------------------------------------------- */

uint8_t dock_crc8(const uint8_t *p, size_t n);

/* Fill an 8-byte frame header. */
void dock_hdr_build(uint8_t hdr[DOCK_HDR_LEN], uint8_t type, uint8_t seq,
                    uint32_t len);

/* ---- tiny payload writer / reader (bounds-checked) ---- */
typedef struct dock_wr {
    uint8_t *p;
    size_t cap;
    size_t len;
    bool overflow;
} dock_wr_t;

void dock_wr_init(dock_wr_t *w, uint8_t *buf, size_t cap);
void dock_wr_u8(dock_wr_t *w, uint8_t v);
void dock_wr_u16(dock_wr_t *w, uint16_t v);
void dock_wr_u24(dock_wr_t *w, uint32_t v);
void dock_wr_u32(dock_wr_t *w, uint32_t v);
void dock_wr_bytes(dock_wr_t *w, const void *src, size_t n);
void dock_wr_str(dock_wr_t *w, const char *s); /* includes the '\0' */

typedef struct dock_rd {
    const uint8_t *p;
    size_t len;
    size_t pos;
    bool underflow;
} dock_rd_t;

void dock_rd_init(dock_rd_t *r, const uint8_t *buf, size_t len);
uint8_t dock_rd_u8(dock_rd_t *r);
uint16_t dock_rd_u16(dock_rd_t *r);
uint32_t dock_rd_u24(dock_rd_t *r);
uint32_t dock_rd_u32(dock_rd_t *r);
/* Returns a pointer to a NUL-terminated string inside the buffer, or "" (and
 * sets underflow) if no terminator is found. */
const char *dock_rd_str(dock_rd_t *r);
size_t dock_rd_left(const dock_rd_t *r);
const uint8_t *dock_rd_ptr(const dock_rd_t *r);

/* ---- incremental stream parser ---- */

typedef struct dock_frame {
    uint8_t type;
    uint8_t seq;
    uint32_t len;
} dock_frame_t;

/*
 * Split sink: when a header arrives, the parser asks where its payload goes.
 * The first `fixed_len` bytes always land in the parser's own buffer; the rest
 * go to `tail` (up to tail_cap, excess counted and dropped). Returning
 * tail == NULL means "put the whole payload in the parser buffer" (dropped if
 * it does not fit; frame.len is still reported so the caller can tell).
 */
typedef struct dock_sink {
    size_t fixed_len;
    uint8_t *tail;
    size_t tail_cap;
} dock_sink_t;

typedef void (*dock_sink_fn)(void *user, const dock_frame_t *f, dock_sink_t *out);

/*
 * Called when a full frame has arrived. `buf`/`buf_len` is what was stored in
 * the parser buffer (fixed part, or the whole payload); `tail_len` is how many
 * bytes went to the tail sink. `truncated` is true if anything was dropped.
 */
typedef void (*dock_frame_fn)(void *user, const dock_frame_t *f,
                              const uint8_t *buf, size_t buf_len,
                              size_t tail_len, bool truncated);

typedef struct dock_rx {
    uint8_t *buf;
    size_t cap;
    dock_sink_fn on_sink; /* may be NULL */
    dock_frame_fn on_frame;
    void *user;

    /* private */
    uint8_t hdr[DOCK_HDR_LEN];
    uint8_t hdr_have;
    dock_frame_t cur;
    dock_sink_t sink;
    uint32_t got;      /* payload bytes consumed */
    size_t buf_have;
    size_t tail_have;
    bool truncated;
    bool in_payload;
    uint32_t resyncs;  /* statistics: bytes skipped hunting for magic */
} dock_rx_t;

void dock_rx_init(dock_rx_t *rx, uint8_t *buf, size_t cap, dock_sink_fn on_sink,
                  dock_frame_fn on_frame, void *user);
void dock_rx_reset(dock_rx_t *rx);
/* Feed any number of bytes; callbacks fire synchronously. */
void dock_rx_feed(dock_rx_t *rx, const uint8_t *data, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_PROTO_H */
