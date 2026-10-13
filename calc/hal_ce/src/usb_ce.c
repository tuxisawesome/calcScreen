/*
 * usb_ce.c - minimal polled USB *device* driver for the TI-84 Plus CE's
 * FOTG210-style controller (base 0xE20000), plus the dock_dev_hal.h back end.
 *
 * Role: the calculator is a full-speed USB DEVICE that a Raspberry Pi host
 * talks to. EP0 control, bulk EP1 IN (calc->host) and bulk EP2 OUT
 * (host->calc), 64-byte max packet. We do NOT use usbdrvce; this is an
 * independent re-implementation of its proven [HW] register sequences,
 * following /tmp/claude-0/ext/FOTG210_DEVICE_SPEC.md ("SPEC" below). All
 * register access is single-byte (mmio_ce.h, SPEC gotcha 18).
 *
 * INTERRUPT POLICY (documented once here):
 *   The CE C runtime (crt0.S) already runs main() under `di` and calls
 *   usb_DisableTimer. We KEEP interrupts disabled for the entire USB session -
 *   the simplest correct option per SPEC 2.1 step A ("stay di for the whole
 *   session, which is simplest"). Consequences:
 *     - No TI-OS timer-3 / USB ISR can run and corrupt our state.
 *     - Status bits latch regardless of masks (SPEC 3.2), so pure polling works.
 *     - The millisecond clock (time_ce, GPT timer 2) and clock() (GPT timer 1)
 *       are free-running hardware counters, so they keep working under `di`.
 *   We still mask intTmr3+intUsb in mpIntMask (saving the old value) so the
 *   hand-back can restore them, and only `ei` at the very end of cleanup.
 *
 * DMA: one shared engine (SPEC section 5). We drive it synchronously - start a
 * transfer, poll to completion with a bounded timeout, deselect - so EP0, bulk
 * IN and bulk OUT never collide. DMA can only reach RAM incl. VRAM
 * (0xD00000-0xD657FF); descriptors are program consts, which live in RAM on the
 * CE, so they are DMA-readable.
 */
#include "hal_ce.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <intce.h> /* int_Disable/int_Enable */
#include <ti/info.h>

#include "dock_common.h"
#include "dock_dev_hal.h"
#include "dock_screen.h"
#include "lcd_ce.h"
#include "mmio_ce.h"
#include "time_ce.h"

/* ---- register addresses (absolute, base 0xE20000; SPEC section 1) ---- */
#define R_USBCMD      0xE20010u
#define R_USBSTS_B1   0xE20015u /* byte 1: b4 = HcHalted */
#define R_USBINTEN    0xE20018u
#define R_OTGCSR_B0   0xE20080u
#define R_OTGCSR_B2   0xE20082u /* b1 B_SESS_VLD, b4 Role(bit20), b5 Id(bit21) */
#define R_OTGISR_B0   0xE20084u /* b6 BSessEnd */
#define R_OTGISR_B1   0xE20085u /* b0 RoleChg, b1 IdChg, b3 BPlug, b4 APlug */
#define R_OTGIER_B0   0xE20088u
#define R_OTGIER_B1   0xE20089u
#define R_USBIMR      0xE200C4u
#define R_DEVCTRL     0xE20100u
#define R_DEVCTRL_B1  0xE20101u /* b1 (bit9) ForceFullSpd */
#define R_DEVADDR     0xE20104u /* b0-6 addr, b7 DevConf */
#define R_DEVTEST     0xE20108u /* b0 TstClrFifo */
#define R_PHYTMSR     0xE20114u /* b0 Unplug (soft-detach) */
#define R_CXFIFO      0xE20120u /* w: b0 Fin, b2 Stall, b3 Clr; r: b4 Full,b5 Empty */
#define R_CXFIFO_B1   0xE20121u /* r: b(n) = FIFOn empty */
#define R_CXBYTES     0xE20123u
#define R_IDLE        0xE20124u
#define R_GIMR        0xE20130u
#define R_CXIMR       0xE20134u
#define R_RXIMR       0xE20138u
#define R_TXIMR       0xE2013Au
#define R_DEVIMR      0xE2013Cu
#define R_DEVIMR_B1   0xE2013Du
#define R_CXISR       0xE20144u /* b0 Setup,b1 In,b2 Out,b3 End,b4 Err,b5 Abort */
#define R_RXISR       0xE20148u /* bit 2f FIFOf OUT, 2f+1 SPK */
#define R_TXISR       0xE2014Au /* b0-3 FIFOf IN ready (level-like) */
#define R_DEVISR      0xE2014Cu /* b0 Reset,b1 Susp,b2 Resume,b5 ZlpTx,b6 ZlpRx,b7 DmaFin */
#define R_DEVISR_B1   0xE2014Du /* b0 DmaErr,b1 Idle,b2 Wakeup */
#define R_RXZLP       0xE20150u
#define R_TXZLP       0xE20154u
#define R_INEP1       0xE20160u /* +0 MPS[7:0]; +1 b3 Stall,b4 Reset,b7 SendZlp */
#define R_INEP1_B1    0xE20161u
#define R_OUTEP2      0xE20184u
#define R_OUTEP2_B1   0xE20185u
#define R_EPMAP       0xE201A0u /* +0..: EP1..4 -> FIFO (b0-3 IN, b4-7 OUT) */
#define R_FIFOMAP     0xE201A8u /* +0..3: FIFO0..3 -> EP (b0-3 EP, b4-5 dir) */
#define R_FIFOCFG     0xE201ACu /* +0..3: b0-1 type, b5 FifoEn */
#define R_FIFO1CSR    0xE201B4u /* FIFO1 (EP2 OUT) byte count b0-10 */
#define R_DMAFIFO     0xE201C0u /* b0-3 FIFOn, b4 CX, 0 none */
#define R_DMACTRL     0xE201C8u /* b0 Start,b1 Dir(1=mem->fifo),b3 Abort,b4 ClrFifo */
#define R_DMACTRL_B3  0xE201CBu /* len[16] in b0; b7 = register-disable (keep 0) */
#define R_DMALEN      0xE201C9u /* 17-bit length C9/CA/CB */
#define R_DMAADDR     0xE201CCu /* 19-bit RAM offset CC/CD/CE */
#define R_EP0DATA     0xE201D0u /* 8-byte SETUP read port */

/* ---- OS hand-off RAM (SPEC 1.4) ---- */
#define R_MPIMASK     0xF00004u /* mpIntMask: byte+0 b3 = intTmr3, byte+1 b5 = intUsb */
#define USB_AREA      0xD13FD8u
#define USB_HANDLEKEYS 0xD14091u
#define USB_INITED    0xD177B7u
#define OS_FLAGS_1B   0xD0009Bu /* flags+0x1B, bit5 mirrored by usbdrvce */
#define ROUTINE_USB_DISABLETIMER 0x0004F4u

/* ---- tunables ---- */
#define MPS 64u
#define RX_RING 1024u /* must hold several bulk OUT packets between polls */
#define DMA_TIMEOUT_MS 100u /* bounded wait for one DMA (SPEC gotcha 7) */

/* USB standard requests / descriptor types */
#define REQ_GET_STATUS        0x00
#define REQ_CLEAR_FEATURE     0x01
#define REQ_SET_FEATURE       0x03
#define REQ_SET_ADDRESS       0x05
#define REQ_GET_DESCRIPTOR    0x06
#define REQ_GET_CONFIGURATION 0x08
#define REQ_SET_CONFIGURATION 0x09
#define DESC_DEVICE        1
#define DESC_CONFIGURATION 2
#define DESC_STRING        3
#define FEAT_ENDPOINT_HALT 0

/* ------------------------------------------------------------------ */
/* driver context                                                      */

typedef struct {
    bool configured;     /* SET_CONFIGURATION(nonzero) done -> link up */
    bool unplug_set;     /* soft-disconnect asserted, awaiting VBUS to reconnect */
    uint8_t config_value;
    uint8_t device_status; /* 1 = self-powered (SPEC 4.6) */
    uint32_t calc_id;
    uint8_t saved_mpimask[3]; /* mpIntMask bytes to restore at hand-back */

    /* EP0 response scratch (descriptors/status; all <= 64 bytes) */
    uint8_t ep0buf[64];

    /* TX (EP1 IN): one sub-transfer in flight (dock_dev's "one in flight"). */
    const uint8_t *tx_buf;
    size_t tx_len;
    size_t tx_sent;
    bool tx_active;
    bool tx_need_zlp;    /* terminate with a ZLP when all bytes have gone */
    bool tx_zlp_armed;   /* SendZlp set, waiting for DevIsr.ZlpTx */

    /* RX (EP2 OUT) byte ring, filled by DMA in service() */
    uint8_t rx_ring[RX_RING];
    uint16_t rx_head, rx_tail; /* head=write, tail=read */
    uint16_t rx_count;
    uint8_t rx_pkt[MPS];       /* DMA landing pad (one packet) */
} ctx_t;

static ctx_t g_ctx;

/* RxZlp near-atomic read-and-clear (dma_ce.S). */
extern void halce_rxzlp_clear(void);

/* ------------------------------------------------------------------ */
/* descriptors (program consts => in RAM on the CE => DMA-readable)    */

static const uint8_t k_device_desc[18] = {
    18, DESC_DEVICE,
    0x00, 0x02,                 /* bcdUSB 2.00 (enumerates at full speed) */
    0x00, 0x00, 0x00,           /* class/subclass/protocol: per-interface */
    (uint8_t)MPS,               /* bMaxPacketSize0 = 64 */
    (uint8_t)DOCK_USB_VID, (uint8_t)(DOCK_USB_VID >> 8),
    (uint8_t)DOCK_USB_PID, (uint8_t)(DOCK_USB_PID >> 8),
    0x01, 0x00,                 /* bcdDevice 0.01 */
    0x00,                       /* iManufacturer: none */
    0x01,                       /* iProduct = string 1 */
    0x02,                       /* iSerialNumber = string 2 */
    0x01                        /* bNumConfigurations */
};

/*
 * Configuration descriptor, total 32 bytes. SPEC gotcha 14 warns that an EP0 IN
 * response whose length is an exact multiple of 64 would need a ZLP we do not
 * generate; 32 (and every other descriptor length here: 18 device, 4/22/18
 * strings, 9 config-header) is not a multiple of 64, so no EP0 ZLP is ever
 * required and no padding descriptor is needed.
 */
static const uint8_t k_config_desc[32] = {
    /* configuration (9) */
    9, DESC_CONFIGURATION,
    32, 0x00,                   /* wTotalLength = 32 */
    0x01,                       /* bNumInterfaces */
    0x01,                       /* bConfigurationValue = 1 */
    0x00,                       /* iConfiguration */
    0x80,                       /* bmAttributes: bus powered */
    0xFA,                       /* bMaxPower = 500 mA */
    /* interface (9) */
    9, 0x04,
    0x00,                       /* bInterfaceNumber */
    0x00,                       /* bAlternateSetting */
    0x02,                       /* bNumEndpoints */
    DOCK_USB_IFACE_CLASS,       /* 0xFF vendor specific */
    DOCK_USB_IFACE_SUBCLASS,    /* 0xD0 */
    DOCK_USB_IFACE_PROTOCOL,    /* 0xC5 */
    0x00,                       /* iInterface */
    /* EP1 IN bulk (7) */
    7, 0x05,
    DOCK_USB_EP_IN,             /* 0x81 */
    0x02,                       /* bulk */
    (uint8_t)MPS, 0x00,         /* wMaxPacketSize = 64 */
    0x00,                       /* bInterval */
    /* EP2 OUT bulk (7) */
    7, 0x05,
    DOCK_USB_EP_OUT,            /* 0x02 */
    0x02,                       /* bulk */
    (uint8_t)MPS, 0x00,
    0x00
};

/* String 0: supported language = US English (0x0409). */
static const uint8_t k_string0[4] = {4, DESC_STRING, 0x09, 0x04};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static void ep0_stall(void)
{
    /* SPEC 4.4: CxFifoStall then ack Setup. */
    wr8(R_CXFIFO, 0x04);
    wr8(R_CXISR, 0x01);
}

static void ep0_ack(void)
{
    /* SPEC 4.3: FIN (ends data+status stage) then ack Setup. */
    wr8(R_CXFIFO, 0x01);
    wr8(R_CXISR, 0x01);
}

/* Bounded wait for a DMA to finish. Returns false on DmaErr, bus reset or
 * timeout, having aborted the engine (SPEC gotcha 7 / 5. "on error"). */
static bool dma_wait(void)
{
    uint32_t t0 = time_ce_ms();
    for (;;) {
        uint8_t dl = rd8(R_DEVISR);
        if (dl & 0x80)        /* DmaFin */
            return true;
        if (dl & 0x01)        /* bus Reset: let the reset handler take over */
            break;
        if (rd8(R_DEVISR_B1) & 0x01) { /* DmaErr */
            wr8(R_DEVISR_B1, 0x01);    /* W1C DmaErr */
            break;
        }
        if ((uint32_t)(time_ce_ms() - t0) >= DMA_TIMEOUT_MS)
            break;
    }
    wr8(R_DMACTRL, 0x18); /* Abort | ClrFifo */
    wr8(R_DMAFIFO, 0x00);
    return false;
}

/* Run one DMA transfer (SPEC section 5 generic recipe). fifo_sel is the
 * 0xE201C0 selector (0x10 for CX, 1<<f for a FIFO). dir is 0x12 (mem->FIFO, IN)
 * or 0x10 (FIFO->mem, OUT), both with ClrFifo. Returns dma_wait()'s result. */
static bool dma_run(uint8_t fifo_sel, uint8_t dir, unsigned addr, unsigned len)
{
    wr8(R_DMAFIFO, fifo_sel);
    wr24(R_DMALEN, len);  /* C9/CA/CB; CB=0 keeps len16 and disable-bit clear */
    wr24(R_DMAADDR, addr);
    wr8(R_DMACTRL, dir);
    wr8(R_DMACTRL, (uint8_t)(dir | 0x01)); /* Start */
    if (!dma_wait())
        return false;
    wr8(R_DEVISR, 0x80);  /* W1C DmaFin */
    wr8(R_DMAFIFO, 0x00); /* deselect (SPEC gotcha 5) */
    return true;
}

/* ------------------------------------------------------------------ */
/* EP0 control transfers                                               */

/* Send an IN data stage from ep0buf by a single CX DMA (SPEC 4.2), then FIN. */
static void ep0_send(size_t len)
{
    wr8(R_DMAFIFO, 0x10); /* CX target */
    wr24(R_DMALEN, (unsigned)len);
    wr24(R_DMAADDR, (unsigned)(uintptr_t)g_ctx.ep0buf);
    wr8(R_DMACTRL, 0x02); /* Dir = mem->FIFO (SPEC 4.2 step 3) */
    wr8(R_DMACTRL, 0x03); /* Start */
    if (!dma_wait()) {
        /* reset/err/timeout: drop it; the host will retry or reset */
        wr8(R_DMAFIFO, 0x00);
        return;
    }
    wr8(R_DEVISR, 0x80);
    wr8(R_DMAFIFO, 0x00);
    ep0_ack(); /* FIN + ack Setup (SPEC 4.2 step 8) */
}

/* Build a string descriptor into ep0buf; returns its length. idx 1 = product,
 * idx 2 = serial (8 hex digits of calc_id). idx 0 handled by caller. */
static size_t build_string(uint8_t idx)
{
    static const char product[] = "calcScreen";
    uint8_t *b = g_ctx.ep0buf;
    size_t i, n;

    if (idx == 1) {
        n = sizeof product - 1u; /* 10 */
        b[0] = (uint8_t)(2u + n * 2u);
        b[1] = DESC_STRING;
        for (i = 0; i < n; i++) {
            b[2 + i * 2u] = (uint8_t)product[i];
            b[3 + i * 2u] = 0;
        }
        return b[0];
    }
    if (idx == 2) {
        static const char hexd[] = "0123456789ABCDEF";
        uint32_t v = g_ctx.calc_id;
        b[0] = (uint8_t)(2u + 8u * 2u); /* 18 */
        b[1] = DESC_STRING;
        for (i = 0; i < 8u; i++) {
            b[2 + i * 2u] = (uint8_t)hexd[(v >> ((7u - i) * 4u)) & 0xFu];
            b[3 + i * 2u] = 0;
        }
        return b[0];
    }
    return 0;
}

/* Configure EP1 IN (FIFO0) and EP2 OUT (FIFO1) for bulk (SPEC 4.7 step 2). */
static void configure_endpoints(void)
{
    /* Pulse DevTest.TstClrFifo to clear all FIFOs. */
    wr8(R_DEVTEST, (uint8_t)(rd8(R_DEVTEST) | 0x01));
    wr8(R_DEVTEST, (uint8_t)(rd8(R_DEVTEST) & ~0x01));

    /* EP1 IN -> FIFO0: direction IN, bulk+enable, MPS 64, toggle Reset. */
    wr8(R_FIFOMAP + 0, (uint8_t)((rd8(R_FIFOMAP + 0) & ~0x30) | 0x10));
    wr8(R_FIFOCFG + 0, 0x22);
    wr8(R_INEP1_B1, 0x10); /* set data-toggle Reset */
    wr8(R_INEP1_B1, 0x00); /* clear it again (SPEC gotcha 10) */
    wr8(R_INEP1, (uint8_t)MPS);

    /* EP2 OUT -> FIFO1: direction OUT, bulk+enable, MPS 64, toggle Reset. */
    wr8(R_FIFOMAP + 1, (uint8_t)(rd8(R_FIFOMAP + 1) & ~0x30));
    wr8(R_FIFOCFG + 1, 0x22);
    wr8(R_OUTEP2_B1, 0x10);
    wr8(R_OUTEP2_B1, 0x00);
    wr8(R_OUTEP2, (uint8_t)MPS);
}

/* Endpoint control-register byte-1 address for a bEndpointAddress (SPEC 4.6).
 * Returns 0 for an unknown endpoint. */
static unsigned ep_byte1(uint8_t ep_addr)
{
    if (ep_addr == DOCK_USB_EP_IN)  /* 0x81 -> IN EP1 */
        return R_INEP1_B1;
    if (ep_addr == DOCK_USB_EP_OUT) /* 0x02 -> OUT EP2 */
        return R_OUTEP2_B1;
    return 0;
}

static void handle_setup(void)
{
    uint8_t s[8];
    uint8_t bmType, bReq, descType, descIdx, recip;
    uint16_t wValue, wIndex, wLength;
    size_t len;

    /* Read the 8-byte SETUP packet in usbdrvce's byte order (SPEC 4.1 step 3).
     * We are fully `di`, so no extra guard is needed around the data port. */
    wr8(R_DMAFIFO, 0x10); /* select CX so the data port pops */
    s[3] = rd8(R_EP0DATA + 3); s[7] = rd8(R_EP0DATA + 3);
    s[2] = rd8(R_EP0DATA + 2); s[6] = rd8(R_EP0DATA + 2);
    s[1] = rd8(R_EP0DATA + 1); s[5] = rd8(R_EP0DATA + 1);
    s[0] = rd8(R_EP0DATA + 0); s[4] = rd8(R_EP0DATA + 0);
    wr8(R_DMAFIFO, 0x00);
    wr8(R_CXFIFO, 0x08); /* CX FIFO Clr right after the fetch (SPEC 4.1 step 5) */

    bmType = s[0];
    bReq = s[1];
    wValue = (uint16_t)(s[2] | (s[3] << 8));
    wIndex = (uint16_t)(s[4] | (s[5] << 8));
    wLength = (uint16_t)(s[6] | (s[7] << 8));
    recip = (uint8_t)(bmType & 0x1Fu);

    /* Only standard requests (type field 0) are supported; stall class/vendor. */
    if ((bmType & 0x60u) != 0x00u) {
        ep0_stall();
        return;
    }

    switch (bReq) {
    case REQ_GET_DESCRIPTOR:
        descType = (uint8_t)(wValue >> 8);
        descIdx = (uint8_t)(wValue & 0xFF);
        if (descType == DESC_DEVICE) {
            memcpy(g_ctx.ep0buf, k_device_desc, sizeof k_device_desc);
            len = sizeof k_device_desc;
        } else if (descType == DESC_CONFIGURATION) {
            memcpy(g_ctx.ep0buf, k_config_desc, sizeof k_config_desc);
            len = sizeof k_config_desc;
        } else if (descType == DESC_STRING && descIdx == 0) {
            memcpy(g_ctx.ep0buf, k_string0, sizeof k_string0);
            len = sizeof k_string0;
        } else if (descType == DESC_STRING && (descIdx == 1 || descIdx == 2)) {
            len = build_string(descIdx);
        } else {
            /* DEVICE_QUALIFIER (6) and everything else: stall. A full-speed
             * only device must stall DEVICE_QUALIFIER (SPEC 4.4 / gotcha 17). */
            ep0_stall();
            return;
        }
        if (len > wLength)
            len = wLength;
        ep0_send(len);
        return;

    case REQ_SET_ADDRESS:
        if ((bmType != 0x00) || (wValue > 127u) || wIndex || wLength) {
            ep0_stall();
            return;
        }
        /* Write immediately; the controller applies it after the status stage
         * (SPEC 4.5). Also clears DevConf, which is fine pre-configuration. */
        wr8(R_DEVADDR, (uint8_t)(wValue & 0x7Fu));
        g_ctx.configured = false;
        g_ctx.config_value = 0;
        ep0_ack();
        return;

    case REQ_SET_CONFIGURATION:
        if ((bmType != 0x00) || wIndex || wLength || (wValue > 1u)) {
            ep0_stall();
            return;
        }
        if (wValue == 1u) {
            configure_endpoints();          /* SPEC 4.7 step 2 */
            wr8(R_DEVADDR, (uint8_t)(rd8(R_DEVADDR) | 0x80)); /* DevConf = 1 */
            g_ctx.configured = true;
        } else {
            wr8(R_DEVADDR, (uint8_t)(rd8(R_DEVADDR) & ~0x80));
            g_ctx.configured = false;
        }
        g_ctx.config_value = (uint8_t)wValue;
        /* A fresh configuration starts with empty pipes. */
        g_ctx.tx_active = false;
        g_ctx.tx_zlp_armed = false;
        g_ctx.rx_head = g_ctx.rx_tail = g_ctx.rx_count = 0;
        ep0_ack();
        return;

    case REQ_GET_CONFIGURATION:
        if (bmType != 0x80) {
            ep0_stall();
            return;
        }
        g_ctx.ep0buf[0] = g_ctx.config_value;
        ep0_send(1);
        return;

    case REQ_GET_STATUS: {
        uint8_t st0 = 0, st1 = 0;
        if (bmType == 0x80) {           /* device */
            st0 = g_ctx.device_status;  /* self-powered */
        } else if (bmType == 0x81) {    /* interface */
            st0 = 0;
        } else if (bmType == 0x82) {    /* endpoint: report halt */
            unsigned b1 = ep_byte1((uint8_t)wIndex);
            if (!b1) {
                ep0_stall();
                return;
            }
            st0 = (uint8_t)((rd8(b1) & 0x08u) ? 1u : 0u);
        } else {
            ep0_stall();
            return;
        }
        g_ctx.ep0buf[0] = st0;
        g_ctx.ep0buf[1] = st1;
        ep0_send(2);
        return;
    }

    case REQ_CLEAR_FEATURE:
    case REQ_SET_FEATURE:
        if (recip == 0x02 && wValue == FEAT_ENDPOINT_HALT) { /* endpoint halt */
            unsigned b1 = ep_byte1((uint8_t)wIndex);
            if (!b1) {
                ep0_stall();
                return;
            }
            if (bReq == REQ_SET_FEATURE) {
                /* set Stall, clear data-toggle Reset (SPEC 4.6). */
                wr8(b1, (uint8_t)((rd8(b1) & ~0x10u) | 0x08u));
            } else {
                /* clear-halt: set Reset then clear Reset+Stall (SPEC 4.6
                 * recommended set-then-clear, gotcha 10). */
                wr8(b1, (uint8_t)(rd8(b1) | 0x10u));
                wr8(b1, (uint8_t)(rd8(b1) & ~0x18u));
            }
            ep0_ack();
            return;
        }
        if (recip == 0x00 && wValue == 1u) { /* DEVICE_REMOTE_WAKEUP: accept */
            ep0_ack();
            return;
        }
        ep0_stall();
        return;

    default:
        /* SET_DESCRIPTOR, GET/SET_INTERFACE, SYNCH_FRAME, ... : stall. */
        ep0_stall();
        return;
    }
}

/* ------------------------------------------------------------------ */
/* bulk endpoints                                                      */

static void bulk_in_pump(void)
{
    uint8_t guard = 8;
    while (guard--) {
        if (!g_ctx.tx_active)
            return;
        if (g_ctx.tx_sent < g_ctx.tx_len) {
            size_t rem, pkt;
            if (!(rd8(R_TXISR) & 0x01)) /* FIFO0 not empty yet (SPEC 5.1) */
                return;
            rem = g_ctx.tx_len - g_ctx.tx_sent;
            pkt = rem < MPS ? rem : MPS;
            if (!dma_run(0x01, 0x12,
                         (unsigned)(uintptr_t)(g_ctx.tx_buf + g_ctx.tx_sent),
                         (unsigned)pkt)) {
                g_ctx.tx_active = false; /* DMA failed: drop the transfer */
                return;
            }
            wr8(R_TXISR, 0x01); /* W1C FIFO0 IN; reasserts when packet drains */
            g_ctx.tx_sent += pkt;
            continue;
        }
        /* all data bytes queued */
        if (g_ctx.tx_need_zlp) {
            if (g_ctx.tx_zlp_armed)
                return; /* waiting for DevIsr.ZlpTx (handled in service) */
            if (!(rd8(R_TXISR) & 0x01))
                return; /* wait until the last data packet has drained */
            /* SPEC 5.1: terminate with a ZLP via InEp1.SendZlp, not a 0-DMA. */
            wr8(R_INEP1_B1, (uint8_t)(rd8(R_INEP1_B1) | 0x80u));
            g_ctx.tx_zlp_armed = true;
            return;
        }
        g_ctx.tx_active = false; /* short last packet already terminated it */
        return;
    }
}

static void bulk_out_pump(void)
{
    uint8_t guard = 8;
    while (guard--) {
        uint16_t n, free_space;
        if (!(rd8(R_RXISR) & 0x04)) /* FIFO1 OUT bit (SPEC 5.2) */
            return;
        n = (uint16_t)(rd8(R_FIFO1CSR) | ((rd8(R_FIFO1CSR + 1) & 0x07u) << 8));
        if (n == 0) {                 /* no payload: just clear OUT+SPK */
            wr8(R_RXISR, 0x0C);
            continue;
        }
        if (n > MPS)                  /* MPS enforces n<=64; be defensive */
            n = MPS;
        free_space = (uint16_t)(RX_RING - 1u - g_ctx.rx_count);
        if (free_space < n)
            return; /* backpressure: leave the packet in the FIFO, retry later */
        if (!dma_run(0x02, 0x10, (unsigned)(uintptr_t)g_ctx.rx_pkt, n))
            return; /* DMA failed/reset: stop; reset handler will recover */
        wr8(R_RXISR, 0x0C); /* W1C FIFO1 OUT + SPK */
        {
            uint16_t i;
            for (i = 0; i < n; i++) {
                g_ctx.rx_ring[g_ctx.rx_head] = g_ctx.rx_pkt[i];
                g_ctx.rx_head = (uint16_t)((g_ctx.rx_head + 1u) % RX_RING);
            }
            g_ctx.rx_count = (uint16_t)(g_ctx.rx_count + n);
        }
    }
}

/* ------------------------------------------------------------------ */
/* bus events                                                          */

static void session_reset_state(void)
{
    g_ctx.configured = false;
    g_ctx.config_value = 0;
    g_ctx.tx_active = false;
    g_ctx.tx_zlp_armed = false;
    g_ctx.tx_need_zlp = false;
    g_ctx.rx_head = g_ctx.rx_tail = g_ctx.rx_count = 0;
}

static void handle_bus_reset(void)
{
    /* SPEC 6.1 */
    session_reset_state();
    wr8(R_DEVADDR, 0x00);  /* address 0, DevConf 0 */
    wr8(R_DMACTRL, 0x18);  /* DMA Abort | ClrFifo */
    wr8(R_DMAFIFO, 0x00);
    wr8(R_CXFIFO, 0x08);   /* CX FIFO Clr */
    g_ctx.device_status = 1;
    wr8(R_DEVISR, 0x01);   /* W1C Reset */
}

static void handle_disconnect(void)
{
    /* SPEC 6.3: VBUS lost / plug removed. Tear the session down and wait for a
     * new bus reset. Keep the endpoint config; it is redone at SET_CONFIG. */
    session_reset_state();
}

/* ------------------------------------------------------------------ */
/* dock_dev_hal.h implementation                                       */

void dock_dhal_service(void *hal)
{
    uint8_t g2l, g2h, otgh, cx;
    (void)hal;

    g2l = rd8(R_DEVISR);
    g2h = (uint8_t)(rd8(R_DEVISR_B1) & 0x07u);
    otgh = rd8(R_OTGISR_B1);

    /* Bus reset first: it invalidates everything else (SPEC 3.2 note). */
    if (g2l & 0x01) {
        handle_bus_reset();
        return; /* re-poll next call from a clean slate */
    }

    /* Disconnect / plug removal / role loss (SPEC 6.3). */
    if ((otgh & 0x18) || (rd8(R_OTGISR_B0) & 0x40) ||
        !(rd8(R_OTGCSR_B2) & 0x02)) {
        if (otgh & 0x18)
            wr8(R_OTGISR_B1, (uint8_t)(otgh & 0x18)); /* W1C BPlug/APlug */
        if (rd8(R_OTGISR_B0) & 0x40)
            wr8(R_OTGISR_B0, 0x40);                   /* W1C BSessEnd */
        handle_disconnect();
        /* fall through: if VBUS is actually still valid we simply re-arm */
    }
    /* Role/ID change: just clear it; role is re-checked via B_SESS_VLD above. */
    if (otgh & 0x03)
        wr8(R_OTGISR_B1, (uint8_t)(otgh & 0x03));

    /* Reconnect: after init we hold soft-disconnect until VBUS is valid, then
     * drop it so the host enumerates us (SPEC 2.1 steps 25-27 / gotcha 13). */
    if (g_ctx.unplug_set) {
        if ((rd8(R_OTGCSR_B2) & 0x02) || (g2h & 0x04)) {
            wr8(R_PHYTMSR, (uint8_t)(rd8(R_PHYTMSR) & ~0x01u)); /* Unplug = 0 */
            if (g2h & 0x04)
                wr8(R_DEVISR_B1, 0x04); /* W1C DevWakeup */
            g_ctx.unplug_set = false;
        }
    } else if (g2h & 0x04) {
        /* Mid-session wake by VBUS: make sure Unplug is clear. */
        wr8(R_PHYTMSR, (uint8_t)(rd8(R_PHYTMSR) & ~0x01u));
        wr8(R_DEVISR_B1, 0x04);
    }

    /* Suspend / resume: just acknowledge (SPEC 6.2). */
    if (g2l & 0x02)
        wr8(R_DEVISR, 0x02);
    if (g2l & 0x04)
        wr8(R_DEVISR, 0x04);

    /* EP0 (SPEC 3.2 loop / section 4). */
    cx = rd8(R_CXISR);
    if (cx & 0x20)             /* Abort: a new Setup overrode a transfer */
        wr8(R_CXISR, 0x20);
    if (cx & 0x01)             /* Setup: handle_setup() acks Setup itself */
        handle_setup();
    else {
        if (cx & 0x08) {       /* End: FIN + ack End */
            wr8(R_CXFIFO, 0x01);
            wr8(R_CXISR, 0x08);
        }
        if (cx & 0x10)         /* Err */
            wr8(R_CXISR, 0x10);
    }

    /* Bulk OUT, zero-length OUT, bulk IN. */
    bulk_out_pump();

    if (g2l & 0x40) {          /* ZlpRx (SPEC 5.3) */
        halce_rxzlp_clear();   /* near-atomic read+clear of usbRxZlp */
        wr8(R_DEVISR, 0x40);
    }
    if (g2l & 0x20) {          /* ZlpTx: our terminating ZLP has gone (SPEC 5.1) */
        wr8(R_TXZLP, 0x00);    /* plain write-0 clear (gotcha 3) */
        wr8(R_DEVISR, 0x20);
        if (g_ctx.tx_zlp_armed) {
            g_ctx.tx_zlp_armed = false;
            g_ctx.tx_active = false;
        }
    }

    bulk_in_pump();
}

bool dock_dhal_link_up(void *hal)
{
    (void)hal;
    return g_ctx.configured;
}

bool dock_dhal_tx(void *hal, const void *p, size_t n, bool last)
{
    (void)hal;
    if (!g_ctx.configured)
        return false;
    if (g_ctx.tx_active)
        return false;
    g_ctx.tx_buf = (const uint8_t *)p;
    g_ctx.tx_len = n;
    g_ctx.tx_sent = 0;
    /* Terminate with a ZLP only when this is the final piece of the frame AND
     * its length is a multiple of the max packet size (otherwise the trailing
     * short packet already ends the transfer). Non-final multiple-of-64 pieces
     * are left unterminated so the host sees one continuous byte stream. */
    g_ctx.tx_need_zlp = last && n != 0 && (n % MPS) == 0;
    g_ctx.tx_zlp_armed = false;
    g_ctx.tx_active = true;
    bulk_in_pump();
    return true;
}

bool dock_dhal_tx_idle(void *hal)
{
    (void)hal;
    return !g_ctx.tx_active;
}

size_t dock_dhal_tx_max(void *hal)
{
    (void)hal;
    /* A multiple of MPS so that splitting a large piece into several
     * dock_dhal_tx() calls never injects a spurious short packet mid-frame.
     * We packetize to 64 internally; the engine chunks bulk already. */
    return 65472u; /* 1023 * 64 */
}

size_t dock_dhal_rx(void *hal, uint8_t *buf, size_t max)
{
    size_t got = 0;
    (void)hal;
    while (got < max && g_ctx.rx_count) {
        buf[got++] = g_ctx.rx_ring[g_ctx.rx_tail];
        g_ctx.rx_tail = (uint16_t)((g_ctx.rx_tail + 1u) % RX_RING);
        g_ctx.rx_count--;
    }
    return got;
}

uint32_t dock_dhal_ms(void *hal)
{
    (void)hal;
    return time_ce_ms();
}

bool dock_dhal_fb(void *hal, dock_fb_t *fb)
{
    (void)hal;
    return halce_lcd_fb(fb);
}

uint32_t dock_dhal_row_hash(const uint8_t *row, size_t n)
{
    return dock_row_hash_c(row, n);
}

/* ------------------------------------------------------------------ */
/* init / cleanup / public glue                                        */

static void busywait_ms(uint32_t ms)
{
    uint32_t t0 = time_ce_ms();
    while ((uint32_t)(time_ce_ms() - t0) < ms)
        ;
}

uint32_t halce_calc_id(void)
{
    /* Derive a stable nonzero id from the 8-byte certificate calc ID that the
     * OS exposes via os_GetSystemInfo (ti/info.h). FNV-1a over the 8 bytes. */
    const system_info_t *si = os_GetSystemInfo();
    uint32_t h = 0x811C9DC5u;
    int i;
    for (i = 0; i < 8; i++) {
        h ^= si ? si->calcid[i] : (uint8_t)(i + 1);
        h *= 0x01000193u;
    }
    if (h == 0)
        h = 0xC5D00001u; /* never 0 (the host treats 0 as "unset") */
    return h;
}

bool halce_init(void)
{
    uint8_t m0, m1, m2;
    uint32_t t0;

    memset(&g_ctx, 0, sizeof g_ctx);
    time_ce_init(); /* millisecond clock, needed for the delays below */

    /* --- A. Fence off TI-OS (SPEC 2.1 A) --- */
    int_Disable(); /* the CRT is already di; make it explicit for the session */
    m0 = rd8(R_MPIMASK + 0);
    m1 = rd8(R_MPIMASK + 1);
    m2 = rd8(R_MPIMASK + 2);
    g_ctx.saved_mpimask[0] = m0;
    g_ctx.saved_mpimask[1] = m1;
    g_ctx.saved_mpimask[2] = m2;
    wr8(R_MPIMASK + 0, (uint8_t)(m0 & ~0x08u)); /* intTmr3 off */
    wr8(R_MPIMASK + 1, (uint8_t)(m1 & ~0x20u)); /* intUsb off */
    ((void (*)(void))ROUTINE_USB_DISABLETIMER)(); /* boot usb_DisableTimer */
    wr8(USB_HANDLEKEYS, 0);  /* keep GetCSC off usbArea (SPEC 2.1 A5) */
    wr8(USB_INITED, 1);      /* nonzero during the session (SPEC 2.1 A6) */
    wr8(OS_FLAGS_1B, (uint8_t)(rd8(OS_FLAGS_1B) | 0x20u)); /* mirror usbdrvce */

    /* --- B. Quiesce the host half of the OTG controller (SPEC 2.1 B) --- */
    wr8(R_DMACTRL_B3, 0x00); /* clear the register-window-disable bit (gotcha 2) */
    wr8(R_USBINTEN, 0x00);
    wr8(R_USBCMD, 0x00);     /* RunStop = 0 */
    t0 = time_ce_ms();
    while (!(rd8(R_USBSTS_B1) & 0x10) &&
           (uint32_t)(time_ce_ms() - t0) < 5u) /* HcHalted, bounded ~5 ms */
        ;

    /* --- C. Require device (B) role; do not drive VBUS (SPEC 2.1 C) --- */
    if (!(rd8(R_OTGCSR_B2) & 0x10)) { /* bit20 Role: 0 = host */
        time_ce_cleanup();
        return false; /* a mini-A plug makes the calc a host: cannot be a device */
    }
    wr8(R_OTGISR_B1, 0x03); /* clear stale RoleChg | IdChg */
    wr8(R_OTGCSR_B0, (uint8_t)(rd8(R_OTGCSR_B0) & ~0x80u));            /* ASrpEn=0 */
    wr8(R_OTGCSR_B0, (uint8_t)((rd8(R_OTGCSR_B0) | 0x20u) & ~0x10u));  /* ABusDrop=1,ABusReq=0 */
    /* (skip the undocumented OS routine 0x021C68; VBUS is not being driven) */
    wr8(R_OTGIER_B0, 0x31);
    wr8(R_OTGIER_B1, 0x1F);
    wr8(R_USBIMR, 0x08);

    /* --- D. Device controller: maps, reset, soft-disconnect (SPEC 2.1 D) --- */
    wr8(R_FIFOCFG + 0, 0x00); wr8(R_FIFOCFG + 1, 0x00);
    wr8(R_FIFOCFG + 2, 0x00); wr8(R_FIFOCFG + 3, 0x00);
    wr8(R_FIFOMAP + 0, 0x01); wr8(R_FIFOMAP + 1, 0x02); /* FIFO f -> EP f+1 */
    wr8(R_FIFOMAP + 2, 0x03); wr8(R_FIFOMAP + 3, 0x04);
    wr8(R_EPMAP + 0, 0x00); wr8(R_EPMAP + 1, 0x11);     /* EPn -> FIFO n-1 */
    wr8(R_EPMAP + 2, 0x22); wr8(R_EPMAP + 3, 0x33);
    wr8(R_IDLE, 0x07);          /* 7 ms PHY idle */
    wr8(R_DEVCTRL_B1, 0x02);    /* ForceFullSpd */
    wr8(R_DEVCTRL, 0x35);       /* DevReset|DevEn|GirqEn|RemoteWake */
    busywait_ms(1);             /* >= 200 us settle after soft reset (SPEC 7) */
    /* re-write the maps in case the soft reset perturbed them */
    wr8(R_FIFOMAP + 0, 0x01); wr8(R_FIFOMAP + 1, 0x02);
    wr8(R_FIFOMAP + 2, 0x03); wr8(R_FIFOMAP + 3, 0x04);
    wr8(R_EPMAP + 0, 0x00); wr8(R_EPMAP + 1, 0x11);
    wr8(R_EPMAP + 2, 0x22); wr8(R_EPMAP + 3, 0x33);
    wr8(R_PHYTMSR, 0x01);       /* Unplug = 1: soft-disconnect */

    /* interrupt masks as usbdrvce leaves them (irrelevant to polling) */
    wr8(R_GIMR, 0x00); wr8(R_CXIMR, 0x00); wr8(R_RXIMR, 0x00);
    wr8(R_TXIMR, 0x0F); wr8(R_DEVIMR, 0x00); wr8(R_DEVIMR_B1, 0x02);

    /* clear stale status (W1C; SPEC 2.1 step 24) */
    wr8(R_CXISR, 0x3F); wr8(R_RXISR, 0xFF); wr8(R_TXISR, 0x0F);
    wr8(R_DEVISR, 0xFF); wr8(R_DEVISR_B1, 0x07);
    wr8(R_RXZLP, 0x00); wr8(R_TXZLP, 0x00);
    wr8(R_OTGISR_B0, 0x71); wr8(R_OTGISR_B1, 0x1F);
    wr8(R_DMAFIFO, 0x00); wr8(R_DEVADDR, 0x00);

    /* Hold the disconnect so the host forgets TI's device (SPEC 2.1 step 25). */
    busywait_ms(250);

    g_ctx.unplug_set = true; /* service() clears Unplug once VBUS is valid */
    g_ctx.device_status = 1;
    g_ctx.calc_id = halce_calc_id();
    return true;
}

void halce_cleanup(void)
{
    uint32_t t0;

    /* SPEC 2.2 recommended polled-driver exit. */
    int_Disable();
    wr8(R_DMACTRL, 0x18); /* abort DMA | ClrFifo */
    wr8(R_DMAFIFO, 0x00);

    /* mask state as usbdrvce leaves it */
    wr8(R_GIMR, 0x00);
    wr8(R_DEVIMR_B1, 0x00);
    wr8(R_RXIMR, 0xFF);
    wr8(R_TXIMR, 0x0F);

    /* halt the host controller (should already be halted) */
    wr8(R_USBINTEN, 0x00);
    wr8(R_USBCMD, 0x00);
    t0 = time_ce_ms();
    while (!(rd8(R_USBSTS_B1) & 0x10) &&
           (uint32_t)(time_ce_ms() - t0) < 5u)
        ;

    /* Zero usbArea..usbInited inclusive: makes usbInited = 0 (the OS re-init
     * trigger) and usbHandleKeys = 0 (SPEC 2.2 step 3/6). */
    memset((void *)USB_AREA, 0, (size_t)(USB_INITED - USB_AREA + 1u));
    wr8(OS_FLAGS_1B, (uint8_t)(rd8(OS_FLAGS_1B) & ~0x20u)); /* clear flags bit5 */

    /* We leave PHYTMSR untouched (the proven path; SPEC 2.2 step 3 note). */

    /* Restore mpIntMask with intUsb (bit13 = byte+1 b5) enabled so TI-OS
     * re-inits its USB stack when interrupts come back (SPEC 2.2 step 8). */
    wr8(R_MPIMASK + 0, g_ctx.saved_mpimask[0]);
    wr8(R_MPIMASK + 1, (uint8_t)(g_ctx.saved_mpimask[1] | 0x20u));
    wr8(R_MPIMASK + 2, g_ctx.saved_mpimask[2]);

    time_ce_cleanup();
    int_Enable(); /* ei: hand interrupts (and USB) back to TI-OS */
}

void *halce_hal(void)
{
    return &g_ctx;
}
