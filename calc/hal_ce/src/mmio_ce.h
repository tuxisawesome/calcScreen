/*
 * mmio_ce.h - byte-granular MMIO helpers for the CE FOTG210 USB controller.
 *
 * SPEC gotcha 2 / gotcha 18: the eZ80 bus is 8-bit and the eZ80 C compiler may
 * emit multi-byte accesses for uint16/24/32 volatile lvalues. Every USB
 * register access MUST therefore be a single-byte `volatile uint8_t` read or
 * write. These inlines guarantee that: each line is one volatile byte access,
 * which the compiler may not reorder relative to another volatile access nor
 * coalesce into a wider transfer. All addresses are absolute and use the USB
 * base 0xE20000 (SPEC section 0.1 - NOT 0xE10000, which is the SHA unit).
 */
#ifndef MMIO_CE_H
#define MMIO_CE_H

#include <stdint.h>

/* An eZ80 address is 24-bit; `unsigned` is 24-bit on this target, so an
 * address fits with no truncation and the int->pointer cast is width-exact. */
static inline uint8_t rd8(unsigned addr)
{
    return *(volatile uint8_t *)addr;
}

static inline void wr8(unsigned addr, uint8_t v)
{
    *(volatile uint8_t *)addr = v;
}

/*
 * Write a value as three consecutive little-endian bytes. Used for the DMA
 * address (0xE201CC-CE) and DMA length (0xE201C9-CB) registers, which the SPEC
 * (5. table, drv:3318/3330-3331) programs one byte at a time. For the length
 * register this also writes 0 into 0xE201CB, keeping both len[16] and the
 * register-disable bit 31 clear (SPEC gotcha 2 / section 0.3).
 */
static inline void wr24(unsigned addr, unsigned v)
{
    *(volatile uint8_t *)(addr + 0) = (uint8_t)v;
    *(volatile uint8_t *)(addr + 1) = (uint8_t)(v >> 8);
    *(volatile uint8_t *)(addr + 2) = (uint8_t)(v >> 16);
}

#endif /* MMIO_CE_H */
