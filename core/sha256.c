/* sha256.c - SHA-256 (FIPS 180-4), HMAC (RFC 2104) and PBKDF2 (RFC 8018). */
#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32u - (n))))

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void compress(uint32_t h[8], const uint8_t blk[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
    uint8_t i;
    for (i = 0; i < 16; i++)
        w[i] = get_be32(blk + 4u * i);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        t1 = hh + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) +
             K[i] + w[i];
        t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void dock_sha256_init(dock_sha256_t *s)
{
    s->h[0] = 0x6a09e667u;
    s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u;
    s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu;
    s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu;
    s->h[7] = 0x5be0cd19u;
    s->buf_len = 0;
    s->total_lo = 0;
    s->total_hi = 0;
}

void dock_sha256_update(dock_sha256_t *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    while (n) {
        size_t take = 64u - (size_t)s->buf_len;
        uint32_t old;
        if (take > n)
            take = n;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += (uint32_t)take;
        p += take;
        n -= take;
        old = s->total_lo;
        s->total_lo += (uint32_t)take;
        if (s->total_lo < old)
            s->total_hi++;
        if (s->buf_len == 64u) {
            compress(s->h, s->buf);
            s->buf_len = 0;
        }
    }
}

void dock_sha256_final(dock_sha256_t *s, uint8_t out[DOCK_SHA256_LEN])
{
    /* Bit length = byte length * 8, as a 64-bit big-endian number. */
    uint32_t bits_hi = (s->total_hi << 3) | (s->total_lo >> 29);
    uint32_t bits_lo = s->total_lo << 3;
    uint8_t i;

    s->buf[s->buf_len++] = 0x80u;
    if (s->buf_len > 56u) {
        memset(s->buf + s->buf_len, 0, 64u - (size_t)s->buf_len);
        compress(s->h, s->buf);
        s->buf_len = 0;
    }
    memset(s->buf + s->buf_len, 0, 56u - (size_t)s->buf_len);
    put_be32(s->buf + 56, bits_hi);
    put_be32(s->buf + 60, bits_lo);
    compress(s->h, s->buf);
    for (i = 0; i < 8; i++)
        put_be32(out + 4u * i, s->h[i]);
    memset(s, 0, sizeof *s);
}

void dock_sha256(const void *data, size_t n, uint8_t out[DOCK_SHA256_LEN])
{
    dock_sha256_t s;
    dock_sha256_init(&s);
    dock_sha256_update(&s, data, n);
    dock_sha256_final(&s, out);
}

/* ---- HMAC ---- */

typedef struct hmac_ctx {
    dock_sha256_t inner, outer;
} hmac_ctx_t;

static void hmac_init(hmac_ctx_t *c, const uint8_t *key, size_t key_len)
{
    uint8_t k[64], pad[64];
    uint8_t i;
    memset(k, 0, sizeof k);
    if (key_len > 64u)
        dock_sha256(key, key_len, k);
    else if (key_len)
        memcpy(k, key, key_len);
    for (i = 0; i < 64; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x36u);
    dock_sha256_init(&c->inner);
    dock_sha256_update(&c->inner, pad, sizeof pad);
    for (i = 0; i < 64; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    dock_sha256_init(&c->outer);
    dock_sha256_update(&c->outer, pad, sizeof pad);
    memset(k, 0, sizeof k);
    memset(pad, 0, sizeof pad);
}

static void hmac_final(hmac_ctx_t *c, uint8_t out[DOCK_SHA256_LEN])
{
    uint8_t ih[DOCK_SHA256_LEN];
    dock_sha256_final(&c->inner, ih);
    dock_sha256_update(&c->outer, ih, sizeof ih);
    dock_sha256_final(&c->outer, out);
    memset(ih, 0, sizeof ih);
}

void dock_hmac_sha256(const uint8_t *key, size_t key_len, const void *msg,
                      size_t n, uint8_t out[DOCK_SHA256_LEN])
{
    hmac_ctx_t c;
    hmac_init(&c, key, key_len);
    dock_sha256_update(&c.inner, msg, n);
    hmac_final(&c, out);
}

/* ---- PBKDF2 ---- */

void dock_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt,
                        size_t salt_len, uint32_t iterations, uint8_t *out,
                        size_t out_len)
{
    hmac_ctx_t base, c;
    uint8_t u[DOCK_SHA256_LEN], t[DOCK_SHA256_LEN], ctr[4];
    uint32_t block = 1, it;

    /* The keyed pads are the same for every HMAC call: compute them once. */
    hmac_init(&base, pw, pw_len);
    while (out_len) {
        size_t take = out_len < DOCK_SHA256_LEN ? out_len : DOCK_SHA256_LEN;
        uint8_t i;
        put_be32(ctr, block);
        c = base;
        dock_sha256_update(&c.inner, salt, salt_len);
        dock_sha256_update(&c.inner, ctr, sizeof ctr);
        hmac_final(&c, u);
        memcpy(t, u, sizeof t);
        for (it = 1; it < iterations; it++) {
            c = base;
            dock_sha256_update(&c.inner, u, sizeof u);
            hmac_final(&c, u);
            for (i = 0; i < DOCK_SHA256_LEN; i++)
                t[i] ^= u[i];
        }
        memcpy(out, t, take);
        out += take;
        out_len -= take;
        block++;
    }
    memset(&base, 0, sizeof base);
    memset(u, 0, sizeof u);
    memset(t, 0, sizeof t);
}

bool dock_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < n; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}
