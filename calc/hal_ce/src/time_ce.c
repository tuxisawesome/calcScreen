/*
 * time_ce.c - monotonic millisecond clock using CE general-purpose timer 2.
 *
 * Which timer and why
 * --------------------
 * We program timer 2 (the hardware GPT block at 0xF20000) as a free-running
 * UP counter clocked by the 32768 Hz crystal, with NO interrupt. Reading it is
 * a plain MMIO load, so it works while the CPU is `di` and while USB/timer-3
 * interrupts are masked - which is exactly the state the USB driver keeps the
 * machine in (SPEC 2.1 step A / section 3.1). It deliberately does not touch
 * timer 1 (used by the C runtime's clock()) or timer 3 (used by TI-OS for USB).
 *
 * Restore
 * -------
 * time_ce_init() saves timer 2's control bits and reload value and
 * time_ce_cleanup() restores them, touching only timer 2's bits in the shared
 * control register. TI-OS does not use timer 2, so in practice we find it
 * disabled and hand it back disabled.
 *
 * Wrap
 * ----
 * The counter is 32-bit at 32768 Hz, so it wraps about every 36 hours; the
 * derived millisecond value therefore wraps about every 131072000 ms rather
 * than at 2^32. All callers use `(uint32_t)(now - then)` deltas over intervals
 * of at most a few seconds, which are unaffected except for a single possibly
 * long delta if a session happens to straddle the 36-hour boundary.
 */
#include "time_ce.h"

#include <stdbool.h>
#include <sys/timers.h>

/* Control-register bits owned by timer 2: enable/rate/int at 3*(2-1)=3..5 and
 * the count-direction bit at (2)+8 = 10. */
#define TMR2_CTRL_MASK ((uint16_t)((0x7u << 3) | (1u << 10)))

static uint16_t s_saved_ctrl;
static uint32_t s_saved_reload;
static bool s_inited;

void time_ce_init(void)
{
    if (s_inited)
        return;
    s_saved_ctrl = timer_Control;
    s_saved_reload = timer_2_ReloadValue;

    timer_Disable(2);
    timer_Set(2, 0u);
    timer_SetReload(2, 0xFFFFFFFFu);
    /* 32768 Hz crystal, no interrupt, count up. */
    timer_Enable(2, TIMER_32K, TIMER_NOINT, TIMER_UP);
    s_inited = true;
}

void time_ce_cleanup(void)
{
    if (!s_inited)
        return;
    timer_Disable(2);
    /* Restore only timer 2's bits in the shared control register. */
    timer_Control = (uint16_t)((timer_Control & ~TMR2_CTRL_MASK) |
                               (s_saved_ctrl & TMR2_CTRL_MASK));
    timer_SetReload(2, s_saved_reload);
    s_inited = false;
}

uint32_t time_ce_ms(void)
{
    /* atomic_load_increasing_32 reads a rising 32-bit counter off the 8-bit bus
     * without tearing (sys/util.h via sys/timers.h). */
    uint32_t t = atomic_load_increasing_32(TIMER_COUNT_ADDR(2));

    /* ms = t * 1000 / 32768, split so no intermediate overflows 32 bits:
     * (t/32768) <= 131072, *1000 < 2^32; (t%32768) < 32768, *1000 < 2^32. */
    return (t / 32768u) * 1000u + ((t % 32768u) * 1000u) / 32768u;
}
