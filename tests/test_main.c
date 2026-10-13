/* test_main.c - runs every suite and prints a one-line summary. */
#include <stdlib.h>

#include "test.h"

unsigned long t_checks, t_failures, t_tests, t_failed_tests;
const char *t_current = "";
int t_verbose;

void t_hexdiff(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i, shown = 0;
    for (i = 0; i < n && shown < 8; i++) {
        if (a[i] != b[i]) {
            fprintf(stderr, "    [%lu] %02x != %02x\n", (unsigned long)i, a[i], b[i]);
            shown++;
        }
    }
}

uint32_t t_rand(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x12345678u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static const struct {
    const char *name;
    void (*fn)(void);
} suites[] = {
    {"proto", suite_proto},     {"screen", suite_screen},
    {"devhost", suite_devhost}, {"sha256", suite_sha256},
    {"settings", suite_settings}, {"keymap", suite_keymap},
    {"dusb", suite_dusb},       {"ui", suite_ui},
};

int main(int argc, char **argv)
{
    size_t i;
    int a;
    const char *only = NULL;
    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "-v") == 0)
            t_verbose = 1;
        else
            only = argv[a];
    }
    for (i = 0; i < sizeof suites / sizeof suites[0]; i++) {
        unsigned long f0 = t_failures, c0 = t_checks;
        if (only && strcmp(only, suites[i].name) != 0)
            continue;
        if (t_verbose)
            printf("%s:\n", suites[i].name);
        suites[i].fn();
        printf("%-9s %6lu checks, %lu failed\n", suites[i].name,
               t_checks - c0, t_failures - f0);
    }
    printf("SUMMARY: %lu tests, %lu checks, %lu failed checks, %lu failed tests\n",
           t_tests, t_checks, t_failures, t_failed_tests);
    return t_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
