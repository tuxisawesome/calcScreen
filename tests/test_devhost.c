/*
 * test_devhost.c - device engine <-> host engine loopback.
 *
 * A fake device HAL (core/dock_dev_hal.h) and a fake host HAL
 * (core/dock_host_hal.h) share one "world". The device's transmit is modelled
 * faithfully: one transfer in flight, completed (and only then copied to the
 * host) a few service calls later, so a device that touches a buffer while it
 * is still being sent corrupts what the host sees. dock_dhal_service() is the
 * world's clock tick: it advances time by 1 ms and completes transfers, which
 * runs the host engine; host replies are queued back to the device.
 */
#include "../core/dock_api.h"
#include "../core/dock_host.h"
#include "test.h"

/* ---------------------------------------------------------------- world */

#define TO_DEV_CAP (1024u * 1024u)
#define NFILES 8
#define FILE_CAP 60000u
#define NHANDLES 4

typedef struct fake_file {
    bool used, dir;
    char name[300];
    uint8_t data[FILE_CAP];
    size_t len;
} fake_file_t;

typedef struct world {
    /* link + clock */
    bool link_up;
    uint32_t ms;
    uint32_t rng;
    /* device transmit, one in flight */
    const uint8_t *tx_p;
    size_t tx_n;
    bool tx_last, tx_busy;
    int tx_delay, tx_delay_max;
    size_t tx_max;
    uint32_t tx_calls, last_violations, tx_refused;
    /* host -> device bytes */
    uint8_t to_dev[TO_DEV_CAP];
    size_t td_r, td_w;
    bool mute; /* drop host replies */
    /* device framebuffer */
    dock_fb_t fb;
    bool has_fb;
    /* host side */
    dock_host_t *host;
    int events[256];
    int nevents;
    bool join_required;
    char join_code[6];
    uint32_t dock_id;
    uint16_t caps;
    char log[160];
    uint32_t damage_rows, damage_calls;
    uint16_t mode_w, mode_h;
    /* audio */
    uint32_t aud_rate;
    uint8_t aud_fmt, aud_ch, volume;
    size_t aud_rx;
    char play_path[128];
    bool aud_stopped;
    /* files */
    fake_file_t files[NFILES];
    struct {
        bool used;
        int file;
        uint32_t pos;
        uint8_t mode;
    } h[NHANDLES];
    /* http */
    uint8_t http_method;
    char http_url[128];
    char http_headers[64];
    uint32_t http_body_len;
    uint8_t put_buf[8192];
    size_t put_len;
    uint8_t get_body[4000];
    size_t get_len, get_pos;
    bool drop_link_on_http_read;
} world_t;

static world_t W;
static dock_host_t host;
static uint8_t host_rx[DOCK_MAX_PAYLOAD];
static uint8_t host_resp[70000];
static uint32_t host_fb[320 * 240];
static uint8_t dev_fb[240 * 640];
static dock_dev_t dev;

/* ---------------------------------------------------- device HAL (fake) */

void dock_dhal_service(void *hal)
{
    world_t *w = (world_t *)hal;
    w->ms++;
    if (!w->tx_busy)
        return;
    if (w->tx_delay > 0) {
        w->tx_delay--;
        return;
    }
    /* The transfer completes now: this is when the bytes are read. */
    w->tx_busy = false;
    dock_host_feed(w->host, w->tx_p, w->tx_n);
    /* `last` must close a frame exactly. */
    if (w->tx_last && (w->host->rx.in_payload || w->host->rx.hdr_have != 0))
        w->last_violations++;
}

bool dock_dhal_link_up(void *hal)
{
    return ((world_t *)hal)->link_up;
}

bool dock_dhal_tx(void *hal, const void *p, size_t n, bool last)
{
    world_t *w = (world_t *)hal;
    if (!w->link_up || w->tx_busy || n == 0 || n > w->tx_max) {
        w->tx_refused++;
        return false;
    }
    w->tx_p = (const uint8_t *)p;
    w->tx_n = n;
    w->tx_last = last;
    w->tx_busy = true;
    w->tx_delay = w->tx_delay_max ? (int)(t_rand(&w->rng) % (uint32_t)(w->tx_delay_max + 1)) : 0;
    w->tx_calls++;
    return true;
}

bool dock_dhal_tx_idle(void *hal)
{
    return !((world_t *)hal)->tx_busy;
}

size_t dock_dhal_tx_max(void *hal)
{
    return ((world_t *)hal)->tx_max;
}

size_t dock_dhal_rx(void *hal, uint8_t *buf, size_t max)
{
    world_t *w = (world_t *)hal;
    size_t n = w->td_w - w->td_r;
    if (!w->link_up)
        return 0;
    if (n > max)
        n = max;
    memcpy(buf, w->to_dev + w->td_r, n);
    w->td_r += n;
    if (w->td_r == w->td_w)
        w->td_r = w->td_w = 0;
    return n;
}

uint32_t dock_dhal_ms(void *hal)
{
    return ((world_t *)hal)->ms;
}

bool dock_dhal_fb(void *hal, dock_fb_t *fb)
{
    world_t *w = (world_t *)hal;
    *fb = w->fb;
    return w->has_fb;
}

uint32_t dock_dhal_row_hash(const uint8_t *row, size_t n)
{
    return dock_row_hash_c(row, n);
}

/* ------------------------------------------------------ host HAL (fake) */

bool dock_hhal_tx(void *hal, const void *p, size_t n)
{
    world_t *w = (world_t *)hal;
    if (!w->link_up)
        return false;
    if (w->mute)
        return true;
    if (w->td_w + n > TO_DEV_CAP) {
        fprintf(stderr, "to_dev pipe overflow\n");
        return false;
    }
    memcpy(w->to_dev + w->td_w, p, n);
    w->td_w += n;
    return true;
}

void dock_hhal_event(void *hal, int event)
{
    world_t *w = (world_t *)hal;
    if (w->nevents < 256)
        w->events[w->nevents++] = event;
}

int dock_hhal_join_check(void *hal, uint32_t calc_id, const char code[6])
{
    world_t *w = (world_t *)hal;
    static const char none[6] = {0, 0, 0, 0, 0, 0};
    (void)calc_id;
    if (!w->join_required)
        return DOCK_OK;
    if (memcmp(code, none, 6) == 0)
        return DOCK_E_JOIN_REQUIRED;
    return memcmp(code, w->join_code, 6) == 0 ? DOCK_OK : DOCK_E_JOIN_BAD;
}

uint32_t dock_hhal_dock_id(void *hal)
{
    return ((world_t *)hal)->dock_id;
}

uint16_t dock_hhal_caps(void *hal)
{
    return ((world_t *)hal)->caps;
}

void dock_hhal_log(void *hal, const char *text)
{
    world_t *w = (world_t *)hal;
    snprintf(w->log, sizeof w->log, "%s", text);
}

void dock_hhal_display_damage(void *hal, uint16_t y0, uint16_t count)
{
    world_t *w = (world_t *)hal;
    (void)y0;
    w->damage_rows += count;
    w->damage_calls++;
}

void dock_hhal_display_mode(void *hal, uint16_t width, uint16_t height)
{
    world_t *w = (world_t *)hal;
    w->mode_w = width;
    w->mode_h = height;
}

int dock_hhal_audio_open(void *hal, uint32_t rate, uint8_t fmt, uint8_t channels)
{
    world_t *w = (world_t *)hal;
    w->aud_rate = rate;
    w->aud_fmt = fmt;
    w->aud_ch = channels;
    return DOCK_OK;
}

int dock_hhal_audio_write(void *hal, const uint8_t *pcm, size_t n, size_t *accepted)
{
    world_t *w = (world_t *)hal;
    size_t i;
    for (i = 0; i < n; i++)
        if (pcm[i] != (uint8_t)(i * 3u))
            return DOCK_E_INVALID;
    w->aud_rx += n;
    *accepted = n < 1000 ? n : 1000;
    return DOCK_OK;
}

int dock_hhal_audio_play_file(void *hal, const char *path)
{
    world_t *w = (world_t *)hal;
    snprintf(w->play_path, sizeof w->play_path, "%s", path);
    return DOCK_OK;
}

int dock_hhal_audio_pos(void *hal, uint32_t *pos_ms, bool *playing)
{
    (void)hal;
    *pos_ms = 1234;
    *playing = true;
    return DOCK_OK;
}

int dock_hhal_audio_stop(void *hal)
{
    ((world_t *)hal)->aud_stopped = true;
    return DOCK_OK;
}

int dock_hhal_audio_volume(void *hal, uint8_t percent)
{
    ((world_t *)hal)->volume = percent;
    return DOCK_OK;
}

static int find_file(world_t *w, const char *path)
{
    int i;
    for (i = 0; i < NFILES; i++)
        if (w->files[i].used && strcmp(w->files[i].name, path) == 0)
            return i;
    return -1;
}

static int new_file(world_t *w, const char *path, bool dir)
{
    int i;
    for (i = 0; i < NFILES; i++) {
        if (!w->files[i].used) {
            memset(&w->files[i], 0, sizeof w->files[i]);
            w->files[i].used = true;
            w->files[i].dir = dir;
            snprintf(w->files[i].name, sizeof w->files[i].name, "%s", path);
            return i;
        }
    }
    return -1;
}

int dock_hhal_fs_drive(void *hal, uint8_t index, char *name, size_t cap)
{
    (void)hal;
    if (index > 0)
        return DOCK_E_END;
    snprintf(name, cap, "USB1");
    return DOCK_OK;
}

int dock_hhal_fs_open(void *hal, const char *path, uint8_t mode, uint8_t *handle)
{
    world_t *w = (world_t *)hal;
    int f = find_file(w, path), i;
    if (f < 0 && (mode & DOCK_FS_CREATE))
        f = new_file(w, path, false);
    if (f < 0)
        return DOCK_E_NOT_FOUND;
    if (w->files[f].dir)
        return DOCK_E_INVALID;
    for (i = 0; i < NHANDLES; i++) {
        if (!w->h[i].used) {
            w->h[i].used = true;
            w->h[i].file = f;
            w->h[i].pos = (mode & DOCK_FS_APPEND) ? (uint32_t)w->files[f].len : 0;
            w->h[i].mode = mode;
            if (mode & DOCK_FS_TRUNC)
                w->files[f].len = 0;
            *handle = (uint8_t)(i + 10);
            return DOCK_OK;
        }
    }
    return DOCK_E_BUSY;
}

static int handle_index(world_t *w, uint8_t handle)
{
    int i = (int)handle - 10;
    return (i >= 0 && i < NHANDLES && w->h[i].used) ? i : -1;
}

int dock_hhal_fs_read(void *hal, uint8_t handle, uint8_t *dst, size_t max, size_t *got)
{
    world_t *w = (world_t *)hal;
    int i = handle_index(w, handle);
    fake_file_t *f;
    size_t n;
    if (i < 0)
        return DOCK_E_INVALID;
    f = &w->files[w->h[i].file];
    if (w->h[i].pos >= f->len) {
        *got = 0;
        return DOCK_E_END;
    }
    n = f->len - w->h[i].pos;
    if (n > max)
        n = max;
    memcpy(dst, f->data + w->h[i].pos, n);
    w->h[i].pos += (uint32_t)n;
    *got = n;
    return DOCK_OK;
}

int dock_hhal_fs_write(void *hal, uint8_t handle, const uint8_t *src, size_t n,
                       size_t *written)
{
    world_t *w = (world_t *)hal;
    int i = handle_index(w, handle);
    fake_file_t *f;
    if (i < 0 || !(w->h[i].mode & DOCK_FS_WRITE))
        return DOCK_E_INVALID;
    f = &w->files[w->h[i].file];
    if (w->h[i].pos + n > FILE_CAP)
        n = FILE_CAP - w->h[i].pos;
    memcpy(f->data + w->h[i].pos, src, n);
    w->h[i].pos += (uint32_t)n;
    if (w->h[i].pos > f->len)
        f->len = w->h[i].pos;
    *written = n;
    return DOCK_OK;
}

int dock_hhal_fs_seek(void *hal, uint8_t handle, int32_t offset, uint8_t whence,
                      uint32_t *pos)
{
    world_t *w = (world_t *)hal;
    int i = handle_index(w, handle);
    long base, np;
    if (i < 0)
        return DOCK_E_INVALID;
    base = whence == DOCK_SEEK_SET ? 0
           : whence == DOCK_SEEK_CUR ? (long)w->h[i].pos
                                     : (long)w->files[w->h[i].file].len;
    np = base + offset;
    if (np < 0)
        return DOCK_E_INVALID;
    w->h[i].pos = (uint32_t)np;
    *pos = w->h[i].pos;
    return DOCK_OK;
}

int dock_hhal_fs_close(void *hal, uint8_t handle)
{
    world_t *w = (world_t *)hal;
    int i = handle_index(w, handle);
    if (i < 0)
        return DOCK_E_INVALID;
    w->h[i].used = false;
    return DOCK_OK;
}

int dock_hhal_fs_stat(void *hal, const char *path, uint32_t *size, bool *is_dir)
{
    world_t *w = (world_t *)hal;
    int f = find_file(w, path);
    if (f < 0)
        return DOCK_E_NOT_FOUND;
    *size = (uint32_t)w->files[f].len;
    *is_dir = w->files[f].dir;
    return DOCK_OK;
}

int dock_hhal_fs_readdir(void *hal, const char *path, uint16_t index, char *name,
                         size_t cap, uint32_t *size, bool *is_dir)
{
    world_t *w = (world_t *)hal;
    size_t pl = strlen(path);
    int i;
    uint16_t k = 0;
    for (i = 0; i < NFILES; i++) {
        const fake_file_t *f = &w->files[i];
        if (!f->used || strncmp(f->name, path, pl) != 0 || f->name[pl] != '/' ||
            strchr(f->name + pl + 1, '/'))
            continue;
        if (k++ == index) {
            snprintf(name, cap, "%s", f->name + pl + 1);
            *size = (uint32_t)f->len;
            *is_dir = f->dir;
            return DOCK_OK;
        }
    }
    return DOCK_E_END;
}

int dock_hhal_fs_mkdir(void *hal, const char *path)
{
    world_t *w = (world_t *)hal;
    if (find_file(w, path) >= 0)
        return DOCK_E_INVALID;
    return new_file(w, path, true) >= 0 ? DOCK_OK : DOCK_E_NO_SPACE;
}

int dock_hhal_fs_remove(void *hal, const char *path)
{
    world_t *w = (world_t *)hal;
    int f = find_file(w, path);
    if (f < 0)
        return DOCK_E_NOT_FOUND;
    w->files[f].used = false;
    return DOCK_OK;
}

int dock_hhal_usb_device(void *hal, uint8_t index, uint16_t *vid, uint16_t *pid,
                         uint8_t *dev_class, char *name, size_t cap)
{
    (void)hal;
    if (index > 0)
        return DOCK_E_END;
    *vid = 0x0781;
    *pid = 0x5567;
    *dev_class = 8;
    snprintf(name, cap, "SanDisk Cruzer Blade");
    return DOCK_OK;
}

int dock_hhal_net_status(void *hal, bool *up, char *ssid, size_t ssid_cap,
                         char *ip, size_t ip_cap)
{
    (void)hal;
    *up = true;
    snprintf(ssid, ssid_cap, "home-net");
    snprintf(ip, ip_cap, "192.168.1.42");
    return DOCK_OK;
}

int dock_hhal_http_begin(void *hal, uint8_t method, const char *url,
                         const char *headers, uint32_t body_len, uint8_t *handle)
{
    world_t *w = (world_t *)hal;
    if (strncmp(url, "http", 4) != 0)
        return DOCK_E_INVALID;
    w->http_method = method;
    snprintf(w->http_url, sizeof w->http_url, "%s", url);
    snprintf(w->http_headers, sizeof w->http_headers, "%s", headers);
    w->http_body_len = body_len;
    w->put_len = 0;
    w->get_pos = 0;
    *handle = 3;
    return DOCK_OK;
}

int dock_hhal_http_body(void *hal, uint8_t handle, const uint8_t *data, size_t n)
{
    world_t *w = (world_t *)hal;
    if (handle != 3 || w->put_len + n > sizeof w->put_buf)
        return DOCK_E_INVALID;
    memcpy(w->put_buf + w->put_len, data, n);
    w->put_len += n;
    return DOCK_OK;
}

int dock_hhal_http_response(void *hal, uint8_t handle, uint16_t *http_status,
                            uint32_t *content_len)
{
    world_t *w = (world_t *)hal;
    if (handle != 3)
        return DOCK_E_INVALID;
    *http_status = w->http_method == DOCK_HTTP_PUT ? 201 : 200;
    *content_len = w->http_method == DOCK_HTTP_PUT ? 0 : (uint32_t)w->get_len;
    return DOCK_OK;
}

int dock_hhal_http_read(void *hal, uint8_t handle, uint8_t *dst, size_t max,
                        size_t *got)
{
    world_t *w = (world_t *)hal;
    size_t n = w->get_len - w->get_pos;
    if (handle != 3)
        return DOCK_E_INVALID;
    if (w->drop_link_on_http_read) {
        w->link_up = false;
        return DOCK_E_IO;
    }
    if (n > 700)
        n = 700; /* the network delivers in pieces */
    if (n > max)
        n = max;
    memcpy(dst, w->get_body + w->get_pos, n);
    w->get_pos += n;
    *got = n;
    return DOCK_OK;
}

int dock_hhal_http_end(void *hal, uint8_t handle)
{
    (void)hal;
    return handle == 3 ? DOCK_OK : DOCK_E_INVALID;
}

/* -------------------------------------------------------------- helpers */

static void fill_fb(uint32_t seed)
{
    size_t i;
    for (i = 0; i < sizeof dev_fb; i++)
        dev_fb[i] = (uint8_t)t_rand(&seed);
}

static void world_init(bool join_required, size_t tx_max, int tx_delay_max)
{
    int f;
    size_t i;
    memset(&W, 0, sizeof W);
    W.rng = 0x1234567u;
    W.link_up = true;
    W.ms = 1000;
    W.tx_max = tx_max;
    W.tx_delay_max = tx_delay_max;
    W.join_required = join_required;
    memcpy(W.join_code, "123456", 6);
    W.dock_id = 0xD0C1D0C1u;
    W.caps = DOCK_CAP_AUDIO | DOCK_CAP_FS | DOCK_CAP_USB | DOCK_CAP_NET | DOCK_CAP_KEYS;
    W.host = &host;
    fill_fb(42);
    W.fb.pixels = dev_fb;
    W.fb.width = 320;
    W.fb.height = 240;
    W.fb.stride = 640;
    W.fb.fmt = DOCK_PIX_RGB565;
    W.fb.flags = DOCK_SCRF_BGR;
    W.fb.palette_le = NULL;
    W.has_fb = true;
    /* files */
    f = new_file(&W, "USB1/hello.txt", false);
    memcpy(W.files[f].data, "hello world", 11);
    W.files[f].len = 11;
    f = new_file(&W, "USB1/big.bin", false);
    for (i = 0; i < 50000; i++)
        W.files[f].data[i] = (uint8_t)(i ^ (i >> 8));
    W.files[f].len = 50000;
    new_file(&W, "USB1/subdir", true);
    {
        /* a 250-character file name (FAT long names go up to 255) */
        char longname[5 + 250 + 1];
        memcpy(longname, "USB1/a_rather_long", 18);
        memset(longname + 18, 'x', 5 + 250 - 18);
        longname[5 + 250] = '\0';
        f = new_file(&W, longname, false);
        W.files[f].len = 7;
    }
    for (i = 0; i < sizeof W.get_body; i++)
        W.get_body[i] = (uint8_t)(i * 7u + 3u);
    W.get_len = 3000;

    memset(host_fb, 0, sizeof host_fb);
    dock_host_init(&host, &W, host_rx, sizeof host_rx, host_resp, sizeof host_resp,
                   host_fb, 320, 240, 320);
    dock_dev_init(&dev, &W, 0xCA1C0001u);
}

static bool poll_until_state(uint8_t state, int max)
{
    while (max-- > 0) {
        dock_dev_poll(&dev);
        if (dev.state == state)
            return true;
    }
    return false;
}

static bool fb_equal(void)
{
    uint32_t x, y;
    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            if (host_fb[y * 320 + x] !=
                dock_color16_to_xrgb((uint16_t)(dev_fb[y * 640 + 2 * x] |
                                                (dev_fb[y * 640 + 2 * x + 1] << 8)),
                                     DOCK_PIX_RGB565, DOCK_SCRF_BGR))
                return false;
    return true;
}

static int count_events(int ev)
{
    int i, n = 0;
    for (i = 0; i < W.nevents; i++)
        n += W.events[i] == ev;
    return n;
}

/* Poll until the host mirror equals the device framebuffer and stays so. */
static bool mirror_converges(int max)
{
    int i;
    for (i = 0; i < max; i++) {
        dock_dev_poll(&dev);
        if ((i & 63) == 0 && fb_equal() && !dev.tx_busy &&
            !dock_scr_tx_any_dirty(&dev.scr) && dev.pass_done)
            return true;
    }
    return fb_equal();
}

static void connect(void)
{
    CHECK(poll_until_state(DOCK_DEV_RUNNING, 200));
}

/* ---------------------------------------------------------------- tests */

static void t_hello_welcome(void)
{
    world_init(false, 1000, 2);
    dev.hello_flags = DOCK_HELLOF_RESIDENT;
    dev.driver_ver = 0x0102;
    connect();
    CHECK(host.accepted);
    CHECK_EQ(host.calc_id, 0xCA1C0001u);
    CHECK_EQ(host.hello_flags, DOCK_HELLOF_RESIDENT);
    CHECK_EQ(host.driver_ver, 0x0102);
    CHECK_EQ(dev.dock_id, W.dock_id);
    CHECK_EQ(dev.host_caps, W.caps);
    CHECK_EQ(dev.last_welcome_status, DOCK_OK);
    CHECK_EQ(count_events(DOCK_HEV_CONNECTED), 1);
    CHECK_EQ(dock_status(&dev), DOCK_OK);
    CHECK_EQ(dev.hellos_sent, 1);
    CHECK_EQ(host.bad_frames, 0);

    /* No WELCOME (host silent): HELLO is retried once a second. */
    world_init(false, 1000, 0);
    W.mute = true;
    {
        int i;
        for (i = 0; i < 2500; i++)
            dock_dev_poll(&dev);
    }
    CHECK_EQ(dev.state, DOCK_DEV_HELLO);
    CHECK_EQ(dev.hellos_sent, 3);
    W.mute = false;
    CHECK(poll_until_state(DOCK_DEV_RUNNING, 1100)); /* next retry */
}

static void t_screen_mirror(void)
{
    uint32_t rows_before;
    world_init(false, 1000, 2);
    connect();
    CHECK(mirror_converges(20000));
    CHECK(count_events(DOCK_HEV_FRAME) >= 1);
    CHECK_EQ(W.mode_w, 320);
    CHECK_EQ(W.mode_h, 240);
    CHECK(W.damage_rows >= 240);
    CHECK_EQ(W.last_violations, 0);
    CHECK_EQ(host.bad_frames, 0);

    /* Change a few rows: only those travel. */
    rows_before = W.damage_rows;
    dev_fb[7 * 640 + 10] ^= 0xFF;
    dev_fb[8 * 640 + 639] ^= 0x01;
    dev_fb[100 * 640 + 320] ^= 0x42;
    dev_fb[239 * 640] ^= 0x80;
    CHECK(!fb_equal());
    CHECK(mirror_converges(5000));
    CHECK(W.damage_rows - rows_before <= 8);
    CHECK(W.damage_rows - rows_before >= 4);

    /* Everything changes. */
    fill_fb(7);
    CHECK(mirror_converges(20000));

    /* The host asks for a refresh: all rows are resent. */
    rows_before = W.damage_rows;
    CHECK(dock_host_request_refresh(&host));
    CHECK(mirror_converges(20000));
    {
        int i;
        for (i = 0; i < 2000; i++)
            dock_dev_poll(&dev);
    }
    CHECK(W.damage_rows - rows_before >= 240);
    CHECK_EQ(W.last_violations, 0);
    CHECK_EQ(W.tx_refused, 0);
}

static void t_screen_stress(void)
{
    /* Tiny transfers with random completion delays. */
    world_init(false, 64, 5);
    connect();
    CHECK(mirror_converges(200000));
    dev_fb[120 * 640 + 7] ^= 0x33;
    CHECK(mirror_converges(20000));
    CHECK_EQ(W.last_violations, 0);
    CHECK_EQ(host.bad_frames, 0);

    /* An 8 bpp palettised mode with padded rows (stride != row bytes). */
    {
        static uint8_t fb8[240 * 336];
        static uint8_t pal[512];
        uint32_t seed = 5, x, y;
        size_t i;
        bool ok = true;
        world_init(false, 1000, 1);
        for (i = 0; i < sizeof fb8; i++)
            fb8[i] = (uint8_t)t_rand(&seed);
        for (i = 0; i < 256; i++)
            dock_put_u16(pal + 2 * i, (uint16_t)(i * 0x0421u));
        W.fb.pixels = fb8;
        W.fb.stride = 336;
        W.fb.fmt = DOCK_PIX_I8;
        W.fb.flags = 0;
        W.fb.palette_le = pal;
        connect();
        for (i = 0; i < 30000; i++)
            dock_dev_poll(&dev);
        for (y = 0; y < 240 && ok; y++)
            for (x = 0; x < 320 && ok; x++)
                ok = host_fb[y * 320 + x] ==
                     dock_color16_to_xrgb((uint16_t)(fb8[y * 336 + x] * 0x0421u),
                                          DOCK_PIX_RGB1555, 0);
        CHECK(ok);
        CHECK_EQ(host.bad_frames, 0);
    }
}

static void t_keys(void)
{
    dock_key_t k;
    dock_hidkey_t hk;
    int i;
    world_init(false, 1000, 1);
    connect();
    CHECK(dock_host_send_key(&host, 0xFCE2, 0x2F, 0));
    CHECK(dock_host_send_key(&host, 0, 0, DOCK_KEYF_ON));
    CHECK(dock_host_send_hidkey(&host, 0x04, 0x02, true, 'A'));
    for (i = 0; i < 5; i++)
        dock_service(&dev); /* public API spelling of dock_dev_poll */
    CHECK(dock_dev_key_pop(&dev, &k));
    CHECK_EQ(k.tikey, 0xFCE2);
    CHECK_EQ(k.scancode, 0x2F);
    CHECK_EQ(k.flags, 0);
    CHECK(dock_dev_key_pop(&dev, &k));
    CHECK_EQ(k.flags, DOCK_KEYF_ON);
    CHECK(!dock_dev_key_pop(&dev, &k));
    CHECK(dock_key_get(&dev, &hk));
    CHECK_EQ(hk.usage, 0x04);
    CHECK_EQ(hk.mods, 0x02);
    CHECK_EQ(hk.down, 1);
    CHECK_EQ(hk.ascii, 'A');
    CHECK(!dock_key_get(&dev, &hk));

    /* Overflow drops the excess, keeps order. */
    for (i = 0; i < 20; i++)
        dock_host_send_key(&host, (uint16_t)(0x80 + i), 0, 0);
    for (i = 0; i < 10; i++)
        dock_dev_poll(&dev);
    for (i = 0; i < 15; i++) {
        CHECK(dock_dev_key_pop(&dev, &k));
        CHECK_EQ(k.tikey, 0x80 + i);
    }
    CHECK(!dock_dev_key_pop(&dev, &k));

    /* Ping is answered with PONG. */
    {
        uint32_t frames = host.frames_rx;
        /* The screen is streaming: the PONG waits for the pipe. */
        CHECK(dev.tx_busy);
        CHECK(dock_host_ping(&host));
        for (i = 0; i < 1000 && host.frames_rx == frames; i++)
            dock_dev_poll(&dev);
        CHECK(host.frames_rx > frames);
        CHECK(!dev.pong_pending);
        CHECK_EQ(host.bad_frames, 0);
    }
}

static uint8_t big[60000];

static void t_api_audio_usb_net(void)
{
    static uint8_t pcm[3000];
    size_t acc = 0, i;
    uint32_t pos = 0;
    bool playing = false, up = false;
    dock_usb_info_t info;
    char ssid[40], ip[40];
    world_init(false, 1000, 1);
    connect();

    CHECK_EQ(dock_audio_open(&dev, 22050, DOCK_AUD_S16LE, 1), DOCK_OK);
    CHECK_EQ(W.aud_rate, 22050);
    CHECK_EQ(W.aud_fmt, DOCK_AUD_S16LE);
    CHECK_EQ(W.aud_ch, 1);
    for (i = 0; i < sizeof pcm; i++)
        pcm[i] = (uint8_t)(i * 3u);
    CHECK_EQ(dock_audio_write(&dev, pcm, sizeof pcm, &acc), DOCK_OK);
    CHECK_EQ(acc, 1000);
    CHECK_EQ(W.aud_rx, 3000);
    CHECK_EQ(dock_audio_play_file(&dev, "USB1/badapple.wav"), DOCK_OK);
    CHECK_STR(W.play_path, "USB1/badapple.wav");
    CHECK_EQ(dock_audio_pos(&dev, &pos, &playing), DOCK_OK);
    CHECK_EQ(pos, 1234);
    CHECK(playing);
    CHECK_EQ(dock_audio_stop(&dev), DOCK_OK);
    CHECK(W.aud_stopped);
    CHECK_EQ(dock_audio_volume(&dev, 150), DOCK_OK);
    CHECK_EQ(W.volume, 100);

    CHECK_EQ(dock_usb_device(&dev, 0, &info), DOCK_OK);
    CHECK_EQ(info.vid, 0x0781);
    CHECK_EQ(info.pid, 0x5567);
    CHECK_EQ(info.dev_class, 8);
    CHECK_STR(info.name, "SanDisk Cruzer Blade");
    CHECK_EQ(dock_usb_device(&dev, 1, &info), DOCK_E_END);

    CHECK_EQ(dock_net_status(&dev, &up, ssid, sizeof ssid, ip, sizeof ip), DOCK_OK);
    CHECK(up);
    CHECK_STR(ssid, "home-net");
    CHECK_STR(ip, "192.168.1.42");
    CHECK_EQ(dock_net_status(&dev, &up, ssid, 5, ip, 4), DOCK_OK);
    CHECK_STR(ssid, "home");
    CHECK_STR(ip, "192");
    CHECK_EQ(host.bad_frames, 0);
    CHECK(!dev.req_active);
}

static void t_api_fs(void)
{
    char name[300];
    uint8_t h = 0, h2 = 0;
    size_t got = 0, wr = 0, i;
    uint32_t pos = 0, size = 0;
    bool dir = true;
    int rc, seen = 0;
    world_init(false, 1000, 1);
    connect();

    CHECK_EQ(dock_fs_drive(&dev, 0, name, sizeof name), DOCK_OK);
    CHECK_STR(name, "USB1");
    CHECK_EQ(dock_fs_drive(&dev, 0, name, 3), DOCK_OK);
    CHECK_STR(name, "US");
    CHECK_EQ(dock_fs_drive(&dev, 1, name, sizeof name), DOCK_E_END);

    /* A large read lands straight in the caller's buffer (response tail). */
    CHECK_EQ(dock_fs_open(&dev, "USB1/big.bin", DOCK_FS_READ, &h), DOCK_OK);
    CHECK_EQ(h, 10);
    memset(big, 0, sizeof big);
    CHECK_EQ(dock_fs_read(&dev, h, big, sizeof big, &got), DOCK_OK);
    CHECK_EQ(got, 50000);
    {
        bool ok = true;
        for (i = 0; i < 50000; i++)
            ok = ok && big[i] == (uint8_t)(i ^ (i >> 8));
        CHECK(ok);
        CHECK_EQ(big[50000], 0); /* nothing beyond what was read */
    }
    CHECK_EQ(dock_fs_read(&dev, h, big, sizeof big, &got), DOCK_E_END);
    CHECK_EQ(got, 0);
    CHECK_EQ(dock_fs_seek(&dev, h, -10, DOCK_SEEK_END, &pos), DOCK_OK);
    CHECK_EQ(pos, 49990);
    CHECK_EQ(dock_fs_seek(&dev, h, 1000, DOCK_SEEK_SET, &pos), DOCK_OK);
    CHECK_EQ(pos, 1000);
    CHECK_EQ(dock_fs_seek(&dev, h, 24, DOCK_SEEK_CUR, &pos), DOCK_OK);
    CHECK_EQ(pos, 1024);
    CHECK_EQ(dock_fs_read(&dev, h, big, 100, &got), DOCK_OK);
    CHECK_EQ(got, 100);
    CHECK_EQ(big[0], (uint8_t)(1024 ^ 4));
    CHECK_EQ(dock_fs_seek(&dev, h, -5000, DOCK_SEEK_SET, &pos), DOCK_E_INVALID);
    CHECK_EQ(pos, 0);
    CHECK_EQ(dock_fs_close(&dev, h), DOCK_OK);
    CHECK_EQ(dock_fs_close(&dev, h), DOCK_E_INVALID);

    CHECK_EQ(dock_fs_open(&dev, "USB1/missing.txt", DOCK_FS_READ, &h), DOCK_E_NOT_FOUND);
    CHECK_EQ(h, 0xFF);

    /* Write a new file and read it back. */
    for (i = 0; i < 5000; i++)
        big[i] = (uint8_t)(i * 11u);
    CHECK_EQ(dock_fs_open(&dev, "USB1/new.txt", DOCK_FS_WRITE | DOCK_FS_CREATE, &h),
             DOCK_OK);
    CHECK_EQ(dock_fs_write(&dev, h, big, 5000, &wr), DOCK_OK);
    CHECK_EQ(wr, 5000);
    CHECK_EQ(dock_fs_close(&dev, h), DOCK_OK);
    CHECK_EQ(dock_fs_stat(&dev, "USB1/new.txt", &size, &dir), DOCK_OK);
    CHECK_EQ(size, 5000);
    CHECK(!dir);
    CHECK_EQ(dock_fs_open(&dev, "USB1/new.txt", DOCK_FS_READ, &h2), DOCK_OK);
    memset(big + 10000, 0, 6000);
    CHECK_EQ(dock_fs_read(&dev, h2, big + 10000, 6000, &got), DOCK_OK);
    CHECK_EQ(got, 5000);
    CHECK_MEM(big + 10000, big, 5000);
    CHECK_EQ(dock_fs_close(&dev, h2), DOCK_OK);
    /* writing to a read-only handle fails cleanly */
    CHECK_EQ(dock_fs_open(&dev, "USB1/hello.txt", DOCK_FS_READ, &h), DOCK_OK);
    CHECK_EQ(dock_fs_write(&dev, h, "x", 1, &wr), DOCK_E_INVALID);
    CHECK_EQ(wr, 0);
    CHECK_EQ(dock_fs_close(&dev, h), DOCK_OK);

    CHECK_EQ(dock_fs_stat(&dev, "USB1/subdir", &size, &dir), DOCK_OK);
    CHECK(dir);
    CHECK_EQ(dock_fs_stat(&dev, "USB1/nope", &size, &dir), DOCK_E_NOT_FOUND);
    CHECK_EQ(size, 0);
    CHECK(!dir);

    /* Directory listing, including a >100 character file name. */
    for (i = 0; i < 20; i++) {
        uint32_t sz = 0;
        bool d = false;
        rc = dock_fs_readdir(&dev, "USB1", (uint16_t)i, name, sizeof name, &sz, &d);
        if (rc == DOCK_E_END)
            break;
        CHECK_EQ(rc, DOCK_OK);
        if (strcmp(name, "hello.txt") == 0) {
            CHECK_EQ(sz, 11);
            CHECK(!d);
            seen |= 1;
        } else if (strcmp(name, "big.bin") == 0) {
            CHECK_EQ(sz, 50000);
            seen |= 2;
        } else if (strcmp(name, "subdir") == 0) {
            CHECK(d);
            seen |= 4;
        } else if (strncmp(name, "a_rather_long", 13) == 0) {
            CHECK_EQ(strlen(name), 250);
            CHECK_EQ(sz, 7);
            seen |= 8;
        } else if (strcmp(name, "new.txt") == 0) {
            seen |= 16;
        }
    }
    CHECK_EQ(seen, 31);
    CHECK_EQ(i, 5);
    /* small name buffer: truncated, NUL-terminated */
    CHECK_EQ(dock_fs_readdir(&dev, "USB1", 0, name, 4, NULL, NULL), DOCK_OK);
    CHECK_STR(name, "hel");

    CHECK_EQ(dock_fs_mkdir(&dev, "USB1/newdir"), DOCK_OK);
    CHECK_EQ(dock_fs_stat(&dev, "USB1/newdir", &size, &dir), DOCK_OK);
    CHECK(dir);
    CHECK_EQ(dock_fs_mkdir(&dev, "USB1/newdir"), DOCK_E_INVALID);
    CHECK_EQ(dock_fs_remove(&dev, "USB1/newdir"), DOCK_OK);
    CHECK_EQ(dock_fs_stat(&dev, "USB1/newdir", &size, &dir), DOCK_E_NOT_FOUND);
    CHECK_EQ(dock_fs_remove(&dev, "USB1/newdir"), DOCK_E_NOT_FOUND);

    /* Paths longer than the protocol allows never leave the device. */
    memset(name, 'x', 299);
    name[299] = '\0';
    {
        uint32_t calls = W.tx_calls;
        CHECK_EQ(dock_fs_stat(&dev, name, &size, &dir), DOCK_E_INVALID);
        CHECK_EQ(dock_fs_open(&dev, NULL, DOCK_FS_READ, &h), DOCK_E_INVALID);
        CHECK(W.tx_calls - calls < 3); /* at most a screen band slipped in */
    }
    CHECK_EQ(host.bad_frames, 0);
}

static void t_api_http(void)
{
    static uint8_t body[2500];
    size_t got = 0, i;
    uint16_t st = 0;
    uint8_t h = 0;
    uint32_t clen = 0;
    world_init(false, 1000, 1);
    connect();

    memset(big, 0, 5000);
    CHECK_EQ(dock_http_get(&dev, "http://example.com/data", big, 5000, &got, &st),
             DOCK_OK);
    CHECK_EQ(st, 200);
    CHECK_EQ(got, 3000);
    CHECK_MEM(big, W.get_body, 3000);
    CHECK_EQ(W.http_method, DOCK_HTTP_GET);
    CHECK_STR(W.http_url, "http://example.com/data");

    /* Into a buffer smaller than the body. */
    CHECK_EQ(dock_http_get(&dev, "http://example.com/data", big, 1000, &got, &st),
             DOCK_OK);
    CHECK_EQ(got, 1000);
    CHECK_MEM(big, W.get_body, 1000);

    for (i = 0; i < sizeof body; i++)
        body[i] = (uint8_t)(i * 5u + 1u);
    CHECK_EQ(dock_http_put(&dev, "http://example.com/up", body, sizeof body, &st),
             DOCK_OK);
    CHECK_EQ(st, 201);
    CHECK_EQ(W.http_method, DOCK_HTTP_PUT);
    CHECK_EQ(W.http_body_len, 2500);
    CHECK_EQ(W.put_len, 2500);
    CHECK_MEM(W.put_buf, body, 2500);

    /* Streaming API with headers. */
    CHECK_EQ(dock_http_begin(&dev, DOCK_HTTP_GET, "https://x.y/z", "Accept: */*\r\n",
                             0, &h), DOCK_OK);
    CHECK_EQ(h, 3);
    CHECK_STR(W.http_headers, "Accept: */*\r\n");
    CHECK_EQ(dock_http_response(&dev, h, &st, &clen), DOCK_OK);
    CHECK_EQ(clen, 3000);
    CHECK_EQ(dock_http_read(&dev, h, big, 10000, &got), DOCK_OK);
    CHECK_EQ(got, 700);
    CHECK_EQ(dock_http_end(&dev, h), DOCK_OK);

    CHECK_EQ(dock_http_begin(&dev, DOCK_HTTP_GET, "ftp://nope", NULL, 0, &h),
             DOCK_E_INVALID);
    CHECK_EQ(h, 0xFF);
    CHECK_EQ(dock_http_get(&dev, "ftp://nope", big, 10, &got, &st), DOCK_E_INVALID);
    CHECK_EQ(host.bad_frames, 0);
}

static void t_api_screen_control(void)
{
    uint32_t rows;
    int i;
    world_init(false, 1000, 1);
    connect();
    CHECK(mirror_converges(20000));
    dock_screen_enable(&dev, false);
    for (i = 0; i < 200; i++)
        dock_dev_poll(&dev);
    rows = W.damage_rows;
    fill_fb(99);
    for (i = 0; i < 5000; i++)
        dock_dev_poll(&dev);
    CHECK_EQ(W.damage_rows, rows);
    dock_screen_enable(&dev, true);
    CHECK(mirror_converges(20000));

    /* dock_screen_push sends the change before returning. */
    dev_fb[33 * 640 + 2] ^= 0x77;
    dock_screen_push(&dev);
    {
        int k;
        for (k = 0; k < 20; k++) /* let the last in-flight transfer land */
            dock_dhal_service(&W);
    }
    CHECK(fb_equal());
}

static void t_join_code(void)
{
    world_init(true, 1000, 1);
    CHECK(poll_until_state(DOCK_DEV_NEED_JOIN, 200));
    CHECK(!host.accepted);
    CHECK_EQ(dock_status(&dev), DOCK_E_JOIN_REQUIRED);
    CHECK_EQ(count_events(DOCK_HEV_JOIN_NEEDED), 1);
    CHECK_EQ(dock_audio_stop(&dev), DOCK_E_JOIN_REQUIRED);
    CHECK_EQ(W.damage_rows, 0); /* no screen before joining */

    dock_dev_set_join_code(&dev, "000000");
    CHECK_EQ(dev.state, DOCK_DEV_HELLO);
    CHECK(poll_until_state(DOCK_DEV_NEED_JOIN, 200));
    CHECK_EQ(dev.last_welcome_status, DOCK_E_JOIN_BAD);
    CHECK_EQ(dock_status(&dev), DOCK_E_JOIN_BAD);
    CHECK(!host.accepted);

    dock_dev_set_join_code(&dev, "123456");
    CHECK(poll_until_state(DOCK_DEV_RUNNING, 200));
    CHECK(host.accepted);
    CHECK_EQ(dock_status(&dev), DOCK_OK);
    CHECK_EQ(count_events(DOCK_HEV_CONNECTED), 1);
    CHECK(mirror_converges(20000));

    /* A device that skips the handshake gets JOIN_REQUIRED from the host. */
    {
        uint8_t frame[DOCK_HDR_LEN + 8];
        dock_host_reset(&host);
        CHECK(!host.accepted);
        W.td_r = W.td_w = 0;
        dock_hdr_build(frame, DOCK_T_AUD_STOP, 0x77, 0);
        dock_host_feed(&host, frame, DOCK_HDR_LEN);
        CHECK_EQ(W.td_w, DOCK_HDR_LEN + 3);
        CHECK_EQ(W.to_dev[2], DOCK_T_RESP);
        CHECK_EQ(W.to_dev[DOCK_HDR_LEN + 0], 0x77);
        CHECK_EQ(W.to_dev[DOCK_HDR_LEN + 1], DOCK_T_AUD_STOP);
        CHECK_EQ(W.to_dev[DOCK_HDR_LEN + 2], DOCK_E_JOIN_REQUIRED);
    }
}

static void t_link_drop_timeout_bye(void)
{
    uint8_t h;
    size_t got;
    int i;
    world_init(false, 1000, 1);
    connect();

    /* Host goes silent: request times out, the engine stays usable. */
    W.mute = true;
    {
        uint32_t t0 = W.ms;
        CHECK_EQ(dock_audio_stop(&dev), DOCK_E_TIMEOUT);
        CHECK(W.ms - t0 >= dev.timeout_ms);
        CHECK(!dev.req_active);
    }
    W.mute = false;
    CHECK_EQ(dock_audio_stop(&dev), DOCK_OK);

    /* Link dies in the middle of a request. */
    W.drop_link_on_http_read = true;
    CHECK_EQ(dock_http_begin(&dev, DOCK_HTTP_GET, "http://a/b", NULL, 0, &h), DOCK_OK);
    CHECK_EQ(dock_http_read(&dev, h, big, 100, &got), DOCK_E_DISCONNECTED);
    CHECK_EQ(got, 0);
    CHECK_EQ(dev.state, DOCK_DEV_OFFLINE);
    CHECK_EQ(dock_status(&dev), DOCK_E_DISCONNECTED);
    CHECK_EQ(dock_audio_stop(&dev), DOCK_E_DISCONNECTED);
    W.drop_link_on_http_read = false;

    /* Link comes back (the platform flushes its pipes and resets the host). */
    W.td_r = W.td_w = 0;
    W.tx_busy = false;
    dock_host_reset(&host);
    W.link_up = true;
    connect();
    CHECK_EQ(dock_audio_stop(&dev), DOCK_OK);
    CHECK(mirror_converges(20000));

    /* BYE */
    dock_dev_bye(&dev);
    for (i = 0; i < 10; i++)
        dock_dhal_service(&W);
    CHECK_EQ(count_events(DOCK_HEV_BYE), 1);
    CHECK(!host.accepted);
    CHECK_EQ(W.last_violations, 0);
}

void suite_devhost(void)
{
    RUN(t_hello_welcome);
    RUN(t_screen_mirror);
    RUN(t_screen_stress);
    RUN(t_keys);
    RUN(t_api_audio_usb_net);
    RUN(t_api_fs);
    RUN(t_api_http);
    RUN(t_api_screen_control);
    RUN(t_join_code);
    RUN(t_link_drop_timeout_bye);
}
