/*
 * sim_hal.h - device HAL (core/dock_dev_hal.h) for the simulated calculator:
 * the transport is a Unix-socket client of `dockd --transport unix:PATH`
 * with the same "one transmit in flight" contract as the real USB driver,
 * and the framebuffer is a 320x240 RGB565 array the simulator draws into.
 */
#ifndef SIM_HAL_H
#define SIM_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIM_W 320u
#define SIM_H 240u
#define SIM_RX_RING (256u * 1024u)

typedef struct sim_hal {
    char path[108];
    int fd;                  /* -1 = not connected */
    uint64_t next_connect_ms;

    uint8_t rx[SIM_RX_RING]; /* received, not yet taken by the engine */
    size_t rx_head, rx_len;

    const uint8_t *tx_p;     /* the one transmit in flight */
    size_t tx_len, tx_off;

    uint8_t fb[SIM_W * SIM_H * 2u]; /* RGB565 LE, red in the high bits */
} sim_hal_t;

void sim_hal_init(sim_hal_t *h, const char *path);
void sim_hal_close(sim_hal_t *h);

/* Pixel helpers. */
void sim_put(sim_hal_t *h, unsigned x, unsigned y, uint16_t rgb565);
uint16_t sim_get(const sim_hal_t *h, unsigned x, unsigned y);

#endif /* SIM_HAL_H */
