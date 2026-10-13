/*
 * dusb.h - minimal TI "DUSB" link protocol (the one TI Connect CE speaks), host
 * side. Used as the fallback path when the DOCK driver is not running: it can
 * take screenshots and send keypresses to a stock TI-84 Plus CE.
 *
 * Transport is abstracted as "send one raw packet" / "receive one raw packet"
 * (one USB bulk transfer each on the real link), so the codec is portable and
 * testable against canned byte vectors.
 *
 * Reference: libticalcs2 (tilibs) dusb_rpkt.cc / dusb_vpkt.cc / dusb_cmd.cc.
 */
#ifndef DOCK_DUSB_H
#define DOCK_DUSB_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOCK_DUSB_VID 0x0451u
#define DOCK_DUSB_PID_84PCE 0xE008u /* TI-84 Plus CE / 83 Premium CE */
#define DOCK_DUSB_SCREEN_W 320u
#define DOCK_DUSB_SCREEN_H 240u
#define DOCK_DUSB_SCREEN_BYTES (320u * 240u * 2u) /* RGB565 little-endian */
#define DOCK_DUSB_RAW_MAX 1023u                    /* max raw packet we offer */

typedef struct dock_dusb_io {
    void *user;
    /* Send one raw packet (header + data). Return false on error. */
    bool (*send)(void *user, const uint8_t *p, size_t n);
    /* Receive one raw packet into p (cap bytes). Return its length, 0 on
     * timeout/error. */
    size_t (*recv)(void *user, uint8_t *p, size_t cap, uint32_t timeout_ms);
} dock_dusb_io_t;

typedef struct dock_dusb {
    dock_dusb_io_t io;
    uint32_t raw_size;        /* negotiated max raw packet size */
    uint32_t timeout_ms;
    uint8_t *vbuf;            /* virtual-packet reassembly buffer */
    size_t vcap;
    uint8_t pkt[DOCK_DUSB_RAW_MAX + 8];
} dock_dusb_t;

/* vbuf must hold the biggest virtual packet you will receive: a screenshot
 * needs DOCK_DUSB_SCREEN_BYTES + 64. */
void dock_dusb_init(dock_dusb_t *d, const dock_dusb_io_t *io, uint8_t *vbuf,
                    size_t vcap);
/* Buffer-size negotiation + mode set. Call once after the device appears. */
int dock_dusb_handshake(dock_dusb_t *d);
/* Fetch the screen as 320x240 RGB565 LE into dst (DOCK_DUSB_SCREEN_BYTES). */
int dock_dusb_screenshot(dock_dusb_t *d, uint8_t *dst, size_t cap);
/* Send one keypress (TI GetKey code; 2-byte keys as 0xXXYY). */
int dock_dusb_send_key(dock_dusb_t *d, uint16_t key);

/* ---- exposed for unit tests ---- */
size_t dock_dusb_build_raw(uint8_t *out, uint8_t type, const uint8_t *data,
                           size_t n);
size_t dock_dusb_build_virtual_header(uint8_t *out, uint32_t size,
                                      uint16_t vtype);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_DUSB_H */
