/* test.h - tiny dependency-free test harness. */
#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned long t_checks, t_failures;
extern const char *t_current;

#define CHECK(cond)                                                          \
    do {                                                                     \
        t_checks++;                                                          \
        if (!(cond)) {                                                       \
            t_failures++;                                                    \
            fprintf(stderr, "%s:%d: [%s] CHECK(%s) failed\n", __FILE__,      \
                    __LINE__, t_current, #cond);                             \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long ta_ = (long long)(a), tb_ = (long long)(b);                \
        t_checks++;                                                          \
        if (ta_ != tb_) {                                                    \
            t_failures++;                                                    \
            fprintf(stderr, "%s:%d: [%s] %s == %s failed: %lld != %lld "     \
                    "(0x%llx != 0x%llx)\n", __FILE__, __LINE__, t_current,   \
                    #a, #b, ta_, tb_, (unsigned long long)ta_,               \
                    (unsigned long long)tb_);                                \
        }                                                                    \
    } while (0)

#define CHECK_MEM(a, b, n)                                                   \
    do {                                                                     \
        t_checks++;                                                          \
        if (memcmp((a), (b), (n)) != 0) {                                    \
            t_failures++;                                                    \
            fprintf(stderr, "%s:%d: [%s] memory %s != %s (%lu bytes)\n",     \
                    __FILE__, __LINE__, t_current, #a, #b,                   \
                    (unsigned long)(n));                                     \
            t_hexdiff((const uint8_t *)(a), (const uint8_t *)(b), (n));      \
        }                                                                    \
    } while (0)

#define CHECK_STR(a, b)                                                      \
    do {                                                                     \
        const char *sa_ = (a), *sb_ = (b);                                   \
        t_checks++;                                                          \
        if (strcmp(sa_, sb_) != 0) {                                         \
            t_failures++;                                                    \
            fprintf(stderr, "%s:%d: [%s] \"%s\" != \"%s\"\n", __FILE__,      \
                    __LINE__, t_current, sa_, sb_);                          \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do {                                                                     \
        unsigned long f0_ = t_failures;                                      \
        t_current = #fn;                                                     \
        fn();                                                                \
        t_tests++;                                                           \
        if (t_failures != f0_)                                               \
            t_failed_tests++;                                                \
        if (t_verbose)                                                       \
            printf("  %-40s %s\n", #fn, t_failures == f0_ ? "ok" : "FAIL");  \
    } while (0)

extern unsigned long t_tests, t_failed_tests;
extern int t_verbose;

void t_hexdiff(const uint8_t *a, const uint8_t *b, size_t n);
uint32_t t_rand(uint32_t *state); /* xorshift32, deterministic */

/* Suites (one per test_*.c). */
void suite_proto(void);
void suite_screen(void);
void suite_devhost(void);
void suite_sha256(void);
void suite_settings(void);
void suite_keymap(void);
void suite_dusb(void);
void suite_ui(void);

#endif
