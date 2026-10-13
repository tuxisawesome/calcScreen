/*
 * sdnotify.h - minimal systemd notify protocol (no libsystemd): READY=1 and
 * watchdog keep-alives over the $NOTIFY_SOCKET datagram socket. Everything is
 * a no-op when not started by systemd.
 */
#ifndef HOST_SDNOTIFY_H
#define HOST_SDNOTIFY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct sdnotify {
    int fd;
    uint64_t wd_interval_ms; /* 0 = no watchdog */
    uint64_t wd_last_ms;
} sdnotify_t;

void sdnotify_init(sdnotify_t *n);
void sdnotify_close(sdnotify_t *n);
bool sdnotify_send(sdnotify_t *n, const char *msg);
void sdnotify_ready(sdnotify_t *n);
/* Send WATCHDOG=1 when due (every half $WATCHDOG_USEC). */
void sdnotify_tick(sdnotify_t *n, uint64_t now_ms);
/* Max poll() timeout to stay on schedule, -1 if no watchdog. */
int sdnotify_timeout_ms(const sdnotify_t *n, uint64_t now_ms);

#endif /* HOST_SDNOTIFY_H */
