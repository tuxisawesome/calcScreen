/*
 * sysstat.h - cheap system telemetry for the diagnostics overlay: CPU load,
 * SoC temperature, memory use and uptime, read from /proc and /sys.
 *
 * sysstat_sample() is rate-limited internally (~1 s) so the overlay can call
 * it every frame. Every field is best-effort: a missing source (e.g. no
 * thermal zone off a Pi) leaves have_* false and the overlay prints "n/a".
 * All reads are bounded; nothing blocks.
 */
#ifndef HOST_SYSSTAT_H
#define HOST_SYSSTAT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct sysstat {
    /* last /proc/stat jiffies, to difference CPU busy% between samples */
    uint64_t last_total, last_idle;
    uint64_t next_ms;   /* earliest next real sample */
    bool primed;        /* a baseline CPU reading was taken */

    /* latest values */
    bool have_cpu;
    int cpu_pct;        /* 0..100 busy since the previous sample */
    bool have_temp;
    int temp_c;         /* SoC temperature, whole degrees C */
    bool have_mem;
    uint32_t mem_used_mb, mem_total_mb;
    bool have_uptime;
    uint32_t uptime_s;
} sysstat_t;

void sysstat_init(sysstat_t *s);
/* Refresh at most once per ~1 s; now_ms is a monotonic clock in ms. */
void sysstat_sample(sysstat_t *s, uint64_t now_ms);

#endif /* HOST_SYSSTAT_H */
