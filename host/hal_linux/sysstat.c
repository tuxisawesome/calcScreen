/* sysstat.c - see sysstat.h. */
#define _GNU_SOURCE
#include "sysstat.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_INTERVAL_MS 1000u

void sysstat_init(sysstat_t *s)
{
    memset(s, 0, sizeof *s);
}

/* Sum of the user..steal fields of the aggregate "cpu " line of /proc/stat;
 * `idle` receives idle+iowait. Returns false if the line is unreadable. */
static bool read_cpu(uint64_t *total, uint64_t *idle)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[256];
    uint64_t v[10];
    int n, i;
    if (!f)
        return false;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    memset(v, 0, sizeof v);
    n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
               (unsigned long long *)&v[0], (unsigned long long *)&v[1],
               (unsigned long long *)&v[2], (unsigned long long *)&v[3],
               (unsigned long long *)&v[4], (unsigned long long *)&v[5],
               (unsigned long long *)&v[6], (unsigned long long *)&v[7],
               (unsigned long long *)&v[8], (unsigned long long *)&v[9]);
    if (n < 4)
        return false;
    *total = 0;
    for (i = 0; i < n; i++)
        *total += v[i];
    *idle = v[3] + (n > 4 ? v[4] : 0); /* idle + iowait */
    return true;
}

static void sample_cpu(sysstat_t *s)
{
    uint64_t total = 0, idle = 0;
    if (!read_cpu(&total, &idle)) {
        s->have_cpu = false;
        return;
    }
    if (s->primed && total > s->last_total) {
        uint64_t dt = total - s->last_total;
        uint64_t di = idle > s->last_idle ? idle - s->last_idle : 0;
        uint64_t busy = dt > di ? dt - di : 0;
        s->cpu_pct = (int)((busy * 100u + dt / 2u) / dt);
        if (s->cpu_pct > 100)
            s->cpu_pct = 100;
        s->have_cpu = true;
    }
    s->last_total = total;
    s->last_idle = idle;
    s->primed = true;
}

static void sample_temp(sysstat_t *s)
{
    char buf[32];
    long milli;
    char *end;
    s->have_temp = false;
    if (!read_text_file("/sys/class/thermal/thermal_zone0/temp", buf, sizeof buf))
        return;
    milli = strtol(buf, &end, 10);
    if (end == buf)
        return;
    s->temp_c = (int)((milli + 500) / 1000);
    s->have_temp = true;
}

static void sample_mem(sysstat_t *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    uint64_t total_kb = 0, avail_kb = 0;
    int got = 0;
    s->have_mem = false;
    if (!f)
        return;
    while (got < 2 && fgets(line, sizeof line, f)) {
        unsigned long long v;
        if (sscanf(line, "MemTotal: %llu kB", &v) == 1) {
            total_kb = v;
            got++;
        } else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) {
            avail_kb = v;
            got++;
        }
    }
    fclose(f);
    if (total_kb == 0)
        return;
    if (avail_kb > total_kb)
        avail_kb = total_kb;
    s->mem_total_mb = (uint32_t)(total_kb / 1024u);
    s->mem_used_mb = (uint32_t)((total_kb - avail_kb) / 1024u);
    s->have_mem = true;
}

static void sample_uptime(sysstat_t *s)
{
    char buf[64];
    double up;
    s->have_uptime = false;
    if (!read_text_file("/proc/uptime", buf, sizeof buf))
        return;
    if (sscanf(buf, "%lf", &up) != 1 || up < 0)
        return;
    s->uptime_s = (uint32_t)up;
    s->have_uptime = true;
}

void sysstat_sample(sysstat_t *s, uint64_t now_ms)
{
    if (s->next_ms != 0 && now_ms < s->next_ms)
        return;
    s->next_ms = now_ms + SAMPLE_INTERVAL_MS;
    sample_cpu(s);
    sample_temp(s);
    sample_mem(s);
    sample_uptime(s);
}
