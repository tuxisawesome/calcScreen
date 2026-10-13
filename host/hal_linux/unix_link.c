/* unix_link.c - see unix_link.h. */
#define _GNU_SOURCE
#include "unix_link.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define RX_CHUNK 65536

struct unix_link {
    char path[108];
    unix_link_cb_t cb;
    int listen_fd;
    int fd;              /* client, -1 = none */
    bool dead;           /* close at the next handle() */
    bool paused;
    uint8_t *tx;
    size_t tx_off, tx_len, tx_cap;
    uint8_t rx[RX_CHUNK];
};

unix_link_t *unix_link_open(const char *path, const unix_link_cb_t *cb)
{
    unix_link_t *u;
    struct sockaddr_un sa;
    if (strlen(path) >= sizeof sa.sun_path) {
        loge("unix: socket path too long");
        return NULL;
    }
    u = calloc(1, sizeof *u);
    if (!u)
        return NULL;
    u->cb = *cb;
    u->fd = -1;
    str_copy(u->path, sizeof u->path, path);
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path));
    u->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    unlink(path); /* stale socket from a previous run */
    if (u->listen_fd < 0 || bind(u->listen_fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        listen(u->listen_fd, 2) != 0) {
        loge("unix: listen on %s: %s", path, strerror(errno));
        if (u->listen_fd >= 0)
            close(u->listen_fd);
        free(u);
        return NULL;
    }
    return u;
}

static void drop_client(unix_link_t *u)
{
    if (u->fd < 0)
        return;
    close(u->fd);
    u->fd = -1;
    u->dead = false;
    u->tx_off = u->tx_len = 0;
    logv("unix: client disconnected");
    if (u->cb.down)
        u->cb.down(u->cb.user);
}

void unix_link_close(unix_link_t *u)
{
    if (!u)
        return;
    if (u->fd >= 0)
        close(u->fd);
    close(u->listen_fd);
    unlink(u->path);
    free(u->tx);
    free(u);
}

int unix_link_pollfds(unix_link_t *u, struct pollfd *pfd, int max)
{
    int n = 0;
    if (max > 0 && !u->paused) {
        pfd[n].fd = u->listen_fd;
        pfd[n].events = POLLIN;
        pfd[n++].revents = 0;
    }
    if (u->fd >= 0 && n < max) {
        pfd[n].fd = u->fd;
        pfd[n].events = (short)(POLLIN | (u->tx_len ? POLLOUT : 0));
        pfd[n++].revents = 0;
    }
    return n;
}

static void accept_client(unix_link_t *u)
{
    int fd = accept4(u->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    if (u->paused) {
        close(fd);
        return;
    }
    drop_client(u); /* newest client wins */
    u->fd = fd;
    logv("unix: client connected");
    if (u->cb.up)
        u->cb.up(u->cb.user);
}

static void read_client(unix_link_t *u)
{
    int rounds = 16; /* bounded so a fast sender cannot starve the loop */
    while (u->fd >= 0 && !u->dead && rounds--) {
        ssize_t n = recv(u->fd, u->rx, sizeof u->rx, 0);
        if (n > 0) {
            u->cb.rx(u->cb.user, u->rx, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        u->dead = true; /* EOF or error */
    }
}

void unix_link_handle(unix_link_t *u, const struct pollfd *pfd, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!pfd[i].revents)
            continue;
        if (pfd[i].fd == u->listen_fd)
            accept_client(u);
        else if (pfd[i].fd == u->fd) {
            if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
                read_client(u);
            if (pfd[i].revents & POLLOUT)
                unix_link_flush(u);
        }
    }
    if (u->dead)
        drop_client(u);
}

bool unix_link_up(const unix_link_t *u)
{
    return u->fd >= 0 && !u->dead;
}

bool unix_link_tx(unix_link_t *u, const void *p, size_t n)
{
    if (!unix_link_up(u))
        return false;
    if (u->tx_len + n > UNIX_LINK_TX_MAX) {
        logv("unix: transmit queue overflow");
        u->dead = true;
        return false;
    }
    if (u->tx_off + u->tx_len + n > u->tx_cap) {
        if (u->tx_off) {
            memmove(u->tx, u->tx + u->tx_off, u->tx_len);
            u->tx_off = 0;
        }
        if (u->tx_len + n > u->tx_cap) {
            size_t cap = u->tx_cap ? u->tx_cap : 65536;
            uint8_t *nb;
            while (cap < u->tx_len + n)
                cap *= 2;
            nb = realloc(u->tx, cap);
            if (!nb) {
                u->dead = true;
                return false;
            }
            u->tx = nb;
            u->tx_cap = cap;
        }
    }
    memcpy(u->tx + u->tx_off + u->tx_len, p, n);
    u->tx_len += n;
    return true;
}

void unix_link_flush(unix_link_t *u)
{
    while (u->fd >= 0 && !u->dead && u->tx_len) {
        ssize_t w = send(u->fd, u->tx + u->tx_off, u->tx_len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN)
                u->dead = true;
            return;
        }
        u->tx_off += (size_t)w;
        u->tx_len -= (size_t)w;
    }
    if (!u->tx_len)
        u->tx_off = 0;
}

void unix_link_set_paused(unix_link_t *u, bool paused)
{
    u->paused = paused;
    if (paused)
        drop_client(u);
}
