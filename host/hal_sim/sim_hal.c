/* sim_hal.c - see sim_hal.h. */
#define _GNU_SOURCE
#include "sim_hal.h"
#include "dock_dev_hal.h"
#include "dock_screen.h"
#include "util.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define RECONNECT_MS 100u
#define TX_MAX 16384u

void sim_hal_init(sim_hal_t *h, const char *path)
{
    memset(h, 0, sizeof *h);
    h->fd = -1;
    str_copy(h->path, sizeof h->path, path);
}

static void disconnect(sim_hal_t *h)
{
    if (h->fd >= 0)
        close(h->fd);
    h->fd = -1;
    h->rx_head = h->rx_len = 0;
    h->tx_p = NULL;
    h->tx_len = h->tx_off = 0;
    h->next_connect_ms = mono_ms() + RECONNECT_MS;
}

void sim_hal_close(sim_hal_t *h)
{
    disconnect(h);
}

void sim_put(sim_hal_t *h, unsigned x, unsigned y, uint16_t c)
{
    uint8_t *p = h->fb + ((size_t)y * SIM_W + x) * 2u;
    p[0] = (uint8_t)c;
    p[1] = (uint8_t)(c >> 8);
}

uint16_t sim_get(const sim_hal_t *h, unsigned x, unsigned y)
{
    const uint8_t *p = h->fb + ((size_t)y * SIM_W + x) * 2u;
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void try_connect(sim_hal_t *h)
{
    struct sockaddr_un sa;
    int fd;
    if (mono_ms() < h->next_connect_ms)
        return;
    h->next_connect_ms = mono_ms() + RECONNECT_MS;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, h->path, strlen(h->path));
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return;
    }
    fd_set_nonblock(fd);
    h->fd = fd;
}

static void pump_rx(sim_hal_t *h)
{
    while (h->fd >= 0 && h->rx_len < SIM_RX_RING) {
        size_t tail = (h->rx_head + h->rx_len) % SIM_RX_RING;
        size_t room = SIM_RX_RING - h->rx_len;
        size_t contig = SIM_RX_RING - tail < room ? SIM_RX_RING - tail : room;
        ssize_t n = recv(h->fd, h->rx + tail, contig, 0);
        if (n > 0) {
            h->rx_len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        disconnect(h); /* EOF: dockd went away */
    }
}

static void pump_tx(sim_hal_t *h)
{
    while (h->fd >= 0 && h->tx_off < h->tx_len) {
        ssize_t n = send(h->fd, h->tx_p + h->tx_off, h->tx_len - h->tx_off, MSG_NOSIGNAL);
        if (n > 0) {
            h->tx_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        disconnect(h);
    }
}

/* ---- dock_dev_hal.h ---- */

void dock_dhal_service(void *hal)
{
    sim_hal_t *h = hal;
    if (h->fd < 0)
        try_connect(h);
    pump_rx(h);
    pump_tx(h);
}

bool dock_dhal_link_up(void *hal)
{
    return ((sim_hal_t *)hal)->fd >= 0;
}

bool dock_dhal_tx(void *hal, const void *p, size_t n, bool last)
{
    sim_hal_t *h = hal;
    (void)last; /* a stream socket needs no transfer terminator */
    if (h->fd < 0 || h->tx_off < h->tx_len || n > TX_MAX)
        return false;
    h->tx_p = p;
    h->tx_len = n;
    h->tx_off = 0;
    pump_tx(h);
    return true;
}

bool dock_dhal_tx_idle(void *hal)
{
    sim_hal_t *h = hal;
    return h->tx_off >= h->tx_len;
}

size_t dock_dhal_tx_max(void *hal)
{
    (void)hal;
    return TX_MAX;
}

size_t dock_dhal_rx(void *hal, uint8_t *buf, size_t max)
{
    sim_hal_t *h = hal;
    size_t n = h->rx_len < max ? h->rx_len : max, i;
    for (i = 0; i < n; i++)
        buf[i] = h->rx[(h->rx_head + i) % SIM_RX_RING];
    h->rx_head = (h->rx_head + n) % SIM_RX_RING;
    h->rx_len -= n;
    return n;
}

uint32_t dock_dhal_ms(void *hal)
{
    (void)hal;
    return (uint32_t)mono_ms();
}

bool dock_dhal_fb(void *hal, dock_fb_t *fb)
{
    sim_hal_t *h = hal;
    fb->pixels = h->fb;
    fb->width = SIM_W;
    fb->height = SIM_H;
    fb->stride = SIM_W * 2u;
    fb->fmt = DOCK_PIX_RGB565;
    fb->flags = DOCK_SCRF_BGR;
    fb->palette_le = NULL;
    return true;
}

uint32_t dock_dhal_row_hash(const uint8_t *row, size_t n)
{
    return dock_row_hash_c(row, n);
}
