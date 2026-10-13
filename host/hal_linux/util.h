/* util.h - small Linux helpers shared by the dock daemon and the simulator. */
#ifndef HOST_UTIL_H
#define HOST_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---- logging: stderr only, verbose messages only with -v ---- */
void log_set_verbose(bool on);
bool log_is_verbose(void);
void logv(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void loge(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Monotonic clock in milliseconds / microseconds. */
uint64_t mono_ms(void);
uint64_t mono_us(void);

/* fcntl helpers; return false on failure. */
bool fd_set_nonblock(int fd);
bool fd_set_cloexec(int fd);

/* Write everything (blocking fd), retrying on EINTR. */
bool write_all(int fd, const void *p, size_t n);

/* Bounded string copy that always terminates (when cap > 0). Returns false if
 * src was truncated. */
bool str_copy(char *dst, size_t cap, const char *src);

/* Read a small text file into buf (NUL-terminated, trailing newline removed).
 * Returns false if it could not be read. */
bool read_text_file(const char *path, char *buf, size_t cap);

/* Random bytes from getrandom(); falls back to a time-based mix (never
 * fails). */
void random_bytes(void *p, size_t n);
uint32_t random_u32(void);

/*
 * fork/exec argv[0] (PATH search) without a shell. stdin and stderr are
 * /dev/null; if out_fd is non-NULL, stdout is a non-blocking pipe returned in
 * *out_fd, otherwise /dev/null (or inherited when inherit_stdio is set, used
 * for the interactive shell). The child gets default signal dispositions and
 * an empty signal mask. Returns the pid, or -1.
 */
pid_t spawn_argv(char *const argv[], int *out_fd, bool inherit_stdio);

/* Split a command line on blanks into at most max-1 words (no quoting, no
 * shell). The words point into buf, which is modified. argv[n] = NULL.
 * Returns the word count. */
int split_words(char *buf, char **argv, int max);

#endif /* HOST_UTIL_H */
