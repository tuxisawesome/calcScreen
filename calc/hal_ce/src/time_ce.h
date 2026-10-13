/*
 * time_ce.h - monotonic millisecond clock for the CE USB HAL.
 *
 * Runs entirely off a free-running general-purpose hardware timer, so it keeps
 * ticking while USB (and timer-3) interrupts are masked and while the whole
 * driver runs under `di`. It does NOT use timer 3 or any interrupt.
 */
#ifndef TIME_CE_H
#define TIME_CE_H

#include <stdint.h>

/* Program the timer and start counting. Saves the previous timer state so it
 * can be restored. Safe to call more than once. */
void time_ce_init(void);

/* Restore the timer to how we found it. */
void time_ce_cleanup(void);

/* Milliseconds since time_ce_init(). Monotonic; see the note in time_ce.c
 * about the wrap period. */
uint32_t time_ce_ms(void);

#endif /* TIME_CE_H */
