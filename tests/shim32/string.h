/* Minimal <string.h> for the freestanding -m32 portability build (the host
 * has no 32-bit libc headers). Declares only what core/ may use. */
#ifndef SHIM32_STRING_H
#define SHIM32_STRING_H
#include <stddef.h>
void *memcpy(void *restrict d, const void *restrict s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
