/* update.c - see update.h. */
#define _GNU_SOURCE
#include "update.h"

#include "dock_common.h"
#include "sha256.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_CHUNK (64u * 1024u)
#define MANIFEST_MAX 4096u
#define MAX_DRIVES 64

/* ------------------------------------------------------------------ */
/* manifest parsing                                                    */

/* Copy the value after '=' for `key` on any line of text into dst (NUL
 * terminated, trailing CR/space trimmed). Returns true when the key is found
 * and its value fit. */
static bool field(const char *text, size_t len, const char *key, char *dst,
                  size_t cap)
{
    size_t klen = strlen(key), i = 0;
    while (i < len) {
        size_t ls = i, le;
        while (i < len && text[i] != '\n')
            i++;
        le = i;
        if (i < len)
            i++; /* step over '\n' */
        /* line is text[ls..le) */
        if (le - ls > klen && memcmp(text + ls, key, klen) == 0 &&
            text[ls + klen] == '=') {
            size_t vs = ls + klen + 1, ve = le, n;
            while (ve > vs && (text[ve - 1] == '\r' || text[ve - 1] == ' ' ||
                               text[ve - 1] == '\t'))
                ve--;
            n = ve - vs;
            if (n + 1 > cap)
                return false;
            memcpy(dst, text + vs, n);
            dst[n] = '\0';
            return true;
        }
    }
    return false;
}

static bool is_hex64(const char *s)
{
    size_t i;
    for (i = 0; i < 64u; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return s[64] == '\0';
}

static bool parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (!s[0])
        return false;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        if (v > (UINT64_MAX - 9u) / 10u)
            return false;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

bool update_parse_manifest(const char *text, size_t len, update_pkg_t *out)
{
    char buf[128];

    /* format=1 is required and must be 1 (this is the only format we grok). */
    if (!field(text, len, "format", buf, sizeof buf) || strcmp(buf, "1") != 0)
        return false;
    /* min_format, when present, must be satisfiable by format 1. */
    if (field(text, len, "min_format", buf, sizeof buf)) {
        uint64_t mf;
        if (!parse_u64(buf, &mf) || mf > 1u)
            return false;
    }
    if (!field(text, len, "version", out->version, sizeof out->version) ||
        !out->version[0])
        return false;
    if (!field(text, len, "file", buf, sizeof buf) ||
        strcmp(buf, UPDATE_TAR_NAME) != 0)
        return false;
    if (!field(text, len, "size", buf, sizeof buf) || !parse_u64(buf, &out->size) ||
        out->size == 0)
        return false;
    if (!field(text, len, "sha256", out->sha256, sizeof out->sha256) ||
        !is_hex64(out->sha256))
        return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */

static bool read_file(const char *path, char *dst, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return false;
    n = fread(dst, 1, cap, f);
    fclose(f);
    *len = n;
    return true;
}

/* Fill *out from the pair in one drive directory. Returns true on success. */
static bool try_drive(const char *dir, update_pkg_t *out)
{
    char manifest[MANIFEST_MAX];
    char mpath[UPDATE_PATH_MAX], tpath[UPDATE_PATH_MAX];
    size_t len = 0;
    FILE *tf;

    if ((size_t)snprintf(mpath, sizeof mpath, "%s/%s", dir,
                         UPDATE_MANIFEST_NAME) >= sizeof mpath)
        return false;
    if ((size_t)snprintf(tpath, sizeof tpath, "%s/%s", dir, UPDATE_TAR_NAME) >=
        sizeof tpath)
        return false;
    /* The tarball must be a real, openable file. */
    tf = fopen(tpath, "rb");
    if (!tf)
        return false;
    fclose(tf);
    if (!read_file(mpath, manifest, sizeof manifest, &len))
        return false;
    memset(out, 0, sizeof *out);
    if (!update_parse_manifest(manifest, len, out))
        return false;
    memcpy(out->tar_path, tpath, sizeof out->tar_path);
    memcpy(out->manifest_path, mpath, sizeof out->manifest_path);
    return true;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool update_scan(const char *drives_root, update_pkg_t *out)
{
    DIR *d = opendir(drives_root);
    struct dirent *e;
    char *names[MAX_DRIVES];
    int n = 0, i;
    bool found = false;

    if (!d)
        return false;
    while ((e = readdir(d)) != NULL && n < MAX_DRIVES) {
        if (e->d_name[0] == '.')
            continue; /* skip ".", "..", hidden */
        names[n] = strdup(e->d_name);
        if (names[n])
            n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_str);
    for (i = 0; i < n; i++) {
        char dir[UPDATE_PATH_MAX];
        if (!found &&
            (size_t)snprintf(dir, sizeof dir, "%s/%s", drives_root, names[i]) <
                sizeof dir)
            found = try_drive(dir, out);
        free(names[i]);
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* verify                                                              */

int update_verify(const update_pkg_t *pkg)
{
    dock_sha256_t ctx;
    uint8_t digest[DOCK_SHA256_LEN];
    char hex[2u * DOCK_SHA256_LEN + 1u];
    uint8_t *chunk;
    uint64_t total = 0;
    FILE *f;
    size_t i;
    int rc = DOCK_OK;

    f = fopen(pkg->tar_path, "rb");
    if (!f)
        return DOCK_E_IO;
    chunk = malloc(SCAN_CHUNK);
    if (!chunk) {
        fclose(f);
        return DOCK_E_IO;
    }
    dock_sha256_init(&ctx);
    for (;;) {
        size_t got = fread(chunk, 1, SCAN_CHUNK, f);
        if (got > 0) {
            dock_sha256_update(&ctx, chunk, got);
            total += got;
        }
        if (got < SCAN_CHUNK) {
            if (ferror(f))
                rc = DOCK_E_IO;
            break;
        }
    }
    free(chunk);
    fclose(f);
    if (rc != DOCK_OK)
        return rc;

    if (total != pkg->size)
        return DOCK_E_INVALID;
    dock_sha256_final(&ctx, digest);
    for (i = 0; i < DOCK_SHA256_LEN; i++)
        snprintf(hex + i * 2u, 3u, "%02x", digest[i]);
    /* Case-insensitive compare (manifest hex may be upper or lower). */
    for (i = 0; i < 2u * DOCK_SHA256_LEN; i++) {
        char a = hex[i], b = pkg->sha256[i];
        if (b >= 'A' && b <= 'F')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return DOCK_E_INVALID;
    }
    return DOCK_OK;
}

/* ------------------------------------------------------------------ */
/* version compare                                                     */

/* Parse the next dotted numeric component of *s, advancing *s past it and any
 * single separating '.'. A non-digit (end, or a "+ghash"/"-rc" suffix) yields
 * no further components. */
static unsigned next_component(const char **s)
{
    unsigned v = 0;
    const char *p = *s;
    while (*p >= '0' && *p <= '9')
        v = v * 10u + (unsigned)(*p++ - '0');
    if (*p == '.')
        p++;
    *s = p;
    return v;
}

int update_version_cmp(const char *pkg_ver, const char *cur_ver)
{
    const char *a = pkg_ver, *b = cur_ver;
    int i;
    for (i = 0; i < 8; i++) {
        unsigned va, vb;
        bool a_end = !(*a >= '0' && *a <= '9');
        bool b_end = !(*b >= '0' && *b <= '9');
        if (a_end && b_end)
            return 0;
        va = a_end ? 0u : next_component(&a);
        vb = b_end ? 0u : next_component(&b);
        if (va != vb)
            return va < vb ? -1 : 1;
    }
    return 0;
}
