/* storage.c - see storage.h. */
#define _GNU_SOURCE
#include "storage.h"
#include "util.h"
#include "dock_proto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DRIVES 16
#define MAX_COMPONENTS 64
#define MAX_PATH_IN 1024

typedef struct handle {
    int fd;          /* -1 = free */
    bool written;    /* fsync on close */
} handle_t;

struct storage {
    char root[PATH_MAX];
    handle_t h[STORAGE_MAX_HANDLES];
};

typedef struct drive_list {
    int n;
    char name[MAX_DRIVES][NAME_MAX + 1];
} drive_list_t;

/* A sandboxed path: the parent directory and the final component ("." for
 * the drive root itself). */
typedef struct resolved {
    int dirfd;
    char last[NAME_MAX + 1];
} resolved_t;

static int errno_status(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return DOCK_E_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS:
    case ELOOP: /* a symlink: never followed */
    case ENOTEMPTY:
    case EBUSY:
        return DOCK_E_DENIED;
    case ENOSPC:
    case EDQUOT:
    case EFBIG:
        return DOCK_E_NO_SPACE;
    case EEXIST:
    case EISDIR:
    case ENAMETOOLONG:
    case EINVAL:
        return DOCK_E_INVALID;
    case ENODEV:
    case ENXIO:
        return DOCK_E_NO_DRIVE;
    default:
        return DOCK_E_IO;
    }
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Sorted drive directory names under the root. */
static void list_drives(const storage_t *s, drive_list_t *dl)
{
    DIR *d = opendir(s->root);
    struct dirent *e;
    dl->n = 0;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL && dl->n < MAX_DRIVES) {
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(st.st_mode))
            continue;
        str_copy(dl->name[dl->n++], NAME_MAX + 1, e->d_name);
    }
    closedir(d);
    qsort(dl->name, (size_t)dl->n, sizeof dl->name[0], cmp_names);
}

static bool bad_component(const char *c)
{
    for (; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7F || *c == '\\')
            return true;
    return false;
}

/* Split a calculator path into components, rejecting anything unsafe. */
static int split_path(char *buf, char **comp, int *n)
{
    char *save = NULL, *t;
    *n = 0;
    if (buf[0] == '/')
        return DOCK_E_DENIED; /* absolute */
    for (t = strtok_r(buf, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (strcmp(t, ".") == 0)
            continue;
        if (strcmp(t, "..") == 0)
            return DOCK_E_DENIED;
        if (bad_component(t) || strlen(t) > NAME_MAX)
            return DOCK_E_INVALID;
        if (*n >= MAX_COMPONENTS)
            return DOCK_E_INVALID;
        comp[(*n)++] = t;
    }
    return DOCK_OK;
}

static int open_drive(const storage_t *s, const char *drive)
{
    int root = open(s->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int fd;
    if (root < 0)
        return -1;
    fd = openat(root, drive, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(root);
    return fd;
}

static int resolve(const storage_t *s, const char *path, resolved_t *r)
{
    char buf[MAX_PATH_IN];
    char *comp[MAX_COMPONENTS];
    drive_list_t dl;
    const char *drive;
    int n = 0, first = 0, i, fd, st;

    r->dirfd = -1;
    if (!path || strlen(path) >= sizeof buf)
        return DOCK_E_INVALID;
    memcpy(buf, path, strlen(path) + 1);
    st = split_path(buf, comp, &n);
    if (st != DOCK_OK)
        return st;

    list_drives(s, &dl);
    if (dl.n == 0)
        return DOCK_E_NO_DRIVE;
    drive = dl.name[0];
    for (i = 0; n > 0 && i < dl.n; i++) {
        if (strcasecmp(comp[0], dl.name[i]) == 0) {
            drive = dl.name[i];
            first = 1;
            break;
        }
    }

    fd = open_drive(s, drive);
    if (fd < 0)
        return DOCK_E_NO_DRIVE;
    for (i = first; i < n - 1; i++) {
        int next = openat(fd, comp[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int e = errno;
        close(fd);
        if (next < 0)
            return errno_status(e);
        fd = next;
    }
    r->dirfd = fd;
    str_copy(r->last, sizeof r->last, n > first ? comp[n - 1] : ".");
    return DOCK_OK;
}

static bool is_root(const resolved_t *r)
{
    return strcmp(r->last, ".") == 0;
}

/* ------------------------------------------------------------------ */

storage_t *storage_open(const char *root)
{
    storage_t *s = calloc(1, sizeof *s);
    unsigned i;
    if (!s)
        return NULL;
    str_copy(s->root, sizeof s->root, root);
    for (i = 0; i < STORAGE_MAX_HANDLES; i++)
        s->h[i].fd = -1;
    return s;
}

void storage_close(storage_t *s)
{
    if (!s)
        return;
    storage_close_all(s);
    free(s);
}

int storage_drive(storage_t *s, uint8_t index, char *name, size_t cap)
{
    drive_list_t dl;
    list_drives(s, &dl);
    if (index >= dl.n)
        return DOCK_E_END;
    str_copy(name, cap, dl.name[index]);
    return DOCK_OK;
}

static handle_t *get_handle(storage_t *s, uint8_t h)
{
    if (h >= STORAGE_MAX_HANDLES || s->h[h].fd < 0)
        return NULL;
    return &s->h[h];
}

static int open_flags(uint8_t mode, int *flags)
{
    bool rd = mode & DOCK_FS_READ, wr = mode & DOCK_FS_WRITE;
    if (!rd && !wr)
        return DOCK_E_INVALID;
    if (!wr && (mode & (DOCK_FS_CREATE | DOCK_FS_TRUNC | DOCK_FS_APPEND)))
        return DOCK_E_INVALID;
    *flags = (rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC |
             O_NOCTTY | ((mode & DOCK_FS_CREATE) ? O_CREAT : 0) |
             ((mode & DOCK_FS_TRUNC) ? O_TRUNC : 0) |
             ((mode & DOCK_FS_APPEND) ? O_APPEND : 0);
    return DOCK_OK;
}

/* Open a regular file through the sandbox. */
static int open_regular(storage_t *s, const char *path, int flags, int *out)
{
    resolved_t r;
    struct stat st;
    int fd, e, rc = resolve(s, path, &r);
    if (rc != DOCK_OK)
        return rc;
    if (is_root(&r)) {
        close(r.dirfd);
        return DOCK_E_INVALID;
    }
    fd = openat(r.dirfd, r.last, flags | O_NONBLOCK, 0644);
    e = errno;
    close(r.dirfd);
    if (fd < 0)
        return errno_status(e);
    /* O_NONBLOCK only guards against FIFOs; regular files ignore it. */
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return DOCK_E_INVALID;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    *out = fd;
    return DOCK_OK;
}

int storage_fopen(storage_t *s, const char *path, uint8_t mode, uint8_t *handle)
{
    unsigned i;
    int flags = 0, fd = -1, rc = open_flags(mode, &flags);
    if (rc != DOCK_OK)
        return rc;
    for (i = 0; i < STORAGE_MAX_HANDLES && s->h[i].fd >= 0; i++)
        ;
    if (i == STORAGE_MAX_HANDLES)
        return DOCK_E_BUSY;
    rc = open_regular(s, path, flags, &fd);
    if (rc != DOCK_OK)
        return rc;
    s->h[i].fd = fd;
    s->h[i].written = false;
    *handle = (uint8_t)i;
    return DOCK_OK;
}

int storage_open_ro_fd(storage_t *s, const char *path, int *fd)
{
    return open_regular(s, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY, fd);
}

int storage_read(storage_t *s, uint8_t handle, uint8_t *dst, size_t max, size_t *got)
{
    handle_t *h = get_handle(s, handle);
    size_t total = 0;
    *got = 0;
    if (!h)
        return DOCK_E_INVALID;
    while (total < max) {
        ssize_t n = read(h->fd, dst + total, max - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (total)
                break;
            return errno_status(errno);
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    *got = total;
    return total == 0 && max > 0 ? DOCK_E_END : DOCK_OK;
}

int storage_write(storage_t *s, uint8_t handle, const uint8_t *src, size_t n,
                  size_t *written)
{
    handle_t *h = get_handle(s, handle);
    size_t total = 0;
    *written = 0;
    if (!h)
        return DOCK_E_INVALID;
    while (total < n) {
        ssize_t w = write(h->fd, src + total, n - total);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (total)
                break;
            return errno_status(errno);
        }
        total += (size_t)w;
    }
    h->written = h->written || total > 0;
    *written = total;
    return DOCK_OK;
}

int storage_seek(storage_t *s, uint8_t handle, int32_t offset, uint8_t whence,
                 uint32_t *pos)
{
    handle_t *h = get_handle(s, handle);
    int wh;
    off_t r;
    *pos = 0;
    if (!h)
        return DOCK_E_INVALID;
    switch (whence) {
    case DOCK_SEEK_SET:
        if (offset < 0)
            return DOCK_E_INVALID;
        wh = SEEK_SET;
        break;
    case DOCK_SEEK_CUR:
        wh = SEEK_CUR;
        break;
    case DOCK_SEEK_END:
        wh = SEEK_END;
        break;
    default:
        return DOCK_E_INVALID;
    }
    r = lseek(h->fd, (off_t)offset, wh);
    if (r < 0)
        return errno_status(errno);
    if ((uint64_t)r > 0xFFFFFFFFu)
        return DOCK_E_TOO_BIG;
    *pos = (uint32_t)r;
    return DOCK_OK;
}

int storage_fclose(storage_t *s, uint8_t handle)
{
    handle_t *h = get_handle(s, handle);
    int rc = DOCK_OK;
    if (!h)
        return DOCK_E_INVALID;
    /* USB drives get yanked: make written data durable before saying OK. */
    if (h->written && fsync(h->fd) != 0)
        rc = errno_status(errno);
    if (close(h->fd) != 0 && rc == DOCK_OK)
        rc = DOCK_E_IO;
    h->fd = -1;
    return rc;
}

void storage_close_all(storage_t *s)
{
    uint8_t i;
    for (i = 0; i < STORAGE_MAX_HANDLES; i++)
        if (s->h[i].fd >= 0)
            storage_fclose(s, i);
}

static void fill_stat(const struct stat *st, uint32_t *size, bool *is_dir)
{
    *is_dir = S_ISDIR(st->st_mode);
    *size = *is_dir ? 0
                    : (st->st_size > (off_t)0xFFFFFFFF ? 0xFFFFFFFFu : (uint32_t)st->st_size);
}

int storage_stat(storage_t *s, const char *path, uint32_t *size, bool *is_dir)
{
    resolved_t r;
    struct stat st;
    int rc = resolve(s, path, &r), e;
    *size = 0;
    *is_dir = false;
    if (rc != DOCK_OK)
        return rc;
    rc = fstatat(r.dirfd, r.last, &st, AT_SYMLINK_NOFOLLOW);
    e = errno;
    close(r.dirfd);
    if (rc != 0)
        return errno_status(e);
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
        return DOCK_E_NOT_FOUND; /* hide symlinks, devices, fifos */
    fill_stat(&st, size, is_dir);
    return DOCK_OK;
}

typedef struct name_list {
    char (*name)[NAME_MAX + 1];
    size_t n, cap;
} name_list_t;

static bool names_add(name_list_t *l, const char *name)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        void *p;
        if (cap > 65536)
            return false;
        p = realloc(l->name, cap * sizeof l->name[0]);
        if (!p)
            return false;
        l->name = p;
        l->cap = cap;
    }
    str_copy(l->name[l->n++], NAME_MAX + 1, name);
    return true;
}

int storage_readdir(storage_t *s, const char *path, uint16_t index, char *name,
                    size_t cap, uint32_t *size, bool *is_dir)
{
    resolved_t r;
    name_list_t l = {NULL, 0, 0};
    struct dirent *e;
    struct stat st;
    DIR *d;
    int fd, rc = resolve(s, path, &r), err;

    *size = 0;
    *is_dir = false;
    if (rc != DOCK_OK)
        return rc;
    fd = openat(r.dirfd, r.last, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    err = errno;
    close(r.dirfd);
    if (fd < 0)
        return errno_status(err);
    d = fdopendir(fd);
    if (!d) {
        close(fd);
        return DOCK_E_IO;
    }
    /* Sorted so that indexes are stable between calls. */
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (!names_add(&l, e->d_name))
            break;
    }
    qsort(l.name, l.n, sizeof l.name[0], cmp_names);
    rc = DOCK_E_END;
    if (index < l.n) {
        str_copy(name, cap, l.name[index]);
        if (fstatat(dirfd(d), l.name[index], &st, AT_SYMLINK_NOFOLLOW) == 0)
            fill_stat(&st, size, is_dir);
        rc = DOCK_OK;
    }
    free(l.name);
    closedir(d);
    return rc;
}

int storage_mkdir(storage_t *s, const char *path)
{
    resolved_t r;
    int rc = resolve(s, path, &r), e;
    if (rc != DOCK_OK)
        return rc;
    if (is_root(&r)) {
        close(r.dirfd);
        return DOCK_E_INVALID;
    }
    rc = mkdirat(r.dirfd, r.last, 0755);
    e = errno;
    if (rc == 0)
        fsync(r.dirfd);
    close(r.dirfd);
    return rc == 0 ? DOCK_OK : errno_status(e);
}

int storage_remove(storage_t *s, const char *path)
{
    resolved_t r;
    struct stat st;
    int rc = resolve(s, path, &r), e;
    if (rc != DOCK_OK)
        return rc;
    if (is_root(&r)) {
        close(r.dirfd);
        return DOCK_E_DENIED;
    }
    if (fstatat(r.dirfd, r.last, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        e = errno;
        close(r.dirfd);
        return errno_status(e);
    }
    rc = unlinkat(r.dirfd, r.last, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0);
    e = errno;
    if (rc == 0)
        fsync(r.dirfd);
    close(r.dirfd);
    return rc == 0 ? DOCK_OK : errno_status(e);
}
