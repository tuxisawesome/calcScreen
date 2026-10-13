/*
 * dock_common.h - shared definitions for the calcScreen dock protocol.
 *
 * Portability rules for everything in core/:
 *   - C99, freestanding-friendly: only <stdint.h>, <stddef.h>, <stdbool.h>,
 *     <string.h>.
 *   - No malloc, no global or static mutable state: every module works on a
 *     caller-owned context struct.
 *   - No floating point, never assume sizeof(int) >= 4 (the eZ80 int is 24
 *     bits), all multi-byte wire values are explicit little-endian.
 */
#ifndef DOCK_COMMON_H
#define DOCK_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOCK_PROTO_VERSION 1u

/* USB identity of the calculator while the DOCK driver owns the port.
 * 1209:0001 is the pid.codes test PID; override at build time if you get a
 * real allocation. The host also checks the interface class/subclass. */
#ifndef DOCK_USB_VID
#define DOCK_USB_VID 0x1209u
#endif
#ifndef DOCK_USB_PID
#define DOCK_USB_PID 0x0001u
#endif
#define DOCK_USB_IFACE_CLASS    0xFFu /* vendor specific */
#define DOCK_USB_IFACE_SUBCLASS 0xD0u
#define DOCK_USB_IFACE_PROTOCOL 0xC5u
#define DOCK_USB_EP_IN  0x81u /* calc -> host bulk */
#define DOCK_USB_EP_OUT 0x02u /* host -> calc bulk */
#define DOCK_USB_MAX_PACKET 64u

/* Status codes carried in responses (uint8 on the wire). */
enum dock_status {
    DOCK_OK = 0,
    DOCK_E_END = 1,          /* end of file / end of list */
    DOCK_E_NOT_FOUND = 2,
    DOCK_E_IO = 3,
    DOCK_E_INVALID = 4,
    DOCK_E_NO_SPACE = 5,
    DOCK_E_DENIED = 6,
    DOCK_E_NET_DISABLED = 7,
    DOCK_E_BUSY = 8,
    DOCK_E_UNSUPPORTED = 9,
    DOCK_E_NO_DRIVE = 10,
    DOCK_E_TIMEOUT = 11,     /* local: no response in time */
    DOCK_E_DISCONNECTED = 12,/* local: link is down */
    DOCK_E_JOIN_REQUIRED = 13,
    DOCK_E_JOIN_BAD = 14,
    DOCK_E_TOO_BIG = 15
};

/* ---- little-endian helpers (no alignment assumptions) ---- */
static inline void dock_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void dock_put_u24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}
static inline void dock_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t dock_get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t dock_get_u24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static inline uint32_t dock_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}
#endif

#endif /* DOCK_COMMON_H */
