/*
 * bavconv - convert raw 8-bit grayscale frames into a .bav file (core/bav.h).
 *
 * Runs on the PC that prepares the video, not on the Pi or the calculator.
 * Typical use (see make_badapple.sh):
 *
 *   ffmpeg -i in.mp4 -vf "fps=30,scale=...,format=gray" -f rawvideo \
 *          -pix_fmt gray - | bavconv -f 30 -o badapple.bav
 *
 * Input:  width*height bytes per frame, one byte per pixel, row-major,
 *         0 = black .. 255 = white. A trailing partial frame is dropped.
 * Output: 32-byte BAV1 header + frames packed to 1 bpp, LSB first: pixel
 *         (x, y) is bit (p & 7) of byte (p >> 3), p = y*width + x. With a
 *         width that is a multiple of 8 this is bit (x & 7) of byte
 *         y*(width/8) + (x >> 3). Bit 1 = white.
 *
 * frame_count is not known until the input ends, so the header is written
 * first with frame_count = 0 and patched at the end (output must be a
 * seekable file). If the output is not seekable (a pipe) but the input is a
 * regular file, the frame count is computed from the input size instead.
 *
 * Portable C99, standard library only.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "../core/bav.h"

#define BAVCONV_VERSION "1.0"

/* 8x8 Bayer matrix, values 0..63. */
static const unsigned char bayer8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

static const char *prog = "bavconv";

static void usage(FILE *out)
{
    fprintf(out,
        "usage: %s [options] [INPUT]\n"
        "Convert raw 8-bit grayscale frames (INPUT or stdin) to a 1-bpp .bav file.\nFor COLOUR video, use video-convert.html (runs in a browser).\n"
        "\n"
        "  -W WIDTH    frame width in pixels  (default 320, multiple of 8)\n"
        "  -H HEIGHT   frame height in pixels (default 240)\n"
        "  -f FPS      frame rate: 30, 29.97 or 30000/1001 (default 30)\n"
        "  -t LEVEL    threshold 0..255, gray >= LEVEL is white (default 128)\n"
        "  -d          ordered (8x8 Bayer) dithering instead of a hard threshold;\n"
        "              -t then shifts the brightness (128 = neutral)\n"
        "  -i          invert (white <-> black)\n"
        "  -o FILE     output file (default: stdout, must then be seekable\n"
        "              unless INPUT is a regular file)\n"
        "  -q          quiet\n"
        "  -h          this help\n",
        prog);
}

static void die(const char *msg, const char *arg)
{
    if (arg)
        fprintf(stderr, "%s: %s: %s\n", prog, msg, arg);
    else
        fprintf(stderr, "%s: %s\n", prog, msg);
    exit(1);
}

static unsigned long parse_ulong(const char *s, unsigned long min, unsigned long max,
                                 const char *what)
{
    char *end;
    unsigned long v;

    if (!s || !*s || *s == '-' || *s == '+')
        die("invalid number for", what);
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || *end || v < min || v > max) {
        fprintf(stderr, "%s: %s must be %lu..%lu (got '%s')\n", prog, what, min, max, s);
        exit(1);
    }
    return v;
}

/*
 * Parse a frame rate into frames per 1000 s without floating point:
 * "30" -> 30000, "29.97" -> 29970, "23.976" -> 23976, "30000/1001" -> 29970
 * (rounded to nearest). Returns 0 on error.
 */
static unsigned long parse_fps_milli(const char *s)
{
    unsigned long ip = 0, frac = 0, scale = 1000, num, den;
    const char *p = s, *slash = strchr(s, '/');
    int digits = 0, round_up = 0;

    if (slash) {
        char buf[64];
        size_t n = (size_t)(slash - s);
        char *end;

        if (n == 0 || n >= sizeof buf)
            return 0;
        memcpy(buf, s, n);
        buf[n] = '\0';
        if (buf[0] < '0' || buf[0] > '9' || slash[1] < '0' || slash[1] > '9')
            return 0;
        errno = 0;
        num = strtoul(buf, &end, 10);
        if (errno || *end)
            return 0;
        den = strtoul(slash + 1, &end, 10);
        if (errno || *end || den == 0)
            return 0;
        if (num > (ULONG_MAX - den / 2) / 1000ul)
            return 0;
        return (num * 1000ul + den / 2) / den;
    }

    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9') {
        ip = ip * 10 + (unsigned long)(*p - '0');
        if (ip > 1000000ul)
            return 0;
        p++;
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9')
            return 0;
        while (*p >= '0' && *p <= '9') {
            if (digits < 3) {
                frac = frac * 10 + (unsigned long)(*p - '0');
                scale /= 10;
            } else if (digits == 3 && *p >= '5') {
                round_up = 1;
            }
            digits++;
            p++;
        }
    }
    if (*p)
        return 0;
    return ip * 1000ul + frac * scale + (unsigned long)round_up;
}

/* Pack one frame. Width must be a multiple of 8. */
static void pack_frame(const unsigned char *gray, unsigned char *out, unsigned w, unsigned h,
                       int threshold, int dither, int invert)
{
    unsigned x, y;
    size_t i = 0, o = 0;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x += 8) {
            unsigned b, byte = 0;
            for (b = 0; b < 8; b++, i++) {
                int t, white;
                if (dither) {
                    /* Bayer cell -> threshold in (0, 256); -t shifts it. */
                    t = (int)bayer8[y & 7][(x + b) & 7] * 4 + 2 + (threshold - 128);
                    if (t < 1)
                        t = 1;
                    if (t > 255)
                        t = 255;
                } else {
                    t = threshold;
                }
                white = (int)gray[i] >= t;
                if (invert)
                    white = !white;
                if (white)
                    byte |= 1u << b;
            }
            out[o++] = (unsigned char)byte;
        }
    }
}

/* Read up to n bytes; returns the number read (< n only at EOF). */
static size_t read_full(FILE *f, unsigned char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        size_t r = fread(buf + got, 1, n - got, f);
        if (r == 0) {
            if (ferror(f))
                die("read error", strerror(errno));
            break;
        }
        got += r;
    }
    return got;
}

int main(int argc, char **argv)
{
    unsigned long width = 320, height = 240, threshold = 128, fps_milli = 30000;
    int dither = 0, invert = 0, quiet = 0;
    const char *out_path = NULL, *in_path = NULL;
    FILE *in = stdin, *out = stdout;
    unsigned char hdr[BAV_HEADER_LEN];
    unsigned char *gray, *packed;
    size_t gray_bytes, frame_bytes, got;
    unsigned long frames = 0, expected = 0;
    int out_seekable, have_expected = 0;
    bav_header_t h;
    int i;

    if (argc > 0 && argv[0] && *argv[0]) {
        const char *s = strrchr(argv[0], '/');
        prog = s ? s + 1 : argv[0];
    }

    /* Minimal POSIX-style option parsing (getopt is not C99). */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        char opt;
        const char *val = NULL;

        if (a[0] != '-' || a[1] == '\0') {
            if (in_path)
                die("more than one input file given", a);
            in_path = a;
            continue;
        }
        if (strcmp(a, "--") == 0) {
            if (i + 1 < argc)
                in_path = argv[++i];
            if (i + 1 < argc)
                die("more than one input file given", argv[i + 1]);
            break;
        }
        if (strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            printf("bavconv %s\n", BAVCONV_VERSION);
            return 0;
        }
        opt = a[1];
        if (opt == 'W' || opt == 'H' || opt == 'f' || opt == 't' || opt == 'o') {
            if (a[2])
                val = a + 2;
            else if (i + 1 < argc)
                val = argv[++i];
            else {
                fprintf(stderr, "%s: option -%c needs a value\n", prog, opt);
                return 1;
            }
        } else if (a[2]) {
            fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
            usage(stderr);
            return 1;
        }
        switch (opt) {
        case 'W': width = parse_ulong(val, 8, 4096, "width"); break;
        case 'H': height = parse_ulong(val, 1, 4096, "height"); break;
        case 't': threshold = parse_ulong(val, 0, 255, "threshold"); break;
        case 'f':
            fps_milli = parse_fps_milli(val);
            if (fps_milli == 0 || fps_milli > 1000000ul)
                die("invalid frame rate (use e.g. 30, 29.97 or 30000/1001)", val);
            break;
        case 'o': out_path = val; break;
        case 'd': dither = 1; break;
        case 'i': invert = 1; break;
        case 'q': quiet = 1; break;
        case 'h': usage(stdout); return 0;
        default:
            fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
            usage(stderr);
            return 1;
        }
    }

    if (width % 8u != 0)
        die("width must be a multiple of 8", NULL);

    gray_bytes = (size_t)width * (size_t)height;
    frame_bytes = gray_bytes / 8u;

    if (in_path && strcmp(in_path, "-") != 0) {
        in = fopen(in_path, "rb");
        if (!in)
            die(strerror(errno), in_path);
    } else {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
    }

    if (out_path && strcmp(out_path, "-") != 0) {
        out = fopen(out_path, "w+b");
        if (!out)
            die(strerror(errno), out_path);
    } else {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        out_path = NULL;
    }

    out_seekable = fseek(out, 0L, SEEK_SET) == 0;

    /* Regular input file: frame count can be known up front. */
    if (in != stdin && fseek(in, 0L, SEEK_END) == 0) {
        long sz = ftell(in);
        if (sz >= 0) {
            expected = (unsigned long)sz / (unsigned long)gray_bytes;
            have_expected = 1;
            if ((unsigned long)sz % (unsigned long)gray_bytes && !quiet)
                fprintf(stderr, "%s: warning: input size is not a whole number of "
                        "%lux%lu frames; the trailing %lu bytes are ignored\n",
                        prog, width, height, (unsigned long)sz % (unsigned long)gray_bytes);
        }
        if (fseek(in, 0L, SEEK_SET) != 0)
            die("cannot rewind input", in_path);
    }

    if (!out_seekable && !have_expected)
        die("output is not seekable and the frame count is unknown; use -o FILE "
            "or give a regular INPUT file", NULL);

    gray = malloc(gray_bytes);
    packed = malloc(frame_bytes);
    if (!gray || !packed)
        die("out of memory", NULL);

    h.width = (uint16_t)width;
    h.height = (uint16_t)height;
    h.bpp = 1;
    h.flags = BAV_F_LSB_FIRST;
    h.fmt = BAV_FMT_I1; /* 1 bpp B/W; colour conversion is video-convert.html */
    h.fps_milli = (uint32_t)fps_milli;
    h.frame_count = (uint32_t)(have_expected ? expected : 0ul);
    h.frame_bytes = (uint32_t)frame_bytes;
    h.data_offset = BAV_HEADER_LEN;
    h.palette_count = 0; /* the player fills a black/white palette by default */
    bav_header_write(hdr, &h);
    if (fwrite(hdr, 1, sizeof hdr, out) != sizeof hdr)
        die("write error", strerror(errno));

    for (;;) {
        if (have_expected && frames == expected)
            break;
        got = read_full(in, gray, gray_bytes);
        if (got == 0)
            break;
        if (got < gray_bytes) {
            if (!quiet)
                fprintf(stderr, "%s: warning: dropping incomplete last frame (%lu of %lu bytes)\n",
                        prog, (unsigned long)got, (unsigned long)gray_bytes);
            break;
        }
        if (frames == 0xFFFFFFFFul)
            die("too many frames", NULL);
        pack_frame(gray, packed, (unsigned)width, (unsigned)height, (int)threshold, dither,
                   invert);
        if (fwrite(packed, 1, frame_bytes, out) != frame_bytes)
            die("write error", strerror(errno));
        frames++;
    }

    if (have_expected && frames != expected)
        die("input file shrank while reading", in_path);

    if (out_seekable) {
        h.frame_count = (uint32_t)frames;
        bav_header_write(hdr, &h);
        if (fflush(out) != 0 || fseek(out, 0L, SEEK_SET) != 0 ||
            fwrite(hdr, 1, sizeof hdr, out) != sizeof hdr)
            die("cannot update header", strerror(errno));
    }
    if (fflush(out) != 0)
        die("write error", strerror(errno));
    if (out != stdout && fclose(out) != 0)
        die("write error", strerror(errno));
    if (in != stdin)
        fclose(in);
    free(gray);
    free(packed);

    if (frames == 0 && !quiet)
        fprintf(stderr, "%s: warning: no complete frames in input\n", prog);
    if (!quiet)
        fprintf(stderr, "%s: %lu frames %lux%lu @ %lu.%03lu fps, %lu bytes/frame%s -> %s\n",
                prog, frames, width, height, fps_milli / 1000ul, fps_milli % 1000ul,
                (unsigned long)frame_bytes, dither ? " (dithered)" : "",
                out_path ? out_path : "stdout");
    return 0;
}
