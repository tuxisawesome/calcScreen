/* test_settings.c - record codec, A/B store (incl. power loss) and passwords. */
#include "../core/dock_settings.h"
#include "test.h"

/* ---- fake slot storage ---- */

enum tear_mode {
    TEAR_NONE = 0,
    TEAR_TRUNCATE, /* file rewritten from scratch: only k bytes survive */
    TEAR_INPLACE,  /* flash/EEPROM overwrite: first k bytes new, rest old */
    TEAR_FLIP      /* full write, but byte k is corrupted */
};

typedef struct store {
    uint8_t slot[2][DOCK_SETTINGS_MAX_RECORD + 16];
    size_t len[2];
    int tear;
    size_t k;
    bool fail;
    int writes;
    int last_slot;
} store_t;

static size_t st_read(void *user, int slot, uint8_t *buf, size_t cap)
{
    store_t *s = (store_t *)user;
    size_t n = s->len[slot];
    if (n > cap)
        n = cap;
    memcpy(buf, s->slot[slot], n);
    return n;
}

static bool st_write(void *user, int slot, const uint8_t *buf, size_t n)
{
    store_t *s = (store_t *)user;
    s->writes++;
    s->last_slot = slot;
    if (s->fail)
        return false;
    switch (s->tear) {
    case TEAR_TRUNCATE:
        memcpy(s->slot[slot], buf, s->k);
        s->len[slot] = s->k;
        s->tear = TEAR_NONE;
        return false; /* power died */
    case TEAR_INPLACE:
        memcpy(s->slot[slot], buf, s->k);
        if (s->len[slot] < s->k)
            s->len[slot] = s->k;
        s->tear = TEAR_NONE;
        return false;
    case TEAR_FLIP:
        memcpy(s->slot[slot], buf, n);
        s->len[slot] = n;
        s->slot[slot][s->k] ^= 0x10;
        s->tear = TEAR_NONE;
        return true;
    default:
        memcpy(s->slot[slot], buf, n);
        s->len[slot] = n;
        return true;
    }
}

static dock_store_io_t io_for(store_t *s)
{
    dock_store_io_t io;
    io.user = s;
    io.read = st_read;
    io.write = st_write;
    return io;
}

static bool same(const dock_settings_t *a, const dock_settings_t *b)
{
    return a->wifi_enabled == b->wifi_enabled &&
           a->net_api_enabled == b->net_api_enabled &&
           a->join_required == b->join_required && a->audio_out == b->audio_out &&
           a->volume == b->volume && strcmp(a->join_code, b->join_code) == 0 &&
           a->dock_id == b->dock_id && a->admin_set == b->admin_set &&
           memcmp(a->admin_salt, b->admin_salt, sizeof a->admin_salt) == 0 &&
           memcmp(a->admin_hash, b->admin_hash, sizeof a->admin_hash) == 0 &&
           a->admin_iterations == b->admin_iterations &&
           strcmp(a->wifi_ssid, b->wifi_ssid) == 0;
}

static void variant(dock_settings_t *s, uint32_t n)
{
    dock_settings_defaults(s, 0xC0FFEE00u);
    s->volume = (uint8_t)(n % 101u);
    s->wifi_enabled = (uint8_t)(n & 1u);
    s->net_api_enabled = (uint8_t)((n >> 1) & 1u);
    s->join_required = (uint8_t)((n >> 2) & 1u);
    s->audio_out = (uint8_t)((n >> 3) & 1u);
    s->dock_id = 0x1000u + n;
    dock_settings_new_join_code(s, n * 7919u);
    memset(s->wifi_ssid, 0, sizeof s->wifi_ssid);
    memcpy(s->wifi_ssid, "net-", 4);
    s->wifi_ssid[4] = (char)('a' + (char)(n % 26u));
}

/* ---- tests ---- */

static void t_crc32(void)
{
    CHECK_EQ(dock_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
    CHECK_EQ(dock_crc32(NULL, 0), 0);
    CHECK_EQ(dock_crc32((const uint8_t *)"a", 1), 0xE8B7BE43u);
}

static void t_defaults(void)
{
    dock_settings_t s;
    size_t i;
    dock_settings_defaults(&s, 0x12345678u);
    CHECK_EQ(s.wifi_enabled, 1);
    CHECK_EQ(s.net_api_enabled, 0);
    CHECK_EQ(s.join_required, 0);
    CHECK_EQ(s.audio_out, DOCK_AUDIO_HDMI);
    CHECK_EQ(s.volume, 80);
    CHECK_EQ(s.dock_id, 0x12345679u);
    CHECK_EQ(s.admin_set, 0);
    CHECK_EQ(strlen(s.join_code), 6);
    for (i = 0; i < 6; i++)
        CHECK(s.join_code[i] >= '0' && s.join_code[i] <= '9');
    CHECK_STR(s.wifi_ssid, "");
    dock_settings_defaults(&s, 0);
    CHECK_EQ(s.dock_id, 1); /* never 0 */

    dock_settings_new_join_code(&s, 123);
    CHECK_STR(s.join_code, "000123");
    dock_settings_new_join_code(&s, 0xFFFFFFFFu);
    CHECK_STR(s.join_code, "967295");
}

static void t_codec(void)
{
    dock_settings_t a, b;
    uint8_t buf[DOCK_SETTINGS_MAX_RECORD + 8];
    uint32_t seq = 0;
    size_t n;
    uint8_t salt[DOCK_PW_SALT_LEN];
    memset(salt, 0x5A, sizeof salt);

    variant(&a, 13);
    memset(a.admin_hash, 0x77, sizeof a.admin_hash);
    memcpy(a.admin_salt, salt, sizeof salt);
    a.admin_set = 1;
    a.admin_iterations = 12345;
    memcpy(a.wifi_ssid, "0123456789abcdef0123456789ABCDEF", 33); /* 32 chars */
    n = dock_settings_encode(&a, 0xA1B2C3D4u, buf, sizeof buf);
    CHECK(n > 0);
    CHECK_MEM(buf, "DKST", 4);
    CHECK_EQ(dock_get_u32(buf + 4), 0xA1B2C3D4u);
    CHECK_EQ(dock_get_u16(buf + 8), n - 14);
    CHECK_EQ(buf[10], DOCK_SETTINGS_VERSION);
    CHECK_EQ(dock_get_u32(buf + n - 4), dock_crc32(buf, n - 4));
    memset(&b, 0xEE, sizeof b);
    CHECK(dock_settings_decode(buf, n, &b, &seq));
    CHECK_EQ(seq, 0xA1B2C3D4u);
    CHECK(same(&a, &b));

    /* Too small a buffer: refused. */
    CHECK_EQ(dock_settings_encode(&a, 1, buf, n - 1), 0);
    CHECK_EQ(dock_settings_encode(&a, 1, buf, 10), 0);
    n = dock_settings_encode(&a, 1, buf, sizeof buf);

    /* Truncated, bad magic, bad crc. */
    CHECK(!dock_settings_decode(buf, n - 1, &b, &seq));
    CHECK(!dock_settings_decode(buf, 3, &b, &seq));
    buf[0] = 'X';
    CHECK(!dock_settings_decode(buf, n, &b, &seq));
    buf[0] = 'D';
    buf[20] ^= 1;
    CHECK(!dock_settings_decode(buf, n, &b, &seq));
    buf[20] ^= 1;
    CHECK(dock_settings_decode(buf, n, &b, &seq));
    /* Trailing garbage after a valid record is fine. */
    buf[n] = 0x99;
    CHECK(dock_settings_decode(buf, n + 1, &b, &seq));

    /* Version 0 is never valid. */
    {
        uint8_t v0[DOCK_SETTINGS_MAX_RECORD];
        memcpy(v0, buf, n);
        v0[10] = 0;
        dock_put_u32(v0 + n - 4, dock_crc32(v0, n - 4));
        CHECK(!dock_settings_decode(v0, n, &b, &seq));
    }

    /* Forward compatibility: a "version 2" record with extra trailing body
     * fields still loads. */
    {
        uint8_t v2[DOCK_SETTINGS_MAX_RECORD];
        size_t body = n - 14, m;
        memcpy(v2, buf, n - 4);
        v2[10] = 2;
        memcpy(v2 + n - 4, "\x01\x02\x03\x04\x05", 5);
        dock_put_u16(v2 + 8, (uint16_t)(body + 5));
        m = n - 4 + 5;
        dock_put_u32(v2 + m, dock_crc32(v2, m));
        memset(&b, 0, sizeof b);
        CHECK(dock_settings_decode(v2, m + 4, &b, &seq));
        CHECK(same(&a, &b));
    }

    /* Out-of-range values: volume clamped, bad join code rejected. */
    {
        dock_settings_t c = a;
        c.volume = 250;
        n = dock_settings_encode(&c, 1, buf, sizeof buf);
        CHECK(dock_settings_decode(buf, n, &b, &seq));
        CHECK_EQ(b.volume, 100);
        c.join_code[2] = 'x';
        n = dock_settings_encode(&c, 1, buf, sizeof buf);
        CHECK(!dock_settings_decode(buf, n, &b, &seq));
    }
}

static void t_store_basic(void)
{
    static store_t st;
    dock_store_io_t io = io_for(&st);
    dock_settings_t a, b, c;
    uint32_t seq = 0, lseq = 0;

    memset(&st, 0, sizeof st);
    memset(&c, 0x42, sizeof c);
    b = c;
    CHECK(!dock_settings_load(&io, &b, &lseq));
    CHECK(memcmp(&b, &c, sizeof b) == 0); /* untouched */

    variant(&a, 1);
    CHECK(dock_settings_save(&io, &a, &seq));
    CHECK_EQ(seq, 1);
    CHECK_EQ(st.last_slot, 0);
    CHECK(dock_settings_load(&io, &b, &lseq));
    CHECK_EQ(lseq, 1);
    CHECK(same(&a, &b));

    variant(&a, 2);
    CHECK(dock_settings_save(&io, &a, &seq));
    CHECK_EQ(seq, 2);
    CHECK_EQ(st.last_slot, 1);
    variant(&a, 3);
    CHECK(dock_settings_save(&io, &a, &seq));
    CHECK_EQ(st.last_slot, 0);
    CHECK(dock_settings_load(&io, &b, &lseq));
    CHECK_EQ(lseq, 3);
    CHECK(same(&a, &b));

    /* A stale caller seq never overwrites the newest record. */
    seq = 0;
    variant(&a, 4);
    CHECK(dock_settings_save(&io, &a, &seq));
    CHECK_EQ(seq, 4);
    CHECK_EQ(st.last_slot, 1);
    CHECK(dock_settings_load(&io, &b, &lseq));
    CHECK_EQ(lseq, 4);
    CHECK(same(&a, &b));

    /* Write failure: seq unchanged, old data still loads. */
    st.fail = true;
    variant(&c, 5);
    CHECK(!dock_settings_save(&io, &c, &seq));
    CHECK_EQ(seq, 4);
    st.fail = false;
    CHECK(dock_settings_load(&io, &b, &lseq));
    CHECK(same(&a, &b));

    /* Sequence wrap-around: 0xFFFFFFFF -> 0 is "newer". */
    memset(&st, 0, sizeof st);
    seq = 0xFFFFFFFEu;
    variant(&a, 6);
    CHECK(dock_settings_save(&io, &a, &seq));
    CHECK_EQ(seq, 0xFFFFFFFFu);
    variant(&a, 7);
    CHECK(dock_settings_save(&io, &a, &seq));
    CHECK_EQ(seq, 0);
    CHECK(dock_settings_load(&io, &b, &lseq));
    CHECK_EQ(lseq, 0);
    CHECK(same(&a, &b));
    variant(&a, 8);
    CHECK(dock_settings_save(&io, &a, &seq));
    CHECK_EQ(seq, 1);
    CHECK(dock_settings_load(&io, &b, &lseq));
    CHECK(same(&a, &b));

    /* One slot corrupt, the other valid: load the valid one, save into the
     * corrupt one. */
    st.slot[0][12] ^= 0xFF; /* slot 0 holds seq 1 (newest), slot 1 seq 0 */
    CHECK(dock_settings_load(&io, &b, &lseq));
    CHECK_EQ(lseq, 0);
    variant(&a, 9);
    CHECK(dock_settings_save(&io, &a, &seq));
    CHECK_EQ(st.last_slot, 0);
    CHECK_EQ(seq, 2);
    CHECK(dock_settings_load(&io, &b, &lseq));
    CHECK(same(&a, &b));
}

/* Power cut at every byte offset of a save, for each tear model. */
static void t_store_power_loss(void)
{
    static store_t st;
    dock_store_io_t io = io_for(&st);
    dock_settings_t prev, next, got;
    uint8_t rec[DOCK_SETTINGS_MAX_RECORD];
    size_t n, k;
    int mode, round;
    unsigned failures = 0, cases = 0;

    variant(&next, 99);
    n = dock_settings_encode(&next, 1, rec, sizeof rec);
    CHECK(n > 0);

    for (mode = TEAR_TRUNCATE; mode <= TEAR_FLIP; mode++) {
        for (round = 0; round < 2; round++) {
            for (k = 0; k < n; k++) {
                uint32_t seq = 0, lseq = 0, before;
                int i;
                memset(&st, 0, sizeof st);
                /* round 0: tear the first save over an empty slot 1 after one
                 * good save; round 1: several saves first so the torn slot
                 * holds an older valid record. */
                for (i = 0; i < (round ? 3 : 1); i++) {
                    variant(&prev, (uint32_t)(10 + i));
                    CHECK(dock_settings_save(&io, &prev, &seq));
                }
                before = seq;
                st.tear = mode;
                st.k = k;
                (void)dock_settings_save(&io, &next, &seq);
                cases++;
                if (!dock_settings_load(&io, &got, &lseq) || !same(&got, &prev) ||
                    lseq != before) {
                    failures++;
                    if (failures < 5)
                        fprintf(stderr, "  power loss: mode %d round %d k=%lu\n",
                                mode, round, (unsigned long)k);
                }
            }
        }
    }
    CHECK_EQ(failures, 0);
    CHECK(cases > 3u * 2u * 80u);

    /* A complete write (k == n) loads the new settings. */
    {
        uint32_t seq = 0, lseq = 0;
        memset(&st, 0, sizeof st);
        variant(&prev, 1);
        CHECK(dock_settings_save(&io, &prev, &seq));
        st.tear = TEAR_TRUNCATE;
        st.k = n;
        (void)dock_settings_save(&io, &next, &seq);
        CHECK(dock_settings_load(&io, &got, &lseq));
        CHECK(same(&got, &next));
        CHECK_EQ(lseq, 2);
    }

    /* Random multi-byte corruption of the slot being written. */
    {
        uint32_t rs = 0xBADC0DEu;
        int t;
        failures = 0;
        for (t = 0; t < 500; t++) {
            uint32_t seq = 0, lseq = 0;
            int j, nbytes = 1 + (int)(t_rand(&rs) % 8u);
            memset(&st, 0, sizeof st);
            variant(&prev, (uint32_t)t);
            CHECK(dock_settings_save(&io, &prev, &seq));
            CHECK(dock_settings_save(&io, &prev, &seq));
            CHECK(dock_settings_save(&io, &next, &seq));
            for (j = 0; j < nbytes; j++) {
                size_t pos = t_rand(&rs) % n;
                st.slot[st.last_slot][pos] ^= (uint8_t)(1u + t_rand(&rs) % 255u);
            }
            if (!dock_settings_load(&io, &got, &lseq) || !same(&got, &prev) ||
                lseq != 2)
                failures++;
        }
        CHECK_EQ(failures, 0);
    }
}

static void t_password(void)
{
    dock_settings_t s, t;
    uint8_t salt[DOCK_PW_SALT_LEN], buf[DOCK_SETTINGS_MAX_RECORD];
    uint32_t seq;
    size_t i, n;
    for (i = 0; i < sizeof salt; i++)
        salt[i] = (uint8_t)(i * 13u);
    dock_settings_defaults(&s, 42);
    CHECK(dock_settings_check_password(&s, "anything")); /* no password set */

    dock_settings_set_password(&s, "hunter2", salt);
    CHECK_EQ(s.admin_set, 1);
    CHECK_EQ(s.admin_iterations, DOCK_PW_ITERATIONS);
    CHECK_MEM(s.admin_salt, salt, sizeof salt);
    {
        uint8_t want[DOCK_SHA256_LEN];
        dock_pbkdf2_sha256((const uint8_t *)"hunter2", 7, salt, sizeof salt,
                           DOCK_PW_ITERATIONS, want, sizeof want);
        CHECK_MEM(s.admin_hash, want, sizeof want);
    }
    CHECK(dock_settings_check_password(&s, "hunter2"));
    CHECK(!dock_settings_check_password(&s, "hunter3"));
    CHECK(!dock_settings_check_password(&s, ""));
    CHECK(!dock_settings_check_password(&s, NULL));

    /* Survives the record codec. */
    n = dock_settings_encode(&s, 7, buf, sizeof buf);
    CHECK(dock_settings_decode(buf, n, &t, &seq));
    CHECK(dock_settings_check_password(&t, "hunter2"));
    CHECK(!dock_settings_check_password(&t, "Hunter2"));

    /* Clearing. */
    dock_settings_set_password(&s, "", salt);
    CHECK_EQ(s.admin_set, 0);
    CHECK(dock_settings_check_password(&s, "whatever"));
}

void suite_settings(void)
{
    RUN(t_crc32);
    RUN(t_defaults);
    RUN(t_codec);
    RUN(t_store_basic);
    RUN(t_store_power_loss);
    RUN(t_password);
}
