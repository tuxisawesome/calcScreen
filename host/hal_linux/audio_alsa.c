/* audio_alsa.c - see audio_alsa.h. */
#define _GNU_SOURCE
#include "audio_alsa.h"
#include "dock_proto.h"
#include "util.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LATENCY_US 200000u   /* ALSA buffer: plenty of slack for a poll loop */
#define PUMP_MS 10           /* how often to top up while playing */
#define CHUNK_BYTES 16384u   /* file read / volume scratch size */
#define MIN_RATE 4000u
#define MAX_RATE 192000u

enum src_kind { SRC_NONE, SRC_STREAM, SRC_FILE };

struct audio {
    bool null_backend;
    char device[128];

    /* current output format */
    uint32_t rate;
    uint8_t fmt, channels, frame_bytes;

    /* device */
    snd_pcm_t *pcm;
    bool pcm_open;               /* pcm, or the null clock, is running */
    uint64_t frames_written;     /* handed to the device this playback */
    uint32_t last_pos_ms;        /* monotonic guard */

    /* null backend clock */
    uint64_t null_head;          /* frames "played" */
    uint64_t null_last_us;

    /* mixer */
    snd_mixer_t *mixer;
    snd_mixer_elem_t *mixer_elem;
    uint8_t master_vol, prog_vol;

    /* source */
    enum src_kind src;
    uint8_t *ring;               /* SRC_STREAM */
    size_t ring_cap, ring_head, ring_len;
    int fd;                      /* SRC_FILE */
    uint64_t file_left;          /* data bytes not read yet */
    uint8_t chunk[CHUNK_BYTES];  /* file data read but not yet written */
    size_t chunk_off, chunk_len;
    uint8_t scratch[CHUNK_BYTES];/* software volume */
};

/* ------------------------------------------------------------------ */
/* mixer / volume                                                      */

static uint8_t effective_volume(const audio_t *a)
{
    return (uint8_t)((unsigned)a->master_vol * a->prog_vol / 100u);
}

static void mixer_close(audio_t *a)
{
    if (a->mixer)
        snd_mixer_close(a->mixer);
    a->mixer = NULL;
    a->mixer_elem = NULL;
}

/* "hdmi:CARD=vc4hdmi0,DEV=0" -> "hw:CARD=vc4hdmi0"; otherwise "default". */
static void mixer_card_name(const char *dev, char *out, size_t cap)
{
    const char *c = strstr(dev, "CARD=");
    size_t n;
    if (!c) {
        str_copy(out, cap, "default");
        return;
    }
    c += 5;
    n = strcspn(c, ",'\" ");
    if (n == 0 || n + 9 >= cap) {
        str_copy(out, cap, "default");
        return;
    }
    memcpy(out, "hw:CARD=", 8);
    memcpy(out + 8, c, n);
    out[8 + n] = '\0';
}

static void mixer_open(audio_t *a)
{
    static const char *const names[] = {"PCM", "Master", "HDMI", "Headphone"};
    char card[96];
    size_t i;
    snd_mixer_elem_t *e;

    mixer_close(a);
    if (a->null_backend)
        return;
    mixer_card_name(a->device, card, sizeof card);
    if (snd_mixer_open(&a->mixer, 0) < 0) {
        a->mixer = NULL;
        return;
    }
    if (snd_mixer_attach(a->mixer, card) < 0 ||
        snd_mixer_selem_register(a->mixer, NULL, NULL) < 0 ||
        snd_mixer_load(a->mixer) < 0) {
        logv("audio: no mixer on %s, using software volume", card);
        mixer_close(a);
        return;
    }
    for (i = 0; i < sizeof names / sizeof names[0] && !a->mixer_elem; i++) {
        for (e = snd_mixer_first_elem(a->mixer); e; e = snd_mixer_elem_next(e)) {
            if (snd_mixer_selem_is_active(e) && snd_mixer_selem_has_playback_volume(e) &&
                strcmp(snd_mixer_selem_get_name(e), names[i]) == 0) {
                a->mixer_elem = e;
                break;
            }
        }
    }
    if (!a->mixer_elem) {
        logv("audio: no volume control on %s, using software volume", card);
        mixer_close(a);
    } else {
        logv("audio: mixer %s '%s'", card, snd_mixer_selem_get_name(a->mixer_elem));
    }
}

static void mixer_apply(audio_t *a)
{
    long lo, hi;
    if (!a->mixer_elem)
        return;
    if (snd_mixer_selem_get_playback_volume_range(a->mixer_elem, &lo, &hi) < 0 || hi <= lo)
        return;
    snd_mixer_selem_set_playback_volume_all(a->mixer_elem,
                                            lo + (hi - lo) * a->master_vol / 100);
}

/* Software gain for devices without a mixer; returns the buffer to send. */
static const uint8_t *apply_soft_volume(audio_t *a, const uint8_t *p, size_t n)
{
    unsigned vol = a->mixer_elem ? a->prog_vol : effective_volume(a);
    size_t i;
    if (vol >= 100)
        return p;
    if (a->fmt == DOCK_AUD_U8) {
        for (i = 0; i < n; i++)
            a->scratch[i] = (uint8_t)(128 + ((int)p[i] - 128) * (int)vol / 100);
    } else {
        for (i = 0; i + 1 < n; i += 2) {
            int16_t s = (int16_t)(uint16_t)(p[i] | (p[i + 1] << 8));
            int32_t v = (int32_t)s * (int32_t)vol / 100;
            a->scratch[i] = (uint8_t)((uint32_t)v & 0xFFu);
            a->scratch[i + 1] = (uint8_t)(((uint32_t)v >> 8) & 0xFFu);
        }
    }
    return a->scratch;
}

/* ------------------------------------------------------------------ */
/* device                                                              */

static void null_advance(audio_t *a)
{
    uint64_t now = mono_us();
    uint64_t elapsed = now - a->null_last_us;
    uint64_t can = elapsed * a->rate / 1000000u;
    uint64_t queued = a->frames_written - a->null_head;
    if (can == 0)
        return; /* keep the remainder for next time */
    if (can > queued) {
        a->null_head = a->frames_written; /* underrun: time is lost */
        a->null_last_us = now;
    } else {
        a->null_head += can;
        a->null_last_us += can * 1000000u / a->rate;
    }
}

static void dev_close(audio_t *a)
{
    if (a->pcm) {
        snd_pcm_drop(a->pcm);
        snd_pcm_close(a->pcm);
        a->pcm = NULL;
    }
    a->pcm_open = false;
}

/* Start playback once a period is queued instead of a whole buffer, so
 * short sounds play and latency stays low. */
static void set_start_threshold(snd_pcm_t *pcm)
{
    snd_pcm_uframes_t buf = 0, period = 0;
    snd_pcm_sw_params_t *sw;
    if (snd_pcm_get_params(pcm, &buf, &period) < 0 || period == 0)
        return;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(pcm, sw) < 0 ||
        snd_pcm_sw_params_set_start_threshold(pcm, sw, period) < 0 ||
        snd_pcm_sw_params(pcm, sw) < 0)
        logv("audio: could not set start threshold");
}

static bool try_pcm(audio_t *a, const char *name)
{
    snd_pcm_format_t f = a->fmt == DOCK_AUD_U8 ? SND_PCM_FORMAT_U8 : SND_PCM_FORMAT_S16_LE;
    int rc = snd_pcm_open(&a->pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (rc < 0) {
        logv("audio: open %s: %s", name, snd_strerror(rc));
        a->pcm = NULL;
        return false;
    }
    rc = snd_pcm_set_params(a->pcm, f, SND_PCM_ACCESS_RW_INTERLEAVED, a->channels,
                            a->rate, 1, LATENCY_US);
    if (rc < 0) {
        logv("audio: %s rejects %u Hz x%u: %s", name, a->rate, a->channels,
             snd_strerror(rc));
        snd_pcm_close(a->pcm);
        a->pcm = NULL;
        return false;
    }
    set_start_threshold(a->pcm);
    logv("audio: playing on %s (%u Hz, %u ch, fmt %u)", name, a->rate, a->channels,
         a->fmt);
    return true;
}

/* Open the output for the current format: configured device, then the same
 * through the "plug" converter, then "default". */
static bool dev_open(audio_t *a)
{
    char plug[sizeof a->device + 16];
    dev_close(a);
    a->frames_written = 0;
    a->last_pos_ms = 0;
    if (a->null_backend) {
        a->null_head = 0;
        a->null_last_us = mono_us();
        a->pcm_open = true;
        return true;
    }
    snprintf(plug, sizeof plug, "plug:'%s'", a->device);
    if (try_pcm(a, a->device) ||
        (strncmp(a->device, "plug", 4) != 0 && try_pcm(a, plug)) ||
        (strcmp(a->device, "default") != 0 && try_pcm(a, "default"))) {
        a->pcm_open = true;
        return true;
    }
    loge("audio: no usable output device");
    return false;
}

/* Frames queued in the device and not heard yet. */
static uint64_t dev_delay(audio_t *a)
{
    snd_pcm_sframes_t d = 0;
    if (!a->pcm_open)
        return 0;
    if (a->null_backend) {
        null_advance(a);
        return a->frames_written - a->null_head;
    }
    if (snd_pcm_state(a->pcm) != SND_PCM_STATE_RUNNING &&
        snd_pcm_state(a->pcm) != SND_PCM_STATE_PREPARED &&
        snd_pcm_state(a->pcm) != SND_PCM_STATE_DRAINING)
        return 0; /* xrun: everything written has been played */
    if (snd_pcm_delay(a->pcm, &d) < 0 || d < 0)
        return 0;
    return (uint64_t)d > a->frames_written ? a->frames_written : (uint64_t)d;
}

/* Write up to `frames`; returns frames accepted (0 when the device is
 * full), -1 on a fatal error. */
static long dev_write(audio_t *a, const uint8_t *p, size_t frames)
{
    snd_pcm_sframes_t n;
    if (a->null_backend) {
        uint64_t room, queued;
        null_advance(a);
        queued = a->frames_written - a->null_head;
        room = (uint64_t)a->rate * LATENCY_US / 1000000u;
        room = room > queued ? room - queued : 0;
        if (frames > room)
            frames = (size_t)room;
        a->frames_written += frames;
        return (long)frames;
    }
    p = apply_soft_volume(a, p, frames * a->frame_bytes);
    n = snd_pcm_writei(a->pcm, p, frames);
    if (n == -EAGAIN)
        return 0;
    if (n < 0) {
        /* underrun or suspend: recover and retry once */
        if (snd_pcm_recover(a->pcm, (int)n, 1) < 0)
            return -1;
        n = snd_pcm_writei(a->pcm, p, frames);
        if (n == -EAGAIN)
            return 0;
        if (n < 0)
            return -1;
    }
    /* A file shorter than one period would never reach the start
     * threshold; the pump kicks it off when the source runs dry. */
    a->frames_written += (uint64_t)n;
    return (long)n;
}

static void dev_kick(audio_t *a)
{
    if (!a->null_backend && a->pcm && snd_pcm_state(a->pcm) == SND_PCM_STATE_PREPARED &&
        a->frames_written > 0)
        snd_pcm_start(a->pcm);
}

/* ------------------------------------------------------------------ */
/* sources                                                             */

static void src_reset(audio_t *a)
{
    if (a->fd >= 0)
        close(a->fd);
    a->fd = -1;
    free(a->ring);
    a->ring = NULL;
    a->ring_cap = a->ring_head = a->ring_len = 0;
    a->chunk_off = a->chunk_len = 0;
    a->file_left = 0;
    a->src = SRC_NONE;
}

static bool format_ok(uint32_t rate, uint8_t fmt, uint8_t ch)
{
    return rate >= MIN_RATE && rate <= MAX_RATE && (ch == 1 || ch == 2) &&
           (fmt == DOCK_AUD_U8 || fmt == DOCK_AUD_S16LE);
}

static bool set_format(audio_t *a, uint32_t rate, uint8_t fmt, uint8_t ch)
{
    a->rate = rate;
    a->fmt = fmt;
    a->channels = ch;
    a->frame_bytes = (uint8_t)(ch * (fmt == DOCK_AUD_U8 ? 1u : 2u));
    return dev_open(a);
}

/* Feed the device from the ring buffer. */
static void pump_stream(audio_t *a)
{
    while (a->ring_len >= a->frame_bytes) {
        size_t contig = a->ring_cap - a->ring_head;
        size_t bytes = a->ring_len < contig ? a->ring_len : contig;
        size_t frames = bytes / a->frame_bytes;
        long w;
        if (frames == 0) {
            /* a frame straddles the wrap: realign by moving it */
            uint8_t tmp[4];
            size_t i;
            for (i = 0; i < a->frame_bytes; i++)
                tmp[i] = a->ring[(a->ring_head + i) % a->ring_cap];
            w = dev_write(a, tmp, 1);
        } else {
            if (frames * a->frame_bytes > CHUNK_BYTES)
                frames = CHUNK_BYTES / a->frame_bytes;
            w = dev_write(a, a->ring + a->ring_head, frames);
        }
        if (w <= 0) {
            if (w < 0)
                dev_close(a);
            return;
        }
        a->ring_head = (a->ring_head + (size_t)w * a->frame_bytes) % a->ring_cap;
        a->ring_len -= (size_t)w * a->frame_bytes;
    }
}

/* Feed the device from the WAV file. */
static void pump_file(audio_t *a)
{
    for (;;) {
        long w;
        size_t frames;
        if (a->chunk_off >= a->chunk_len) {
            size_t want = CHUNK_BYTES - CHUNK_BYTES % a->frame_bytes;
            ssize_t n;
            if (a->file_left == 0)
                break;
            if (want > a->file_left)
                want = (size_t)a->file_left;
            n = read(a->fd, a->chunk, want);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) { /* error or truncated file: end of data */
                a->file_left = 0;
                break;
            }
            a->file_left -= (uint64_t)n;
            a->chunk_off = 0;
            a->chunk_len = (size_t)n - (size_t)n % a->frame_bytes;
            if (a->chunk_len == 0)
                continue;
        }
        frames = (a->chunk_len - a->chunk_off) / a->frame_bytes;
        w = dev_write(a, a->chunk + a->chunk_off, frames);
        if (w <= 0) {
            if (w < 0) {
                dev_close(a);
                src_reset(a);
            }
            return;
        }
        a->chunk_off += (size_t)w * a->frame_bytes;
    }
    dev_kick(a);
}

/* ------------------------------------------------------------------ */
/* WAV header                                                          */

static bool read_exact(int fd, void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Parse RIFF/WAVE up to the data chunk. Leaves fd at the first sample. */
static int wav_parse(int fd, uint32_t *rate, uint8_t *fmt, uint8_t *ch,
                     uint64_t *data_len)
{
    uint8_t h[12], c[8], f[40];
    bool have_fmt = false;
    int guard;
    struct stat st;

    if (!read_exact(fd, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
        return DOCK_E_INVALID;
    for (guard = 0; guard < 64; guard++) {
        uint32_t len;
        if (!read_exact(fd, c, 8))
            return DOCK_E_INVALID;
        len = le32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0) {
            uint16_t tag, chans, bits, align;
            if (len < 16 || len > sizeof f || !read_exact(fd, f, len))
                return DOCK_E_INVALID;
            tag = le16(f);
            chans = le16(f + 2);
            *rate = le32(f + 4);
            align = le16(f + 12);
            bits = le16(f + 14);
            /* WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the tag */
            if (tag == 0xFFFE && len >= 26)
                tag = le16(f + 24);
            if (tag != 1)
                return DOCK_E_UNSUPPORTED; /* compressed */
            if ((bits != 8 && bits != 16) || chans < 1 || chans > 2 ||
                align != chans * bits / 8)
                return DOCK_E_UNSUPPORTED;
            *fmt = bits == 8 ? DOCK_AUD_U8 : DOCK_AUD_S16LE;
            *ch = (uint8_t)chans;
            if (len & 1u)
                lseek(fd, 1, SEEK_CUR);
            have_fmt = true;
        } else if (memcmp(c, "data", 4) == 0) {
            off_t pos = lseek(fd, 0, SEEK_CUR);
            if (!have_fmt || pos < 0 || fstat(fd, &st) != 0)
                return DOCK_E_INVALID;
            *data_len = len;
            /* streaming writers leave 0 / 0xFFFFFFFF: trust the file size */
            if (len == 0 || len == 0xFFFFFFFFu || (off_t)len > st.st_size - pos)
                *data_len = st.st_size > pos ? (uint64_t)(st.st_size - pos) : 0;
            return format_ok(*rate, *fmt, *ch) ? DOCK_OK : DOCK_E_UNSUPPORTED;
        } else {
            if (lseek(fd, (off_t)len + (len & 1u), SEEK_CUR) < 0)
                return DOCK_E_INVALID;
        }
    }
    return DOCK_E_INVALID;
}

/* ------------------------------------------------------------------ */
/* public                                                              */

/* alsa-lib prints configuration noise to stderr; keep it for -v only. */
static void alsa_error(const char *file, int line, const char *func, int err,
                       const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    (void)file;
    (void)line;
    (void)err;
    if (!log_is_verbose())
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    logv("alsa: %s: %s", func, msg);
}

audio_t *audio_open(const char *backend, const char *device)
{
    audio_t *a;
    if (strcmp(backend, "alsa") != 0 && strcmp(backend, "null") != 0) {
        loge("unknown audio backend '%s' (alsa, null)", backend);
        return NULL;
    }
    a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->null_backend = strcmp(backend, "null") == 0;
    if (!a->null_backend)
        snd_lib_error_set_handler(alsa_error);
    a->fd = -1;
    a->master_vol = 100;
    a->prog_vol = 100;
    str_copy(a->device, sizeof a->device, device ? device : "default");
    mixer_open(a);
    return a;
}

void audio_close(audio_t *a)
{
    if (!a)
        return;
    dev_close(a);
    src_reset(a);
    mixer_close(a);
    free(a);
}

void audio_set_device(audio_t *a, const char *device)
{
    if (strcmp(device, a->device) == 0)
        return;
    audio_stop(a);
    str_copy(a->device, sizeof a->device, device);
    mixer_open(a);
    mixer_apply(a);
}

int audio_stream_open(audio_t *a, uint32_t rate, uint8_t fmt, uint8_t channels)
{
    size_t cap;
    if (!format_ok(rate, fmt, channels))
        return DOCK_E_INVALID;
    audio_stop(a);
    if (!set_format(a, rate, fmt, channels))
        return DOCK_E_IO;
    cap = (size_t)rate * a->frame_bytes; /* one second */
    a->ring = malloc(cap);
    if (!a->ring) {
        dev_close(a);
        return DOCK_E_NO_SPACE;
    }
    a->ring_cap = cap;
    a->src = SRC_STREAM;
    return DOCK_OK;
}

int audio_stream_write(audio_t *a, const uint8_t *pcm, size_t n, size_t *accepted)
{
    size_t room, take, tail, first;
    *accepted = 0;
    if (a->src != SRC_STREAM)
        return DOCK_E_INVALID;
    room = a->ring_cap - a->ring_len;
    take = (n < room ? n : room);
    take -= take % a->frame_bytes;
    tail = (a->ring_head + a->ring_len) % a->ring_cap;
    first = a->ring_cap - tail < take ? a->ring_cap - tail : take;
    memcpy(a->ring + tail, pcm, first);
    memcpy(a->ring, pcm + first, take - first);
    a->ring_len += take;
    *accepted = take;
    if (!a->pcm_open && !dev_open(a))
        return DOCK_E_IO;
    pump_stream(a);
    return DOCK_OK;
}

int audio_play_wav_fd(audio_t *a, int fd)
{
    uint32_t rate = 0;
    uint8_t fmt = 0, ch = 0;
    uint64_t len = 0;
    int rc = wav_parse(fd, &rate, &fmt, &ch, &len);
    if (rc != DOCK_OK) {
        close(fd);
        return rc;
    }
    audio_stop(a);
    if (!set_format(a, rate, fmt, ch)) {
        close(fd);
        return DOCK_E_IO;
    }
    a->fd = fd;
    a->file_left = len;
    a->src = SRC_FILE;
    pump_file(a);
    return DOCK_OK;
}

static bool source_empty(const audio_t *a)
{
    switch (a->src) {
    case SRC_STREAM:
        return a->ring_len < a->frame_bytes;
    case SRC_FILE:
        return a->file_left == 0 && a->chunk_off >= a->chunk_len;
    default:
        return true;
    }
}

/* Recompute the heard position (monotonic within one playback). */
static bool update_pos(audio_t *a)
{
    uint64_t delay = dev_delay(a), played = a->frames_written - delay, ms;
    ms = played * 1000u / a->rate;
    if (ms > 0xFFFFFFFFu)
        ms = 0xFFFFFFFFu;
    if ((uint32_t)ms > a->last_pos_ms)
        a->last_pos_ms = (uint32_t)ms;
    return a->src != SRC_NONE && (delay > 0 || !source_empty(a));
}

int audio_pos(audio_t *a, uint32_t *pos_ms, bool *playing)
{
    audio_pump(a);
    *playing = a->pcm_open && a->rate && update_pos(a);
    *pos_ms = a->last_pos_ms;
    return DOCK_OK;
}

int audio_stop(audio_t *a)
{
    if (a->pcm_open && a->rate)
        update_pos(a); /* keep the final position readable */
    dev_close(a);
    src_reset(a);
    return DOCK_OK;
}

void audio_set_master_volume(audio_t *a, uint8_t percent)
{
    a->master_vol = percent > 100 ? 100 : percent;
    mixer_apply(a);
}

int audio_set_program_volume(audio_t *a, uint8_t percent)
{
    a->prog_vol = percent > 100 ? 100 : percent;
    return DOCK_OK;
}

void audio_pump(audio_t *a)
{
    if (!a->pcm_open)
        return;
    if (a->src == SRC_STREAM)
        pump_stream(a);
    else if (a->src == SRC_FILE)
        pump_file(a);
    /* A finished file: release the device once everything was heard. */
    if (a->src == SRC_FILE && source_empty(a) && dev_delay(a) == 0) {
        update_pos(a);
        dev_close(a);
        src_reset(a);
    }
}

int audio_timeout_ms(const audio_t *a)
{
    return a->pcm_open ? PUMP_MS : -1;
}
