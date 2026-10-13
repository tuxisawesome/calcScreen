/*
 * dock_settings.h - the dock's persistent settings and a power-loss-safe
 * A/B store.
 *
 * Two slots hold encoded records {magic, seq, len, body, crc32}. Saving always
 * overwrites the *older* slot with seq+1, so a power cut mid-write leaves the
 * other slot intact; loading picks the valid record with the highest seq.
 * Slot I/O is abstracted (files, flash pages, EEPROM...).
 */
#ifndef DOCK_SETTINGS_H
#define DOCK_SETTINGS_H

#include "dock_common.h"
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOCK_SETTINGS_VERSION 2u
#define DOCK_PW_SALT_LEN 16u
#define DOCK_PW_ITERATIONS 20000u
#define DOCK_SETTINGS_MAX_RECORD 512u

enum dock_audio_out { DOCK_AUDIO_HDMI = 0, DOCK_AUDIO_JACK = 1 };

/* Performance-overlay section bits (perf_overlay_sections). */
#define DOCK_OVL_FPS 0x01u  /* FPS, throughput, rows/frame, link mode */
#define DOCK_OVL_CONN 0x02u /* calc id, driver, vid/pid, join, resyncs */
#define DOCK_OVL_SYS 0x04u  /* CPU, temperature, memory, uptime, version */
#define DOCK_OVL_ALL (DOCK_OVL_FPS | DOCK_OVL_CONN | DOCK_OVL_SYS)

typedef struct dock_settings {
    uint8_t wifi_enabled;     /* radio on */
    uint8_t net_api_enabled;  /* calc programs may use NET_* */
    uint8_t join_required;    /* calcs must present the join code */
    uint8_t audio_out;        /* enum dock_audio_out */
    uint8_t volume;           /* 0..100 */
    char join_code[7];        /* 6 digits + NUL */
    uint32_t dock_id;         /* random, identifies this dock to calcs */
    uint8_t admin_set;        /* a password protects the settings page */
    uint8_t admin_salt[DOCK_PW_SALT_LEN];
    uint8_t admin_hash[DOCK_SHA256_LEN];
    uint32_t admin_iterations;
    char wifi_ssid[33];       /* last network chosen (password lives in NM) */
    /* v2 fields (appended; absent in v1 records, which load the defaults). */
    uint8_t perf_overlay;          /* diagnostics performance overlay on */
    uint8_t perf_overlay_sections; /* DOCK_OVL_* bitmask of enabled groups */
} dock_settings_t;

typedef struct dock_store_io {
    void *user;
    /* Read slot (0 or 1). Return bytes read, 0 if empty/unreadable. */
    size_t (*read)(void *user, int slot, uint8_t *buf, size_t cap);
    /* Write slot durably (e.g. write + fsync). Return false on failure. */
    bool (*write)(void *user, int slot, const uint8_t *buf, size_t n);
} dock_store_io_t;

uint32_t dock_crc32(const uint8_t *p, size_t n);

/* Factory defaults; `random32` seeds dock_id and the join code. */
void dock_settings_defaults(dock_settings_t *s, uint32_t random32);

/* Record codec (exposed for tests). */
size_t dock_settings_encode(const dock_settings_t *s, uint32_t seq,
                            uint8_t *buf, size_t cap);
bool dock_settings_decode(const uint8_t *buf, size_t n, dock_settings_t *s,
                          uint32_t *seq);

/* Load the newest valid slot. Returns false (and leaves *s untouched) if
 * neither slot is valid. *seq receives the loaded sequence number. */
bool dock_settings_load(const dock_store_io_t *io, dock_settings_t *s,
                        uint32_t *seq);
/* Save into the older slot with *seq + 1; updates *seq on success. */
bool dock_settings_save(const dock_store_io_t *io, const dock_settings_t *s,
                        uint32_t *seq);

/* Admin password (empty string clears it). */
void dock_settings_set_password(dock_settings_t *s, const char *pw,
                                const uint8_t salt[DOCK_PW_SALT_LEN]);
bool dock_settings_check_password(const dock_settings_t *s, const char *pw);

/* New random 6-digit join code. */
void dock_settings_new_join_code(dock_settings_t *s, uint32_t random32);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_SETTINGS_H */
