/*
 * unix_link.h - a FAST-mode "calculator" over a Unix stream socket. dockd
 * listens; the connected client (the simulator, or a test) is treated exactly
 * like the DOCK driver on USB. A new client replaces the old one.
 */
#ifndef HOST_UNIX_LINK_H
#define HOST_UNIX_LINK_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNIX_LINK_TX_MAX (4u * 1024u * 1024u)

typedef struct unix_link_cb {
    void *user;
    void (*up)(void *user);
    void (*rx)(void *user, const uint8_t *p, size_t n);
    void (*down)(void *user);
} unix_link_cb_t;

typedef struct unix_link unix_link_t;

unix_link_t *unix_link_open(const char *path, const unix_link_cb_t *cb);
void unix_link_close(unix_link_t *u);

int unix_link_pollfds(unix_link_t *u, struct pollfd *pfd, int max);
void unix_link_handle(unix_link_t *u, const struct pollfd *pfd, int n);

bool unix_link_up(const unix_link_t *u);
bool unix_link_tx(unix_link_t *u, const void *p, size_t n);
void unix_link_flush(unix_link_t *u);
/* Paused: disconnect the client and refuse new ones. */
void unix_link_set_paused(unix_link_t *u, bool paused);

#endif /* HOST_UNIX_LINK_H */
