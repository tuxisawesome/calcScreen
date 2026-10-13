/* input_evdev.c - see input_evdev.h. */
#define _GNU_SOURCE
#include "input_evdev.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_DEVS 16
#define RESCAN_MS 3000u

typedef struct evdev {
    int fd;
    char name[32]; /* "eventN" */
} evdev_t;

struct input {
    char dir[PATH_MAX];
    input_key_fn fn;
    void *user;
    evdev_t dev[MAX_DEVS];
    int ndev;
    int ino_fd;
    bool grab;
    uint8_t mods;
    uint64_t last_scan_ms;
};

/* Linux key code -> USB HID usage (keyboard page 0x07). */
static const uint8_t k2hid[128] = {
    [KEY_A] = 0x04, [KEY_B] = 0x05, [KEY_C] = 0x06, [KEY_D] = 0x07,
    [KEY_E] = 0x08, [KEY_F] = 0x09, [KEY_G] = 0x0A, [KEY_H] = 0x0B,
    [KEY_I] = 0x0C, [KEY_J] = 0x0D, [KEY_K] = 0x0E, [KEY_L] = 0x0F,
    [KEY_M] = 0x10, [KEY_N] = 0x11, [KEY_O] = 0x12, [KEY_P] = 0x13,
    [KEY_Q] = 0x14, [KEY_R] = 0x15, [KEY_S] = 0x16, [KEY_T] = 0x17,
    [KEY_U] = 0x18, [KEY_V] = 0x19, [KEY_W] = 0x1A, [KEY_X] = 0x1B,
    [KEY_Y] = 0x1C, [KEY_Z] = 0x1D,
    [KEY_1] = 0x1E, [KEY_2] = 0x1F, [KEY_3] = 0x20, [KEY_4] = 0x21,
    [KEY_5] = 0x22, [KEY_6] = 0x23, [KEY_7] = 0x24, [KEY_8] = 0x25,
    [KEY_9] = 0x26, [KEY_0] = 0x27,
    [KEY_ENTER] = 0x28, [KEY_ESC] = 0x29, [KEY_BACKSPACE] = 0x2A,
    [KEY_TAB] = 0x2B, [KEY_SPACE] = 0x2C, [KEY_MINUS] = 0x2D,
    [KEY_EQUAL] = 0x2E, [KEY_LEFTBRACE] = 0x2F, [KEY_RIGHTBRACE] = 0x30,
    [KEY_BACKSLASH] = 0x31, [KEY_SEMICOLON] = 0x33, [KEY_APOSTROPHE] = 0x34,
    [KEY_GRAVE] = 0x35, [KEY_COMMA] = 0x36, [KEY_DOT] = 0x37,
    [KEY_SLASH] = 0x38, [KEY_CAPSLOCK] = 0x39,
    [KEY_F1] = 0x3A, [KEY_F2] = 0x3B, [KEY_F3] = 0x3C, [KEY_F4] = 0x3D,
    [KEY_F5] = 0x3E, [KEY_F6] = 0x3F, [KEY_F7] = 0x40, [KEY_F8] = 0x41,
    [KEY_F9] = 0x42, [KEY_F10] = 0x43, [KEY_F11] = 0x44, [KEY_F12] = 0x45,
    [KEY_SYSRQ] = 0x46, [KEY_SCROLLLOCK] = 0x47, [KEY_PAUSE] = 0x48,
    [KEY_INSERT] = 0x49, [KEY_HOME] = 0x4A, [KEY_PAGEUP] = 0x4B,
    [KEY_DELETE] = 0x4C, [KEY_END] = 0x4D, [KEY_PAGEDOWN] = 0x4E,
    [KEY_RIGHT] = 0x4F, [KEY_LEFT] = 0x50, [KEY_DOWN] = 0x51, [KEY_UP] = 0x52,
    [KEY_NUMLOCK] = 0x53, [KEY_KPSLASH] = 0x54, [KEY_KPASTERISK] = 0x55,
    [KEY_KPMINUS] = 0x56, [KEY_KPPLUS] = 0x57, [KEY_KPENTER] = 0x58,
    [KEY_KP1] = 0x59, [KEY_KP2] = 0x5A, [KEY_KP3] = 0x5B, [KEY_KP4] = 0x5C,
    [KEY_KP5] = 0x5D, [KEY_KP6] = 0x5E, [KEY_KP7] = 0x5F, [KEY_KP8] = 0x60,
    [KEY_KP9] = 0x61, [KEY_KP0] = 0x62, [KEY_KPDOT] = 0x63,
    [KEY_102ND] = 0x64, [KEY_COMPOSE] = 0x65, [KEY_KPEQUAL] = 0x67,
    [KEY_LEFTCTRL] = 0xE0, [KEY_LEFTSHIFT] = 0xE1, [KEY_LEFTALT] = 0xE2,
    [KEY_LEFTMETA] = 0xE3, [KEY_RIGHTCTRL] = 0xE4, [KEY_RIGHTSHIFT] = 0xE5,
    [KEY_RIGHTALT] = 0xE6, [KEY_RIGHTMETA] = 0xE7,
};

uint8_t input_key_to_hid(unsigned code)
{
    return code < sizeof k2hid ? k2hid[code] : 0;
}

/* ------------------------------------------------------------------ */

#define BITS_PER_LONG (sizeof(long) * CHAR_BIT)
#define NLONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static bool test_bit(const unsigned long *bits, unsigned n)
{
    return (bits[n / BITS_PER_LONG] >> (n % BITS_PER_LONG)) & 1ul;
}

/* A keyboard has EV_KEY with the letter keys (rules out mice, power
 * buttons, HDMI-CEC remotes...). */
static bool is_keyboard(int fd)
{
    unsigned long ev[NLONGS(EV_MAX + 1)] = {0};
    unsigned long keys[NLONGS(KEY_MAX + 1)] = {0};
    unsigned k;
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 || !test_bit(ev, EV_KEY))
        return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
        return false;
    for (k = KEY_Q; k <= KEY_P; k++) /* the QWERTYUIOP row */
        if (!test_bit(keys, k))
            return false;
    return test_bit(keys, KEY_A) && test_bit(keys, KEY_Z);
}

static bool has_dev(const input_t *in, const char *name)
{
    int i;
    for (i = 0; i < in->ndev; i++)
        if (strcmp(in->dev[i].name, name) == 0)
            return true;
    return false;
}

static void dev_grab(evdev_t *d, bool on)
{
    if (ioctl(d->fd, EVIOCGRAB, on ? 1 : 0) < 0)
        logv("input: %s grab(%d): %s", d->name, on, strerror(errno));
}

static void add_dev(input_t *in, const char *name)
{
    char path[PATH_MAX + 32];
    int fd;
    if (strncmp(name, "event", 5) != 0 || has_dev(in, name) || in->ndev >= MAX_DEVS)
        return;
    snprintf(path, sizeof path, "%s/%s", in->dir, name);
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return; /* permissions may not be set yet; IN_ATTRIB retries */
    if (!is_keyboard(fd)) {
        close(fd);
        return;
    }
    in->dev[in->ndev].fd = fd;
    str_copy(in->dev[in->ndev].name, sizeof in->dev[in->ndev].name, name);
    if (in->grab)
        dev_grab(&in->dev[in->ndev], true);
    in->ndev++;
    logv("input: keyboard %s", path);
}

static void remove_dev(input_t *in, int i)
{
    logv("input: keyboard %s gone", in->dev[i].name);
    close(in->dev[i].fd);
    in->dev[i] = in->dev[--in->ndev];
    in->mods = 0; /* we may have missed its releases */
}

static void scan(input_t *in)
{
    DIR *dir = opendir(in->dir);
    struct dirent *e;
    if (!dir)
        return;
    while ((e = readdir(dir)) != NULL)
        add_dev(in, e->d_name);
    closedir(dir);
}

input_t *input_open(const char *dir, input_key_fn fn, void *user)
{
    input_t *in = calloc(1, sizeof *in);
    if (!in)
        return NULL;
    str_copy(in->dir, sizeof in->dir, dir);
    in->fn = fn;
    in->user = user;
    in->grab = true;
    in->ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (in->ino_fd >= 0 &&
        inotify_add_watch(in->ino_fd, dir, IN_CREATE | IN_ATTRIB | IN_MOVED_TO) < 0) {
        logv("input: inotify on %s: %s (polling instead)", dir, strerror(errno));
        close(in->ino_fd);
        in->ino_fd = -1;
    }
    scan(in);
    in->last_scan_ms = mono_ms();
    return in;
}

void input_close(input_t *in)
{
    if (!in)
        return;
    input_set_grab(in, false);
    while (in->ndev)
        remove_dev(in, in->ndev - 1);
    if (in->ino_fd >= 0)
        close(in->ino_fd);
    free(in);
}

int input_pollfds(input_t *in, struct pollfd *pfd, int max)
{
    int n = 0, i;
    if (in->ino_fd >= 0 && n < max) {
        pfd[n].fd = in->ino_fd;
        pfd[n].events = POLLIN;
        pfd[n++].revents = 0;
    }
    for (i = 0; i < in->ndev && n < max; i++) {
        pfd[n].fd = in->dev[i].fd;
        pfd[n].events = POLLIN;
        pfd[n++].revents = 0;
    }
    return n;
}

static uint8_t mod_bit(uint8_t usage)
{
    return usage >= 0xE0 && usage <= 0xE7 ? (uint8_t)(1u << (usage - 0xE0)) : 0;
}

static void key_event(input_t *in, unsigned code, int value)
{
    uint8_t usage = input_key_to_hid(code), bit;
    if (!usage || value < 0 || value > 2)
        return;
    bit = mod_bit(usage);
    if (bit) {
        if (value)
            in->mods |= bit;
        else
            in->mods &= (uint8_t)~bit;
    }
    in->fn(in->user, usage, in->mods, value);
}

/* Drain one device; false if it disappeared. */
static bool read_dev(input_t *in, evdev_t *d)
{
    struct input_event ev[32];
    for (;;) {
        ssize_t n = read(d->fd, ev, sizeof ev);
        size_t i;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno == EAGAIN;
        }
        if (n == 0)
            return false;
        for (i = 0; i < (size_t)n / sizeof ev[0]; i++)
            if (ev[i].type == EV_KEY)
                key_event(in, ev[i].code, ev[i].value);
    }
}

static void read_inotify(input_t *in)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(in->ino_fd, buf, sizeof buf);
        char *p;
        if (n <= 0)
            return;
        for (p = buf; p < buf + n;) {
            const struct inotify_event *e = (const struct inotify_event *)p;
            if (e->len)
                add_dev(in, e->name);
            p += sizeof *e + e->len;
        }
    }
}

void input_handle(input_t *in, const struct pollfd *pfd, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        if (!pfd[i].revents)
            continue;
        if (pfd[i].fd == in->ino_fd) {
            read_inotify(in);
            continue;
        }
        for (j = 0; j < in->ndev; j++) {
            if (in->dev[j].fd != pfd[i].fd)
                continue;
            if (!read_dev(in, &in->dev[j]) || (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)))
                remove_dev(in, j);
            break;
        }
    }
}

void input_tick(input_t *in, uint64_t now_ms)
{
    if (in->ino_fd < 0 && now_ms - in->last_scan_ms >= RESCAN_MS) {
        in->last_scan_ms = now_ms;
        scan(in);
    }
}

void input_set_grab(input_t *in, bool grab)
{
    int i;
    if (in->grab == grab)
        return;
    in->grab = grab;
    for (i = 0; i < in->ndev; i++)
        dev_grab(&in->dev[i], grab);
    in->mods = 0;
}
