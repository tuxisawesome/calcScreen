/* sha256.h - SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 (portable C99). */
#ifndef DOCK_SHA256_H
#define DOCK_SHA256_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOCK_SHA256_LEN 32u

typedef struct dock_sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint32_t buf_len;
    uint32_t total_lo, total_hi; /* message length in bytes */
} dock_sha256_t;

void dock_sha256_init(dock_sha256_t *s);
void dock_sha256_update(dock_sha256_t *s, const void *data, size_t n);
void dock_sha256_final(dock_sha256_t *s, uint8_t out[DOCK_SHA256_LEN]);
void dock_sha256(const void *data, size_t n, uint8_t out[DOCK_SHA256_LEN]);

void dock_hmac_sha256(const uint8_t *key, size_t key_len, const void *msg,
                      size_t n, uint8_t out[DOCK_SHA256_LEN]);

/* PBKDF2-HMAC-SHA256 producing out_len bytes. */
void dock_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt,
                        size_t salt_len, uint32_t iterations, uint8_t *out,
                        size_t out_len);

/* Constant-time comparison. */
bool dock_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_SHA256_H */
