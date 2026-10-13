/* test_sha256.c - FIPS 180-2, RFC 4231 and RFC 7914 vectors. */
#include "../core/sha256.h"
#include "test.h"

static void unhex(const char *h, uint8_t *out)
{
    size_t i;
    for (i = 0; h[2 * i]; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void check_digest(const uint8_t *got, const char *hex, size_t n)
{
    uint8_t want[64];
    unhex(hex, want);
    CHECK_MEM(got, want, n);
}

static void t_sha256_fips(void)
{
    static const char *abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char *two_blocks =
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    static const char *million =
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    static const char *empty =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static uint8_t a1000[1000];
    uint8_t out[32];
    dock_sha256_t s;
    size_t i;

    dock_sha256("abc", 3, out);
    check_digest(out, abc, 32);
    dock_sha256("", 0, out);
    check_digest(out, empty, 32);
    dock_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    check_digest(out, two_blocks, 32);

    memset(a1000, 'a', sizeof a1000);
    dock_sha256_init(&s);
    for (i = 0; i < 1000; i++)
        dock_sha256_update(&s, a1000, sizeof a1000);
    dock_sha256_final(&s, out);
    check_digest(out, million, 32);

    /* Same message fed in awkward pieces. */
    dock_sha256_init(&s);
    dock_sha256_update(&s, "abcdbcdecdefdefgefghfghighijhij", 31);
    dock_sha256_update(&s, "", 0);
    dock_sha256_update(&s, "kijkljklmklmnlmnomnopnopq", 25);
    dock_sha256_final(&s, out);
    check_digest(out, two_blocks, 32);

    /* Padding edge cases: 55, 56, 63, 64 bytes vs one-shot. */
    {
        static const size_t lens[] = {55, 56, 57, 63, 64, 65, 119, 120};
        for (i = 0; i < sizeof lens / sizeof lens[0]; i++) {
            uint8_t one[32], inc[32];
            size_t k;
            dock_sha256(a1000, lens[i], one);
            dock_sha256_init(&s);
            for (k = 0; k < lens[i]; k++)
                dock_sha256_update(&s, a1000 + k, 1);
            dock_sha256_final(&s, inc);
            CHECK_MEM(one, inc, 32);
        }
        /* 'a' * 64 */
        dock_sha256(a1000, 64, out);
        check_digest(out, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", 32);
    }
}

static void t_hmac_rfc4231(void)
{
    uint8_t key[131], data[160], out[32];
    size_t i;

    /* Test case 1 */
    memset(key, 0x0b, 20);
    dock_hmac_sha256(key, 20, "Hi There", 8, out);
    check_digest(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);
    /* Test case 2 */
    dock_hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, out);
    check_digest(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);
    /* Test case 3 */
    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    dock_hmac_sha256(key, 20, data, 50, out);
    check_digest(out, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 32);
    /* Test case 4 */
    for (i = 0; i < 25; i++)
        key[i] = (uint8_t)(i + 1);
    memset(data, 0xcd, 50);
    dock_hmac_sha256(key, 25, data, 50, out);
    check_digest(out, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", 32);
    /* Test case 5 (truncated to 128 bits) */
    memset(key, 0x0c, 20);
    dock_hmac_sha256(key, 20, "Test With Truncation", 20, out);
    check_digest(out, "a3b6167473100ee06e0c796c2955552b", 16);
    /* Test case 6: key longer than a block */
    memset(key, 0xaa, 131);
    dock_hmac_sha256(key, 131, "Test Using Larger Than Block-Size Key - Hash Key First",
                     54, out);
    check_digest(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32);
    /* Test case 7 */
    {
        static const char *m =
            "This is a test using a larger than block-size key and a larger than "
            "block-size data. The key needs to be hashed before being used by the "
            "HMAC algorithm.";
        dock_hmac_sha256(key, 131, m, strlen(m), out);
        check_digest(out, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
                     32);
    }
}

static void t_pbkdf2(void)
{
    uint8_t out[64];
    /* RFC 7914 section 11 */
    dock_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1,
                       out, 64);
    check_digest(out,
                 "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                 "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
                 64);
    dock_pbkdf2_sha256((const uint8_t *)"Password", 8, (const uint8_t *)"NaCl", 4,
                       80000, out, 64);
    check_digest(out,
                 "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
                 "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d",
                 64);
    /* Widely used vector (c = 4096, dkLen = 32) and a partial last block. */
    dock_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4,
                       4096, out, 32);
    check_digest(out, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a",
                 32);
    memset(out, 0xEE, sizeof out);
    dock_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1,
                       out, 40);
    check_digest(out, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                      "49ca9cccf179b645", 40);
    CHECK_EQ(out[40], 0xEE); /* nothing written past out_len */
}

static void t_ct_equal(void)
{
    uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 4};
    CHECK(dock_ct_equal(a, b, 4));
    CHECK(dock_ct_equal(a, b, 0));
    b[3] = 5;
    CHECK(!dock_ct_equal(a, b, 4));
    CHECK(dock_ct_equal(a, b, 3));
    b[0] = 0x81;
    CHECK(!dock_ct_equal(a, b, 1));
}

void suite_sha256(void)
{
    RUN(t_sha256_fips);
    RUN(t_hmac_rfc4231);
    RUN(t_pbkdf2);
    RUN(t_ct_equal);
}
