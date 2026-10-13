/*
 * settings_store.h - dock_store_io_t (core/dock_settings.h) over two files in
 * a state directory ("settings.0", "settings.1"). Each save rewrites one slot
 * with open/write/fsync and then fsyncs the directory, so after a power cut
 * at least one slot is always intact.
 */
#ifndef HOST_SETTINGS_STORE_H
#define HOST_SETTINGS_STORE_H

#include <limits.h>

#include "dock_settings.h"

typedef struct settings_store {
    char dir[PATH_MAX - 32];
    dock_store_io_t io;
} settings_store_t;

/* Creates dir if missing. Returns false if it cannot be created. */
bool settings_store_init(settings_store_t *st, const char *dir);

/* Load settings, or create defaults (random dock id and join code) and save
 * them when no valid slot exists. */
void settings_store_load(settings_store_t *st, dock_settings_t *s, uint32_t *seq);

#endif /* HOST_SETTINGS_STORE_H */
