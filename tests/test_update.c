/* test_update.c - USB update manifest parse, SHA-256 verify, version compare.
 *
 * update.c is host code (opendir/stdio), but it is deliberately dependency
 * light (only core/sha256 + libc) so it links straight into this core test
 * binary. The verify/scan tests build a tiny package in a temp directory. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include "../host/dockd/update.h"
#include "../core/sha256.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- helpers ---- */

static void hex_of(const void *p, size_t n, char *out)
{
    const uint8_t *b = p;
    size_t i;
    dock_sha256_t ctx;
    uint8_t d[DOCK_SHA256_LEN];
    dock_sha256_init(&ctx);
    dock_sha256_update(&ctx, b, n);
    dock_sha256_final(&ctx, d);
    for (i = 0; i < DOCK_SHA256_LEN; i++)
        sprintf(out + i * 2u, "%02x", d[i]);
    out[64] = '\0';
}

static int mk_tmpdir(char *buf, size_t cap)
{
    const char *base = getenv("TMPDIR");
    snprintf(buf, cap, "%s/dock-upd.XXXXXX", base && base[0] ? base : "/tmp");
    return mkdtemp(buf) ? 0 : -1;
}

static void write_bytes(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        if (n)
            fwrite(p, 1, n, f);
        fclose(f);
    }
}

/* Build a drive dir <root>/<drv> with the tarball bytes and a manifest whose
 * fields can be overridden. A NULL field is omitted. */
static void make_pkg(const char *root, const char *drv, const void *tar,
                     size_t tar_len, const char *ver, const char *sha,
                     const char *size, const char *fmt, const char *fname)
{
    char dir[512], path[640], man[1024];
    size_t off = 0;
    snprintf(dir, sizeof dir, "%s/%s", root, drv);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s", dir, UPDATE_TAR_NAME);
    write_bytes(path, tar, tar_len);
#define ADD(k, v) \
    if (v) off += (size_t)snprintf(man + off, sizeof man - off, "%s=%s\n", k, v)
    ADD("format", fmt);
    ADD("product", "calcscreen");
    ADD("version", ver);
    ADD("file", fname);
    ADD("size", size);
    ADD("sha256", sha);
    ADD("min_format", "1");
#undef ADD
    snprintf(path, sizeof path, "%s/%s", dir, UPDATE_MANIFEST_NAME);
    write_bytes(path, man, off);
}

static void rm_pkg(const char *root, const char *drv)
{
    char p[640];
    snprintf(p, sizeof p, "%s/%s/%s", root, drv, UPDATE_TAR_NAME);
    unlink(p);
    snprintf(p, sizeof p, "%s/%s/%s", root, drv, UPDATE_MANIFEST_NAME);
    unlink(p);
    snprintf(p, sizeof p, "%s/%s", root, drv);
    rmdir(p);
}

/* ---- manifest parse ---- */

static void t_parse(void)
{
    update_pkg_t p;
    static const char good[] =
        "format=1\nproduct=calcscreen\nversion=1.2.3\ndate=x\n"
        "file=" UPDATE_TAR_NAME "\nsize=4096\n"
        "sha256=" "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "\nmin_format=1\n";
    memset(&p, 0, sizeof p);
    CHECK(update_parse_manifest(good, sizeof good - 1, &p));
    CHECK_STR(p.version, "1.2.3");
    CHECK_EQ(p.size, 4096);
    CHECK_STR(p.sha256,
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

    /* Extra/unknown keys are ignored; CRLF and trailing spaces trimmed. */
    {
        static const char extra[] =
            "format=1\r\nversion=2.0.0  \r\nextra=whatever\r\n"
            "file=" UPDATE_TAR_NAME "\r\nsize=10\r\n"
            "sha256=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n";
        memset(&p, 0, sizeof p);
        CHECK(update_parse_manifest(extra, sizeof extra - 1, &p));
        CHECK_STR(p.version, "2.0.0");
        CHECK_EQ(p.size, 10);
    }

    /* Missing required keys are rejected. */
    {
        static const char no_ver[] = "format=1\nfile=" UPDATE_TAR_NAME
                                     "\nsize=1\nsha256=" "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
        static const char no_sha[] = "format=1\nversion=1\nfile=" UPDATE_TAR_NAME "\nsize=1\n";
        static const char no_fmt[] = "version=1\nfile=" UPDATE_TAR_NAME "\nsize=1\n"
            "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
        CHECK(!update_parse_manifest(no_ver, sizeof no_ver - 1, &p));
        CHECK(!update_parse_manifest(no_sha, sizeof no_sha - 1, &p));
        CHECK(!update_parse_manifest(no_fmt, sizeof no_fmt - 1, &p));
    }

    /* Bad values: format!=1, short/non-hex sha, zero size, wrong file name. */
    {
        static const char bad_fmt[] = "format=2\nversion=1\nfile=" UPDATE_TAR_NAME
            "\nsize=1\nsha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
        static const char short_sha[] = "format=1\nversion=1\nfile=" UPDATE_TAR_NAME
            "\nsize=1\nsha256=deadbeef\n";
        static const char bad_hex[] = "format=1\nversion=1\nfile=" UPDATE_TAR_NAME
            "\nsize=1\nsha256=zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n";
        static const char zero_sz[] = "format=1\nversion=1\nfile=" UPDATE_TAR_NAME
            "\nsize=0\nsha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
        static const char bad_file[] = "format=1\nversion=1\nfile=other.tar.gz"
            "\nsize=1\nsha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
        static const char min_hi[] = "format=1\nversion=1\nfile=" UPDATE_TAR_NAME
            "\nsize=1\nmin_format=2\nsha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
        CHECK(!update_parse_manifest(bad_fmt, sizeof bad_fmt - 1, &p));
        CHECK(!update_parse_manifest(short_sha, sizeof short_sha - 1, &p));
        CHECK(!update_parse_manifest(bad_hex, sizeof bad_hex - 1, &p));
        CHECK(!update_parse_manifest(zero_sz, sizeof zero_sz - 1, &p));
        CHECK(!update_parse_manifest(bad_file, sizeof bad_file - 1, &p));
        CHECK(!update_parse_manifest(min_hi, sizeof min_hi - 1, &p));
    }
}

/* ---- version compare ---- */

static void t_version(void)
{
    CHECK_EQ(update_version_cmp("0.2.0", "0.2.0"), 0);
    CHECK_EQ(update_version_cmp("0.2.1", "0.2.0"), 1);
    CHECK_EQ(update_version_cmp("0.1.9", "0.2.0"), -1);
    CHECK_EQ(update_version_cmp("1.0.0", "0.9.9"), 1);
    CHECK_EQ(update_version_cmp("0.10.0", "0.9.0"), 1); /* numeric, not lexical */
    /* +ghash and -rc suffixes ignored. */
    CHECK_EQ(update_version_cmp("0.2.0+gdeadbee", "0.2.0"), 0);
    CHECK_EQ(update_version_cmp("0.2.0", "0.2.0+gcafef00"), 0);
    /* Short vs long: missing components are zero. */
    CHECK_EQ(update_version_cmp("1.0", "1.0.0"), 0);
    CHECK_EQ(update_version_cmp("1.0.1", "1.0"), 1);
    CHECK_EQ(update_version_cmp("2", "1.9.9"), 1);
}

/* ---- verify + scan ---- */

static void t_verify_scan(void)
{
    char root[200];
    update_pkg_t p;
    static const char tar[] = "not a real tarball, but bytes are bytes\n";
    size_t tn = sizeof tar - 1;
    char sha[65], szbuf[32];

    if (mk_tmpdir(root, sizeof root) != 0) {
        CHECK(0);
        return;
    }
    hex_of(tar, tn, sha);
    snprintf(szbuf, sizeof szbuf, "%zu", tn);

    /* One good drive: scan finds it, verify passes. */
    make_pkg(root, "USB0", tar, tn, "1.4.0", sha, szbuf, "1", UPDATE_TAR_NAME);
    memset(&p, 0, sizeof p);
    CHECK(update_scan(root, &p));
    CHECK_STR(p.version, "1.4.0");
    CHECK_EQ(p.size, tn);
    CHECK_EQ(update_verify(&p), DOCK_OK);

    /* Corrupted byte: hash mismatch. */
    {
        char tpath[640];
        FILE *f;
        snprintf(tpath, sizeof tpath, "%s/USB0/%s", root, UPDATE_TAR_NAME);
        f = fopen(tpath, "r+b");
        if (f) {
            fseek(f, 0, SEEK_SET);
            fputc('X', f);
            fclose(f);
        }
        CHECK_EQ(update_verify(&p), DOCK_E_INVALID);
    }

    /* Truncated tarball: size mismatch. */
    {
        char tpath[640];
        snprintf(tpath, sizeof tpath, "%s/USB0/%s", root, UPDATE_TAR_NAME);
        write_bytes(tpath, tar, tn - 3);
        CHECK_EQ(update_verify(&p), DOCK_E_INVALID);
    }

    rm_pkg(root, "USB0");

    /* Size field disagrees with the real file. */
    make_pkg(root, "USB0", tar, tn, "1.0.0", sha, "999999", "1", UPDATE_TAR_NAME);
    memset(&p, 0, sizeof p);
    CHECK(update_scan(root, &p));
    CHECK_EQ(update_verify(&p), DOCK_E_INVALID);
    rm_pkg(root, "USB0");

    /* Missing tarball (manifest only): scan skips the drive. */
    {
        char dir[512], mpath[640];
        snprintf(dir, sizeof dir, "%s/USB0", root);
        mkdir(dir, 0755);
        snprintf(mpath, sizeof mpath, "%s/%s", dir, UPDATE_MANIFEST_NAME);
        write_bytes(mpath, "format=1\n", 9);
        CHECK(!update_scan(root, &p));
        unlink(mpath);
        rmdir(dir);
    }

    /* Two drives: the first in sorted order wins. */
    make_pkg(root, "ZZZ", tar, tn, "9.9.9", sha, szbuf, "1", UPDATE_TAR_NAME);
    make_pkg(root, "AAA", tar, tn, "1.1.1", sha, szbuf, "1", UPDATE_TAR_NAME);
    memset(&p, 0, sizeof p);
    CHECK(update_scan(root, &p));
    CHECK_STR(p.version, "1.1.1");
    rm_pkg(root, "AAA");
    rm_pkg(root, "ZZZ");

    /* No drives at all. */
    CHECK(!update_scan(root, &p));

    rmdir(root);
}

void suite_update(void)
{
    RUN(t_parse);
    RUN(t_version);
    RUN(t_verify_scan);
}
