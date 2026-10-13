/*
 * dock_settings.c - settings record codec and power-loss-safe A/B store.
 *
 * Record layout (all multi-byte fields little-endian):
 *
 *   offset    size  field
 *   0         4     magic "DKST"
 *   4         4     seq          save counter, newest valid slot wins
 *   8         2     body_len
 *   10        n     body         see below
 *   10+n      4     crc32        IEEE CRC-32 over bytes 0 .. 10+n-1
 *
 * Body, version 1 (fields are only ever appended; a decoder reads the fields
 * it knows and skips the rest, so old firmware can load newer records):
 *
 *   u8 version, u8 wifi_enabled, u8 net_api_enabled, u8 join_required,
 *   u8 audio_out, u8 volume, char join_code[6], u32 dock_id, u8 admin_set,
 *   u8 admin_salt[16], u8 admin_hash[32], u32 admin_iterations,
 *   u8 ssid_len, char ssid[ssid_len]
 *
 * Body additions in version 2 (read only when present, so v1 records still
 * load and get the defaults):
 *
 *   u8 perf_overlay, u8 perf_overlay_sections
 */
#include "dock_settings.h"
#include "dock_proto.h"

#define REC_MAGIC "DKST"
#define REC_HDR_LEN 10u
#define REC_CRC_LEN 4u
#define BODY_V1_MIN 70u /* body bytes up to and including ssid_len */

uint32_t dock_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        uint8_t i;
        crc ^= *p++;
        for (i = 0; i < 8; i++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}

static void join_code_from(char out[7], uint32_t v)
{
    uint8_t i;
    v %= 1000000u;
    for (i = 6; i > 0; i--) {
        out[i - 1] = (char)('0' + (char)(v % 10u));
        v /= 10u;
    }
    out[6] = '\0';
}

void dock_settings_new_join_code(dock_settings_t *s, uint32_t random32)
{
    join_code_from(s->join_code, random32);
}

void dock_settings_defaults(dock_settings_t *s, uint32_t random32)
{
    /* dock_id is shown to every calculator (WELCOME), so the join code must
     * not be a plain function of it: derive it through a separate hash. The
     * platform should still call dock_settings_new_join_code() with fresh
     * randomness when it has more than 32 bits available. */
    uint8_t seed[8], h[DOCK_SHA256_LEN];
    memset(s, 0, sizeof *s);
    s->wifi_enabled = 1;
    s->net_api_enabled = 0;
    s->join_required = 0;
    s->audio_out = DOCK_AUDIO_HDMI;
    s->volume = 80;
    s->dock_id = random32 | 1u;
    s->perf_overlay = 0;
    s->perf_overlay_sections = DOCK_OVL_ALL;
    memcpy(seed, "join", 4);
    dock_put_u32(seed + 4, random32);
    dock_sha256(seed, sizeof seed, h);
    join_code_from(s->join_code, dock_get_u32(h));
}

/* ---- codec ---- */

size_t dock_settings_encode(const dock_settings_t *s, uint32_t seq,
                            uint8_t *buf, size_t cap)
{
    dock_wr_t w;
    size_t ssid_len = strlen(s->wifi_ssid);
    size_t body_len;
    if (ssid_len > sizeof s->wifi_ssid - 1)
        ssid_len = sizeof s->wifi_ssid - 1;

    dock_wr_init(&w, buf, cap);
    dock_wr_bytes(&w, REC_MAGIC, 4);
    dock_wr_u32(&w, seq);
    dock_wr_u16(&w, 0); /* body_len, patched below */

    dock_wr_u8(&w, DOCK_SETTINGS_VERSION);
    dock_wr_u8(&w, s->wifi_enabled ? 1u : 0u);
    dock_wr_u8(&w, s->net_api_enabled ? 1u : 0u);
    dock_wr_u8(&w, s->join_required ? 1u : 0u);
    dock_wr_u8(&w, s->audio_out);
    dock_wr_u8(&w, s->volume);
    dock_wr_bytes(&w, s->join_code, 6);
    dock_wr_u32(&w, s->dock_id);
    dock_wr_u8(&w, s->admin_set ? 1u : 0u);
    dock_wr_bytes(&w, s->admin_salt, DOCK_PW_SALT_LEN);
    dock_wr_bytes(&w, s->admin_hash, DOCK_SHA256_LEN);
    dock_wr_u32(&w, s->admin_iterations);
    dock_wr_u8(&w, (uint8_t)ssid_len);
    dock_wr_bytes(&w, s->wifi_ssid, ssid_len);
    dock_wr_u8(&w, s->perf_overlay ? 1u : 0u);       /* v2 */
    dock_wr_u8(&w, s->perf_overlay_sections);        /* v2 */

    if (w.overflow || w.len + REC_CRC_LEN > cap)
        return 0;
    body_len = w.len - REC_HDR_LEN;
    dock_put_u16(buf + 8, (uint16_t)body_len);
    dock_wr_u32(&w, dock_crc32(buf, w.len));
    return w.overflow ? 0 : w.len;
}

static bool join_code_valid(const char *c)
{
    uint8_t i;
    for (i = 0; i < 6; i++)
        if (c[i] < '0' || c[i] > '9')
            return false;
    return true;
}

bool dock_settings_decode(const uint8_t *buf, size_t n, dock_settings_t *s,
                          uint32_t *seq)
{
    dock_settings_t t;
    dock_rd_t r;
    uint16_t body_len;
    uint8_t version, ssid_len;

    if (n < REC_HDR_LEN + REC_CRC_LEN || memcmp(buf, REC_MAGIC, 4) != 0)
        return false;
    body_len = dock_get_u16(buf + 8);
    if ((size_t)body_len > n - REC_HDR_LEN - REC_CRC_LEN)
        return false;
    if (dock_crc32(buf, REC_HDR_LEN + (size_t)body_len) !=
        dock_get_u32(buf + REC_HDR_LEN + body_len))
        return false;

    memset(&t, 0, sizeof t);
    dock_rd_init(&r, buf + REC_HDR_LEN, body_len);
    version = dock_rd_u8(&r);
    if (version < 1u || body_len < BODY_V1_MIN)
        return false;
    t.wifi_enabled = dock_rd_u8(&r) ? 1u : 0u;
    t.net_api_enabled = dock_rd_u8(&r) ? 1u : 0u;
    t.join_required = dock_rd_u8(&r) ? 1u : 0u;
    t.audio_out = dock_rd_u8(&r);
    t.volume = dock_rd_u8(&r);
    if (dock_rd_left(&r) < 6)
        return false;
    memcpy(t.join_code, dock_rd_ptr(&r), 6);
    r.pos += 6;
    t.join_code[6] = '\0';
    t.dock_id = dock_rd_u32(&r);
    t.admin_set = dock_rd_u8(&r) ? 1u : 0u;
    if (dock_rd_left(&r) < DOCK_PW_SALT_LEN + DOCK_SHA256_LEN)
        return false;
    memcpy(t.admin_salt, dock_rd_ptr(&r), DOCK_PW_SALT_LEN);
    r.pos += DOCK_PW_SALT_LEN;
    memcpy(t.admin_hash, dock_rd_ptr(&r), DOCK_SHA256_LEN);
    r.pos += DOCK_SHA256_LEN;
    t.admin_iterations = dock_rd_u32(&r);
    ssid_len = dock_rd_u8(&r);
    if (r.underflow || ssid_len > sizeof t.wifi_ssid - 1 ||
        dock_rd_left(&r) < ssid_len)
        return false;
    memcpy(t.wifi_ssid, dock_rd_ptr(&r), ssid_len);
    t.wifi_ssid[ssid_len] = '\0';

    /* v2 fields: present only when version >= 2 and the bytes are there.
     * A v1 record (or a truncated v2 body) keeps the defaults set below. */
    t.perf_overlay = 0;
    t.perf_overlay_sections = DOCK_OVL_ALL;
    if (version >= 2u && dock_rd_left(&r) >= 2u) {
        t.perf_overlay = dock_rd_u8(&r) ? 1u : 0u;
        t.perf_overlay_sections = dock_rd_u8(&r);
    }

    if (!join_code_valid(t.join_code) || t.audio_out > DOCK_AUDIO_JACK)
        return false;
    if (t.volume > 100)
        t.volume = 100;
    *s = t;
    if (seq)
        *seq = dock_get_u32(buf + 4);
    return true;
}

/* ---- A/B store ---- */

/* Serial-number comparison: true if a is newer than b (wraps safely). */
static bool seq_newer(uint32_t a, uint32_t b)
{
    return a != b && (uint32_t)(a - b) < 0x80000000u;
}

typedef struct slot_info {
    bool valid;
    uint32_t seq;
    dock_settings_t s;
} slot_info_t;

static void read_slot(const dock_store_io_t *io, int slot, slot_info_t *out)
{
    uint8_t buf[DOCK_SETTINGS_MAX_RECORD];
    size_t n = io->read(io->user, slot, buf, sizeof buf);
    out->valid = n > 0 && n <= sizeof buf &&
                 dock_settings_decode(buf, n, &out->s, &out->seq);
}

/* Index of the newest valid slot, -1 if none. */
static int newest(const slot_info_t sl[2])
{
    if (sl[0].valid && sl[1].valid)
        return seq_newer(sl[1].seq, sl[0].seq) ? 1 : 0;
    if (sl[0].valid)
        return 0;
    if (sl[1].valid)
        return 1;
    return -1;
}

bool dock_settings_load(const dock_store_io_t *io, dock_settings_t *s,
                        uint32_t *seq)
{
    slot_info_t sl[2];
    int best;
    read_slot(io, 0, &sl[0]);
    read_slot(io, 1, &sl[1]);
    best = newest(sl);
    if (best < 0)
        return false;
    *s = sl[best].s;
    if (seq)
        *seq = sl[best].seq;
    return true;
}

bool dock_settings_save(const dock_store_io_t *io, const dock_settings_t *s,
                        uint32_t *seq)
{
    uint8_t buf[DOCK_SETTINGS_MAX_RECORD];
    slot_info_t sl[2];
    uint32_t next = *seq + 1u;
    int best, target;
    size_t n;

    /* Never overwrite the newest valid record, whatever the caller thinks
     * the current sequence number is. */
    read_slot(io, 0, &sl[0]);
    read_slot(io, 1, &sl[1]);
    best = newest(sl);
    target = best == 0 ? 1 : 0;
    if (best >= 0 && !seq_newer(next, sl[best].seq))
        next = sl[best].seq + 1u;

    n = dock_settings_encode(s, next, buf, sizeof buf);
    if (n == 0 || !io->write(io->user, target, buf, n))
        return false;
    *seq = next;
    return true;
}

/* ---- admin password ---- */

void dock_settings_set_password(dock_settings_t *s, const char *pw,
                                const uint8_t salt[DOCK_PW_SALT_LEN])
{
    if (!pw || !pw[0]) {
        s->admin_set = 0;
        s->admin_iterations = 0;
        memset(s->admin_salt, 0, sizeof s->admin_salt);
        memset(s->admin_hash, 0, sizeof s->admin_hash);
        return;
    }
    memcpy(s->admin_salt, salt, DOCK_PW_SALT_LEN);
    s->admin_iterations = DOCK_PW_ITERATIONS;
    dock_pbkdf2_sha256((const uint8_t *)pw, strlen(pw), s->admin_salt,
                       DOCK_PW_SALT_LEN, s->admin_iterations, s->admin_hash,
                       DOCK_SHA256_LEN);
    s->admin_set = 1;
}

/* Returns true when no password is set (the settings page is unlocked). */
bool dock_settings_check_password(const dock_settings_t *s, const char *pw)
{
    uint8_t h[DOCK_SHA256_LEN];
    bool ok;
    if (!s->admin_set)
        return true;
    if (!pw || s->admin_iterations == 0)
        return false;
    dock_pbkdf2_sha256((const uint8_t *)pw, strlen(pw), s->admin_salt,
                       DOCK_PW_SALT_LEN, s->admin_iterations, h, sizeof h);
    ok = dock_ct_equal(h, s->admin_hash, sizeof h);
    memset(h, 0, sizeof h);
    return ok;
}
