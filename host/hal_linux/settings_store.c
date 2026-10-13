/* settings_store.c - see settings_store.h. */
#define _GNU_SOURCE
#include "settings_store.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void slot_path(const settings_store_t *st, int slot, char *out, size_t cap)
{
    snprintf(out, cap, "%s/settings.%d", st->dir, slot ? 1 : 0);
}

static size_t slot_read(void *user, int slot, uint8_t *buf, size_t cap)
{
    const settings_store_t *st = user;
    char path[PATH_MAX];
    size_t got = 0;
    int fd;
    slot_path(st, slot, path, sizeof path);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    while (got < cap) {
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    return got;
}

static bool fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool ok;
    if (fd < 0)
        return false;
    ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

static bool slot_write(void *user, int slot, const uint8_t *buf, size_t n)
{
    const settings_store_t *st = user;
    char path[PATH_MAX];
    bool ok;
    int fd;
    slot_path(st, slot, path, sizeof path);
    /* In place, not tmp+rename: the A/B scheme already guarantees the other
     * slot is intact, and rename on FAT is not atomic anyway. */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        loge("settings: %s: %s", path, strerror(errno));
        return false;
    }
    ok = write_all(fd, buf, n) && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (ok)
        ok = fsync_dir(st->dir);
    if (!ok)
        loge("settings: writing %s failed: %s", path, strerror(errno));
    return ok;
}

bool settings_store_init(settings_store_t *st, const char *dir)
{
    memset(st, 0, sizeof *st);
    if (!str_copy(st->dir, sizeof st->dir, dir))
        return false;
    st->io.user = st;
    st->io.read = slot_read;
    st->io.write = slot_write;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        loge("settings: mkdir %s: %s", dir, strerror(errno));
        return false;
    }
    return true;
}

void settings_store_load(settings_store_t *st, dock_settings_t *s, uint32_t *seq)
{
    if (dock_settings_load(&st->io, s, seq))
        return;
    logv("settings: no valid record, using defaults");
    dock_settings_defaults(s, random_u32());
    *seq = 0;
    if (!dock_settings_save(&st->io, s, seq))
        loge("settings: could not save defaults");
}
