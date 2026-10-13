/*
 * dock_dev.h - portable device engine (runs on the calculator).
 *
 * One dock_dev_t holds the whole device-side session: handshake, screen
 * streaming, key queue and the single outstanding request used by the public
 * API (dock_api.h). It never allocates and keeps no hidden state, so on the
 * calculator it can live at a fixed RAM address and be driven from a TI-OS
 * hook, while a simulator can run several instances side by side.
 */
#ifndef DOCK_DEV_H
#define DOCK_DEV_H

#include "dock_common.h"
#include "dock_dev_hal.h"
#include "dock_proto.h"
#include "dock_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOCK_DEV_RXBUF 300u   /* largest non-tail frame the device accepts */
#define DOCK_DEV_SCRATCH 300u /* header + fixed fields of one outgoing frame */
#define DOCK_DEV_KEYQ 16u     /* power of two */
#define DOCK_JOIN_LEN 6u

enum dock_dev_state {
    DOCK_DEV_OFFLINE = 0,  /* no host */
    DOCK_DEV_HELLO,        /* HELLO sent, waiting for WELCOME */
    DOCK_DEV_NEED_JOIN,    /* host wants a (different) join code */
    DOCK_DEV_RUNNING
};

typedef struct dock_key {
    uint16_t tikey;   /* GetKey code (k* in ti/getkey.h), 0 = none */
    uint8_t scancode; /* GetCSC code (sk_*), 0 = none */
    uint8_t flags;    /* DOCK_KEYF_* */
} dock_key_t;

typedef struct dock_hidkey {
    uint8_t usage, mods, down, ascii;
} dock_hidkey_t;

typedef struct dock_dev {
    void *hal;

    /* configuration (may be changed between polls) */
    uint32_t calc_id;
    char join_code[DOCK_JOIN_LEN]; /* all '\0' = none */
    uint8_t hello_flags;
    uint16_t driver_ver;
    uint16_t scan_rows_per_poll;   /* rows hashed per dock_dev_poll() */
    uint16_t band_rows_max;        /* rows per SCR_ROWS frame */
    uint16_t frame_interval_ms;    /* min time between full scan passes */
    uint32_t full_refresh_ms;      /* periodic resend of everything, 0 = off */
    uint32_t timeout_ms;           /* request timeout */
    bool screen_enabled;

    /* session */
    uint8_t state;     /* enum dock_dev_state */
    uint8_t last_welcome_status;
    bool link_was_up;
    uint32_t dock_id;
    uint16_t host_caps;
    uint8_t seq;

    /* receive */
    dock_rx_t rx;
    uint8_t rxbuf[DOCK_DEV_RXBUF];
    uint8_t rxchunk[64];

    /* transmit: one frame = scratch piece + optional tail, sent in chunks */
    uint8_t scratch[DOCK_DEV_SCRATCH];
    const uint8_t *tx_ptr[2];
    size_t tx_len[2];
    uint8_t tx_piece;
    size_t tx_off;
    bool tx_busy;

    /* screen */
    dock_scr_tx_t scr;
    uint32_t pass_start_ms;
    uint32_t last_full_ms;
    bool pass_done;    /* a scan pass finished; bands are being sent */
    bool frame_marker; /* next band closes a pass */
    uint16_t pal_sent_n;

    /* keys from the host */
    dock_key_t keyq[DOCK_DEV_KEYQ];
    uint8_t key_head, key_tail;
    dock_hidkey_t hidq[DOCK_DEV_KEYQ];
    uint8_t hid_head, hid_tail;

    /* the one outstanding request */
    bool req_active;
    bool req_done;
    uint8_t req_seq;
    uint8_t req_type;
    uint8_t req_status;
    uint8_t resp[64];        /* fixed part of the response (after req hdr) */
    size_t resp_len;
    uint8_t *resp_tail;      /* where bulk response data goes */
    size_t resp_tail_cap;
    size_t resp_tail_len;
    uint8_t resp_fixed;      /* bytes of type-specific fixed fields */

    /* statistics */
    uint32_t frames_sent;
    uint32_t bytes_sent;
    uint32_t hellos_sent;
    uint32_t last_hello_ms;
} dock_dev_t;

void dock_dev_init(dock_dev_t *d, void *hal, uint32_t calc_id);

/* Change the join code (6 ASCII digits, or NULL for none) and re-handshake. */
void dock_dev_set_join_code(dock_dev_t *d, const char *code);

/* Do a bounded amount of work: driver service, RX, TX, screen. Call often. */
void dock_dev_poll(dock_dev_t *d);

/* Tell the host we are leaving (best effort, waits up to ~100 ms). */
void dock_dev_bye(dock_dev_t *d);

bool dock_dev_key_pop(dock_dev_t *d, dock_key_t *k);
bool dock_dev_hid_pop(dock_dev_t *d, dock_hidkey_t *k);

/* Rescan the whole screen now and send everything that changed (blocking). */
void dock_dev_screen_push(dock_dev_t *d);
/* Force a resend of mode, palette and every row. */
void dock_dev_screen_refresh(dock_dev_t *d);

/*
 * Blocking request/response used by dock_api.c.
 *   fixed/fixed_len : type-specific request fields (copied)
 *   tail/tail_len   : bulk request data, sent zero-copy
 *   resp_fixed      : number of fixed response bytes expected before bulk data
 *   resp_tail/cap   : where bulk response data goes (may be NULL)
 * On return d->resp / d->resp_len hold the fixed response fields and
 * d->resp_tail_len the bulk byte count. Returns a dock_status.
 */
int dock_dev_request(dock_dev_t *d, uint8_t type, const void *fixed,
                     size_t fixed_len, const void *tail, size_t tail_len,
                     uint8_t resp_fixed, void *resp_tail, size_t resp_tail_cap);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_DEV_H */
