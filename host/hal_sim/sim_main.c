/*
 * dock-sim - a simulated calculator running the portable device engine
 * (core/dock_dev.c + core/dock_api.c) against dockd over a Unix socket.
 *
 *   dock-sim [-v] [--frames N] SOCK      animate the screen (mirror demo)
 *   dock-sim --selftest SOCK [--expect-ppm PATH]
 *                                        exercise the public API, print
 *                                        PASS/FAIL lines, exit 0 if all pass
 *   dock-sim --gen-files DIR             create the drive contents the
 *                                        self-test expects (DIR/USB0/...)
 */
#define _GNU_SOURCE
#include "bav.h"
#include "dock_api.h"
#include "sim_hal.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BAV_FRAMES 4u
#define BAV_FRAME_BYTES (320u * 240u / 8u)
#define WAV_RATE 22050u
#define WAV_SECONDS 2u

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 31u) << 11) | ((g & 63u) << 5) | (b & 31u));
}

/* Animated test card: scrolling gradient, a bouncing block and a stripe
 * pattern that looks a bit like text, so every frame changes. */
static void draw_frame(sim_hal_t *h, unsigned t)
{
    unsigned x, y, bx = (t * 5u) % (SIM_W - 32u), by = (t * 3u) % (SIM_H - 32u);
    for (y = 0; y < SIM_H; y++) {
        for (x = 0; x < SIM_W; x++) {
            uint16_t c = rgb565((x + t) >> 3, (y * 2u + t) >> 2, (x ^ y) >> 4);
            if (y >= 8 && y < 16 && ((x + t) % 6u) < 4u && ((x / 6u + y) & 1u))
                c = 0xFFFF;
            if (x >= bx && x < bx + 32u && y >= by && y < by + 32u)
                c = rgb565(31, 63 - (t & 63u), t & 31u);
            sim_put(h, x, y, c);
        }
    }
}

/* Static pattern the self-test leaves on screen; differs from every
 * animated frame on every row. */
static void draw_final(sim_hal_t *h)
{
    unsigned x, y;
    for (y = 0; y < SIM_H; y++)
        for (x = 0; x < SIM_W; x++)
            sim_put(h, x, y,
                    ((x / 8u + y / 8u) & 1u) ? rgb565(x * 31u / 319u, 0, y * 31u / 239u)
                                             : rgb565(0, (x + y) * 63u / 558u, 31));
}

/* The framebuffer as the dock should mirror it, as a binary PPM. */
static bool write_expected_ppm(const sim_hal_t *h, const char *path)
{
    FILE *f = fopen(path, "wb");
    unsigned x, y;
    bool ok;
    if (!f)
        return false;
    fprintf(f, "P6\n%u %u\n255\n", SIM_W, SIM_H);
    for (y = 0; y < SIM_H; y++) {
        for (x = 0; x < SIM_W; x++) {
            uint32_t p = dock_color16_to_xrgb(sim_get(h, x, y), DOCK_PIX_RGB565, DOCK_SCRF_BGR);
            fputc((int)((p >> 16) & 0xFF), f);
            fputc((int)((p >> 8) & 0xFF), f);
            fputc((int)(p & 0xFF), f);
        }
    }
    ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

/* Keep the engine (and the mirror) running for ms milliseconds. */
static void service_for(dock_t *d, unsigned ms)
{
    uint64_t end = mono_ms() + ms;
    while (mono_ms() < end) {
        dock_service(d);
        usleep(500);
    }
}

/* ------------------------------------------------------------------ */
/* test files                                                          */

static uint8_t bav_byte(unsigned frame, unsigned i)
{
    return (uint8_t)(frame * 7u + i * 13u);
}

static bool write_file(const char *path, const void *p, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    bool ok;
    if (fd < 0)
        return false;
    ok = write_all(fd, p, n);
    return close(fd) == 0 && ok;
}

static int gen_files(const char *dir)
{
    char drive[4096], path[4200];
    uint8_t *buf;
    size_t wav_data = WAV_RATE * WAV_SECONDS * 2u, i;
    bav_header_t bh;
    unsigned f;
    bool ok = true;

    snprintf(drive, sizeof drive, "%s/USB0", dir);
    if ((mkdir(dir, 0755) != 0 && errno != EEXIST) ||
        (mkdir(drive, 0755) != 0 && errno != EEXIST)) {
        perror("mkdir");
        return 1;
    }
    /* BAV: 4 frames with a known byte pattern. */
    buf = calloc(1, BAV_HEADER_LEN + BAV_FRAMES * BAV_FRAME_BYTES + 44 + wav_data);
    if (!buf)
        return 1;
    memset(&bh, 0, sizeof bh);
    bh.width = 320;
    bh.height = 240;
    bh.bpp = 1;
    bh.flags = BAV_F_LSB_FIRST;
    bh.fps_milli = 30000;
    bh.frame_count = BAV_FRAMES;
    bh.frame_bytes = BAV_FRAME_BYTES;
    bh.data_offset = BAV_HEADER_LEN;
    bav_header_write(buf, &bh);
    for (f = 0; f < BAV_FRAMES; f++)
        for (i = 0; i < BAV_FRAME_BYTES; i++)
            buf[BAV_HEADER_LEN + f * BAV_FRAME_BYTES + i] = bav_byte(f, (unsigned)i);
    snprintf(path, sizeof path, "%s/test.bav", drive);
    ok = ok && write_file(path, buf, BAV_HEADER_LEN + BAV_FRAMES * BAV_FRAME_BYTES);

    /* WAV: 2 s of a 441 Hz square wave, 22.05 kHz mono s16. */
    memcpy(buf, "RIFF", 4);
    dock_put_u32(buf + 4, (uint32_t)(36 + wav_data));
    memcpy(buf + 8, "WAVEfmt ", 8);
    dock_put_u32(buf + 16, 16);
    dock_put_u16(buf + 20, 1);
    dock_put_u16(buf + 22, 1);
    dock_put_u32(buf + 24, WAV_RATE);
    dock_put_u32(buf + 28, WAV_RATE * 2u);
    dock_put_u16(buf + 32, 2);
    dock_put_u16(buf + 34, 16);
    memcpy(buf + 36, "data", 4);
    dock_put_u32(buf + 40, (uint32_t)wav_data);
    for (i = 0; i < wav_data / 2u; i++)
        dock_put_u16(buf + 44 + i * 2u, (uint16_t)(((i / 25u) & 1u) ? 6000 : -6000));
    snprintf(path, sizeof path, "%s/test.wav", drive);
    ok = ok && write_file(path, buf, 44 + wav_data);
    free(buf);

    /* A symlink pointing out of the drive: must never be followed. */
    snprintf(path, sizeof path, "%s/escape", drive);
    unlink(path);
    if (symlink("/etc/passwd", path) != 0)
        ok = false;
    if (!ok)
        fprintf(stderr, "gen-files: %s\n", strerror(errno));
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* self-test                                                           */

typedef struct tally {
    int pass, fail;
} tally_t;

static void check(tally_t *t, bool ok, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void check(tally_t *t, bool ok, const char *name, const char *fmt, ...)
{
    char detail[256] = "";
    va_list ap;
    if (fmt) {
        va_start(ap, fmt);
        vsnprintf(detail, sizeof detail, fmt, ap);
        va_end(ap);
    }
    printf("%s %s%s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? " (" : "", detail,
           detail[0] ? ")" : "");
    fflush(stdout);
    if (ok)
        t->pass++;
    else
        t->fail++;
}

static void test_drives(dock_t *d, tally_t *t)
{
    char name[64] = "";
    int rc = dock_fs_drive(d, 0, name, sizeof name);
    check(t, rc == DOCK_OK && strcmp(name, "USB0") == 0, "fs_drive(0)", "rc=%d name=%s", rc,
          name);
    rc = dock_fs_drive(d, 1, name, sizeof name);
    check(t, rc == DOCK_E_END, "fs_drive(1) end", "rc=%d", rc);
}

static void test_readdir(dock_t *d, tally_t *t)
{
    char name[64];
    uint32_t size, bav_size = 0, wav_size = 0;
    bool dir;
    uint16_t i;
    int rc = DOCK_OK, count = 0;
    for (i = 0; i < 100; i++) {
        rc = dock_fs_readdir(d, "USB0", i, name, sizeof name, &size, &dir);
        if (rc != DOCK_OK)
            break;
        count++;
        if (strcmp(name, "test.bav") == 0)
            bav_size = size;
        if (strcmp(name, "test.wav") == 0)
            wav_size = size;
    }
    check(t, rc == DOCK_E_END && count >= 2, "fs_readdir lists the drive", "rc=%d count=%d",
          rc, count);
    check(t, bav_size == BAV_HEADER_LEN + BAV_FRAMES * BAV_FRAME_BYTES, "fs_readdir size",
          "bav=%u", bav_size);
    check(t, wav_size == 44u + WAV_RATE * WAV_SECONDS * 2u, "fs_readdir wav size", "wav=%u",
          wav_size);
}

static void test_bav_read(dock_t *d, tally_t *t)
{
    static uint8_t frame[BAV_FRAME_BYTES];
    uint8_t hdr[BAV_HEADER_LEN];
    bav_header_t bh;
    uint8_t h = 0xFF;
    size_t got = 0, i;
    uint32_t pos = 0;
    bool same = true;
    int rc;

    rc = dock_fs_open(d, "USB0/test.bav", DOCK_FS_READ, &h);
    check(t, rc == DOCK_OK && h < 16, "fs_open read", "rc=%d h=%u", rc, h);
    if (rc != DOCK_OK)
        return;
    rc = dock_fs_read(d, h, hdr, sizeof hdr, &got);
    check(t, rc == DOCK_OK && got == sizeof hdr && bav_header_read(hdr, &bh) &&
                 bh.frame_count == BAV_FRAMES,
          "fs_read bav header", "rc=%d got=%zu", rc, got);
    rc = dock_fs_seek(d, h, (int32_t)(BAV_HEADER_LEN + 2u * BAV_FRAME_BYTES), DOCK_SEEK_SET,
                      &pos);
    check(t, rc == DOCK_OK && pos == BAV_HEADER_LEN + 2u * BAV_FRAME_BYTES, "fs_seek set",
          "rc=%d pos=%u", rc, pos);
    rc = dock_fs_read(d, h, frame, sizeof frame, &got);
    for (i = 0; i < sizeof frame; i++)
        same = same && frame[i] == bav_byte(2, (unsigned)i);
    check(t, rc == DOCK_OK && got == sizeof frame && same, "fs_read frame 2 (9600 B)",
          "rc=%d got=%zu", rc, got);
    rc = dock_fs_seek(d, h, -10, DOCK_SEEK_CUR, &pos);
    check(t, rc == DOCK_OK && pos == BAV_HEADER_LEN + 3u * BAV_FRAME_BYTES - 10u,
          "fs_seek cur", "rc=%d pos=%u", rc, pos);
    rc = dock_fs_seek(d, h, 0, DOCK_SEEK_END, &pos);
    check(t, rc == DOCK_OK && pos == BAV_HEADER_LEN + BAV_FRAMES * BAV_FRAME_BYTES,
          "fs_seek end", "rc=%d pos=%u", rc, pos);
    rc = dock_fs_read(d, h, frame, 16, &got);
    check(t, rc == DOCK_E_END && got == 0, "fs_read at EOF", "rc=%d got=%zu", rc, got);
    rc = dock_fs_close(d, h);
    check(t, rc == DOCK_OK, "fs_close", "rc=%d", rc);
    rc = dock_fs_close(d, h);
    check(t, rc == DOCK_E_INVALID, "fs_close twice", "rc=%d", rc);
}

static void test_write(dock_t *d, tally_t *t)
{
    static const char msg[] = "hello dock\n";
    char back[64], name[64];
    uint8_t h = 0xFF;
    size_t n = 0;
    uint32_t size = 0;
    bool dir = false;
    int rc;

    rc = dock_fs_open(d, "USB0/simdir/hello.txt", DOCK_FS_WRITE | DOCK_FS_CREATE, &h);
    check(t, rc == DOCK_E_NOT_FOUND, "fs_open in missing dir", "rc=%d", rc);
    rc = dock_fs_mkdir(d, "USB0/simdir");
    check(t, rc == DOCK_OK, "fs_mkdir", "rc=%d", rc);
    rc = dock_fs_mkdir(d, "USB0/simdir");
    check(t, rc == DOCK_E_INVALID, "fs_mkdir existing", "rc=%d", rc);
    rc = dock_fs_open(d, "USB0/simdir/hello.txt",
                      DOCK_FS_WRITE | DOCK_FS_CREATE | DOCK_FS_TRUNC, &h);
    check(t, rc == DOCK_OK, "fs_open create", "rc=%d", rc);
    rc = dock_fs_write(d, h, msg, sizeof msg - 1, &n);
    check(t, rc == DOCK_OK && n == sizeof msg - 1, "fs_write", "rc=%d n=%zu", rc, n);
    rc = dock_fs_close(d, h);
    check(t, rc == DOCK_OK, "fs_close after write", "rc=%d", rc);
    rc = dock_fs_stat(d, "USB0/simdir/hello.txt", &size, &dir);
    check(t, rc == DOCK_OK && size == sizeof msg - 1 && !dir, "fs_stat file", "rc=%d size=%u",
          rc, size);
    rc = dock_fs_stat(d, "USB0/simdir", &size, &dir);
    check(t, rc == DOCK_OK && dir, "fs_stat dir", "rc=%d", rc);
    rc = dock_fs_readdir(d, "USB0/simdir", 0, name, sizeof name, &size, &dir);
    check(t, rc == DOCK_OK && strcmp(name, "hello.txt") == 0, "fs_readdir subdir",
          "rc=%d name=%s", rc, name);
    rc = dock_fs_open(d, "USB0/simdir/hello.txt", DOCK_FS_READ, &h);
    if (rc == DOCK_OK) {
        rc = dock_fs_read(d, h, back, sizeof back, &n);
        dock_fs_close(d, h);
    }
    check(t, rc == DOCK_OK && n == sizeof msg - 1 && memcmp(back, msg, n) == 0,
          "fs read back", "rc=%d n=%zu", rc, n);
    rc = dock_fs_remove(d, "USB0/simdir");
    check(t, rc == DOCK_E_DENIED, "fs_remove non-empty dir", "rc=%d", rc);
    rc = dock_fs_remove(d, "USB0/simdir/hello.txt");
    check(t, rc == DOCK_OK, "fs_remove file", "rc=%d", rc);
    rc = dock_fs_remove(d, "USB0/simdir");
    check(t, rc == DOCK_OK, "fs_remove dir", "rc=%d", rc);
    rc = dock_fs_stat(d, "USB0/simdir", &size, &dir);
    check(t, rc == DOCK_E_NOT_FOUND, "fs_stat removed", "rc=%d", rc);
}

static void test_sandbox(dock_t *d, tally_t *t)
{
    static const char *const bad[] = {"../etc/passwd", "USB0/../../etc/passwd",
                                      "/etc/passwd", "USB0/./../USB0/test.wav"};
    uint32_t size = 0;
    bool dir;
    uint8_t h;
    size_t i;
    int rc;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        rc = dock_fs_stat(d, bad[i], &size, &dir);
        check(t, rc == DOCK_E_DENIED, "sandbox rejects", "%s rc=%d", bad[i], rc);
    }
    rc = dock_fs_open(d, "USB0/escape", DOCK_FS_READ, &h);
    check(t, rc == DOCK_E_DENIED, "symlink not followed (open)", "rc=%d", rc);
    rc = dock_fs_stat(d, "USB0/escape", &size, &dir);
    check(t, rc == DOCK_E_NOT_FOUND, "symlink hidden (stat)", "rc=%d", rc);
    rc = dock_fs_stat(d, "test.wav", &size, &dir);
    check(t, rc == DOCK_OK && size > 44, "path without drive = first drive", "rc=%d", rc);
    rc = dock_fs_stat(d, "usb0/test.wav", &size, &dir);
    check(t, rc == DOCK_OK, "drive name case-insensitive", "rc=%d", rc);
    rc = dock_fs_open(d, "USB0/test.wav", DOCK_FS_CREATE, &h);
    check(t, rc == DOCK_E_INVALID, "bad open mode", "rc=%d", rc);
}

static void test_handles(dock_t *d, tally_t *t)
{
    uint8_t h[17];
    int i, opened = 0, rc = DOCK_OK;
    for (i = 0; i < 17; i++) {
        rc = dock_fs_open(d, "USB0/test.bav", DOCK_FS_READ, &h[i]);
        if (rc != DOCK_OK)
            break;
        opened++;
    }
    check(t, opened == 16 && rc == DOCK_E_BUSY, "16 handles then BUSY", "opened=%d rc=%d",
          opened, rc);
    for (i = 0; i < opened; i++)
        dock_fs_close(d, h[i]);
}

static void test_audio(dock_t *d, tally_t *t)
{
    static uint8_t pcm[50000];
    uint32_t p0 = 0, p1 = 0, p2 = 0;
    bool playing = false;
    size_t acc = 0;
    int rc;

    rc = dock_audio_play_file(d, "USB0/test.wav");
    check(t, rc == DOCK_OK, "audio_play_file", "rc=%d", rc);
    rc = dock_audio_pos(d, &p0, &playing);
    check(t, rc == DOCK_OK && playing && p0 < 300, "audio_pos at start", "rc=%d pos=%u", rc, p0);
    service_for(d, 300);
    dock_audio_pos(d, &p1, &playing);
    service_for(d, 300);
    rc = dock_audio_pos(d, &p2, &playing);
    check(t, rc == DOCK_OK && playing && p2 > p1 && p1 >= p0 && p2 >= 350 && p2 <= 1200,
          "audio_pos advances in real time", "p0=%u p1=%u p2=%u", p0, p1, p2);
    rc = dock_audio_volume(d, 50);
    check(t, rc == DOCK_OK, "audio_volume", "rc=%d", rc);
    rc = dock_audio_stop(d);
    dock_audio_pos(d, &p1, &playing);
    check(t, rc == DOCK_OK && !playing, "audio_stop", "rc=%d playing=%d", rc, playing);
    rc = dock_audio_play_file(d, "USB0/test.bav");
    check(t, rc == DOCK_E_INVALID, "audio_play_file rejects non-WAV", "rc=%d", rc);
    rc = dock_audio_play_file(d, "USB0/missing.wav");
    check(t, rc == DOCK_E_NOT_FOUND, "audio_play_file missing", "rc=%d", rc);

    rc = dock_audio_open(d, WAV_RATE, DOCK_AUD_S16LE, 1);
    check(t, rc == DOCK_OK, "audio_open stream", "rc=%d", rc);
    rc = dock_audio_write(d, pcm, 8820, &acc);
    check(t, rc == DOCK_OK && acc == 8820, "audio_write", "rc=%d acc=%zu", rc, acc);
    rc = dock_audio_write(d, pcm, sizeof pcm, &acc);
    check(t, rc == DOCK_OK && acc > 0 && acc < sizeof pcm, "audio_write paces (ring full)",
          "rc=%d acc=%zu", rc, acc);
    rc = dock_audio_open(d, 1, DOCK_AUD_S16LE, 1);
    check(t, rc == DOCK_E_INVALID, "audio_open bad rate", "rc=%d", rc);
    dock_audio_stop(d);
}

static void test_misc(dock_t *d, tally_t *t)
{
    dock_usb_info_t info;
    bool up = true;
    char ssid[40], ip[40], body[16];
    size_t got = 0;
    uint16_t hs = 0;
    int rc = dock_usb_device(d, 0, &info);
    check(t, rc == DOCK_OK || rc == DOCK_E_END, "usb_device(0)", "rc=%d", rc);
    rc = dock_net_status(d, &up, ssid, sizeof ssid, ip, sizeof ip);
    check(t, rc == DOCK_E_NET_DISABLED && !up, "net_status disabled by default", "rc=%d", rc);
    rc = dock_http_get(d, "http://example.com/", body, sizeof body, &got, &hs);
    check(t, rc == DOCK_E_NET_DISABLED, "http_get disabled by default", "rc=%d", rc);
}

static int selftest(sim_hal_t *hal, dock_t *d, const char *expect_ppm)
{
    tally_t t = {0, 0};
    uint64_t end = mono_ms() + 5000;
    uint32_t pos;
    bool pl;
    unsigned f;

    while (dock_status(d) != DOCK_OK && mono_ms() < end) {
        dock_service(d);
        usleep(500);
    }
    check(&t, dock_status(d) == DOCK_OK, "connect + HELLO accepted", "status=%d",
          dock_status(d));
    if (dock_status(d) != DOCK_OK)
        goto done;
    for (f = 0; f < 20; f++) { /* stream some animation */
        draw_frame(hal, f);
        service_for(d, 33);
    }
    check(&t, d->frames_sent > 20, "screen frames sent", "frames=%u", d->frames_sent);

    test_drives(d, &t);
    test_readdir(d, &t);
    test_bav_read(d, &t);
    test_write(d, &t);
    test_sandbox(d, &t);
    test_handles(d, &t);
    test_audio(d, &t);
    test_misc(d, &t);

    /* Leave a known picture on the mirror. A request answered after the push
     * proves dockd has applied every row (frames are processed in order). */
    draw_final(hal);
    dock_screen_push(d);
    service_for(d, 200);
    check(&t, dock_audio_pos(d, &pos, &pl) == DOCK_OK, "sync after final frame", NULL);
    if (expect_ppm)
        check(&t, write_expected_ppm(hal, expect_ppm), "write expected ppm", "%s", expect_ppm);
done:
    dock_dev_bye(d);
    printf("selftest: %d passed, %d failed\n", t.pass, t.fail);
    return t.fail == 0 && t.pass > 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */

static int animate(sim_hal_t *hal, dock_t *d, long frames)
{
    unsigned t = 0;
    uint64_t next = mono_ms();
    for (;;) {
        dock_hidkey_t hk;
        dock_key_t k;
        if (mono_ms() >= next) {
            draw_frame(hal, t++);
            next += 33;
            if (frames > 0 && (long)t >= frames)
                break;
        }
        dock_service(d);
        while (dock_dev_key_pop(d, &k))
            logv("key: tikey %04x scan %02x flags %02x", k.tikey, k.scancode, k.flags);
        while (dock_key_get(d, &hk))
            logv("hid: usage %02x mods %02x %s '%c'", hk.usage, hk.mods,
                 hk.down ? "down" : "up", hk.ascii >= 0x20 && hk.ascii < 0x7F ? hk.ascii : ' ');
        usleep(500);
    }
    dock_dev_bye(d);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {{"selftest", no_argument, NULL, 's'},
                                       {"gen-files", required_argument, NULL, 'g'},
                                       {"expect-ppm", required_argument, NULL, 'e'},
                                       {"frames", required_argument, NULL, 'f'},
                                       {"verbose", no_argument, NULL, 'v'},
                                       {NULL, 0, NULL, 0}};
    static sim_hal_t hal;
    static dock_t dev;
    const char *expect = NULL;
    bool self = false;
    long frames = 0;
    int c, rc;

    while ((c = getopt_long(argc, argv, "v", lo, NULL)) != -1) {
        switch (c) {
        case 's': self = true; break;
        case 'g': return gen_files(optarg);
        case 'e': expect = optarg; break;
        case 'f': frames = strtol(optarg, NULL, 10); break;
        case 'v': log_set_verbose(true); break;
        default:
            fprintf(stderr, "usage: dock-sim [-v] [--frames N] [--selftest [--expect-ppm F]] SOCK\n"
                            "       dock-sim --gen-files DIR\n");
            return 2;
        }
    }
    if (optind != argc - 1) {
        fprintf(stderr, "dock-sim: socket path required\n");
        return 2;
    }
    sim_hal_init(&hal, argv[optind]);
    draw_frame(&hal, 0);
    dock_dev_init(&dev, &hal, 0x51A1C0DEu);
    rc = self ? selftest(&hal, &dev, expect) : animate(&hal, &dev, frames);
    sim_hal_close(&hal);
    return rc;
}
