/* sdnotify.c - see sdnotify.h. */
#define _GNU_SOURCE
#include "sdnotify.h"
#include "util.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void sdnotify_init(sdnotify_t *n)
{
    const char *wd = getenv("WATCHDOG_USEC");
    const char *pid = getenv("WATCHDOG_PID");
    n->fd = -1;
    n->wd_interval_ms = 0;
    n->wd_last_ms = 0;
    if (getenv("NOTIFY_SOCKET"))
        n->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (wd && (!pid || strtol(pid, NULL, 10) == (long)getpid())) {
        unsigned long long us = strtoull(wd, NULL, 10);
        n->wd_interval_ms = us / 2000u; /* half the timeout, in ms */
        if (n->wd_interval_ms == 0 && us)
            n->wd_interval_ms = 1;
    }
}

void sdnotify_close(sdnotify_t *n)
{
    if (n->fd >= 0)
        close(n->fd);
    n->fd = -1;
}

bool sdnotify_send(sdnotify_t *n, const char *msg)
{
    const char *path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un sa;
    size_t len;
    socklen_t salen;
    if (n->fd < 0 || !path || !path[0])
        return false;
    len = strlen(path);
    if (len >= sizeof sa.sun_path)
        return false;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, len);
    if (path[0] == '@')
        sa.sun_path[0] = '\0'; /* abstract namespace */
    salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len);
    return sendto(n->fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa,
                  salen) >= 0;
}

void sdnotify_ready(sdnotify_t *n)
{
    sdnotify_send(n, "READY=1");
    n->wd_last_ms = mono_ms();
}

void sdnotify_tick(sdnotify_t *n, uint64_t now_ms)
{
    if (!n->wd_interval_ms || now_ms - n->wd_last_ms < n->wd_interval_ms)
        return;
    n->wd_last_ms = now_ms;
    sdnotify_send(n, "WATCHDOG=1");
}

int sdnotify_timeout_ms(const sdnotify_t *n, uint64_t now_ms)
{
    uint64_t due;
    if (!n->wd_interval_ms)
        return -1;
    due = n->wd_last_ms + n->wd_interval_ms;
    return due <= now_ms ? 0 : (int)(due - now_ms > 60000u ? 60000u : due - now_ms);
}
