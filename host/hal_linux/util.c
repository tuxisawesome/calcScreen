/* util.c - see util.h. */
#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

static bool g_verbose;

void log_set_verbose(bool on)
{
    g_verbose = on;
}

bool log_is_verbose(void)
{
    return g_verbose;
}

static void vlog(const char *tag, const char *fmt, va_list ap)
{
    char line[512];
    vsnprintf(line, sizeof line, fmt, ap);
    fprintf(stderr, "%s%s\n", tag, line);
}

void logv(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose)
        return;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void loge(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("error: ", fmt, ap);
    va_end(ap);
}

uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t mono_ms(void)
{
    return mono_us() / 1000u;
}

bool fd_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

bool fd_set_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD);
    return fl >= 0 && fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == 0;
}

bool write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        b += w;
        n -= (size_t)w;
    }
    return true;
}

bool str_copy(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!cap)
        return false;
    n = strlen(src);
    if (n >= cap) {
        memcpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}

bool read_text_file(const char *path, char *buf, size_t cap)
{
    int fd;
    ssize_t n;
    if (cap < 2)
        return false;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    do {
        n = read(fd, buf, cap - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 0)
        return false;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return true;
}

void random_bytes(void *p, size_t n)
{
    uint8_t *b = p;
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(b + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        got += (size_t)r;
    }
    if (got < n) {
        /* Should never happen on Linux >= 3.17; degrade instead of failing. */
        uint64_t x = mono_us() ^ ((uint64_t)getpid() << 32);
        for (; got < n; got++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            b[got] = (uint8_t)x;
        }
    }
}

uint32_t random_u32(void)
{
    uint32_t v;
    random_bytes(&v, sizeof v);
    return v;
}

/* Child side of spawn_argv: never returns. */
static void child_exec(char *const argv[], int out_w, bool inherit_stdio)
{
    sigset_t none;
    int nul = open("/dev/null", O_RDWR);
    int sig;

    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    for (sig = 1; sig < NSIG; sig++)
        signal(sig, SIG_DFL); /* errors for SIGKILL etc. are harmless */

    if (!inherit_stdio && nul >= 0) {
        dup2(nul, STDIN_FILENO);
        dup2(nul, STDERR_FILENO);
        dup2(out_w >= 0 ? out_w : nul, STDOUT_FILENO);
    }
    execvp(argv[0], argv);
    _exit(127);
}

pid_t spawn_argv(char *const argv[], int *out_fd, bool inherit_stdio)
{
    int pfd[2] = {-1, -1};
    pid_t pid;

    if (out_fd && pipe2(pfd, O_CLOEXEC) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        if (out_fd) {
            close(pfd[0]);
            close(pfd[1]);
        }
        return -1;
    }
    if (pid == 0)
        child_exec(argv, pfd[1], inherit_stdio);
    if (out_fd) {
        close(pfd[1]);
        fd_set_nonblock(pfd[0]);
        *out_fd = pfd[0];
    }
    return pid;
}

int split_words(char *buf, char **argv, int max)
{
    int n = 0;
    char *save = NULL, *tok;
    if (max < 1)
        return 0;
    for (tok = strtok_r(buf, " \t", &save); tok && n < max - 1;
         tok = strtok_r(NULL, " \t", &save))
        argv[n++] = tok;
    argv[n] = NULL;
    return n;
}
