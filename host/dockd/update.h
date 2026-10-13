/*
 * update.h - USB update package discovery and integrity check (host side).
 *
 * The update is a source bundle (calcscreen-update.tar.gz) plus a manifest
 * (calcscreen-update.manifest) produced by tools/make_dist.sh. dockd scans the
 * USB drive mounts for the pair, verifies the tarball's SHA-256 against the
 * manifest (integrity only; trust comes from the admin password + physical
 * access), then hands the paths to the privileged applier (pi/dock-update),
 * which rebuilds dockd on the Pi.
 *
 * This module is deliberately free of libusb/alsa/drm so it can be unit
 * tested: it uses only core/sha256 and the C library (opendir/readdir, stdio).
 */
#ifndef DOCKD_UPDATE_H
#define DOCKD_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed file names on the stick (and inside the manifest's file= field). */
#define UPDATE_TAR_NAME "calcscreen-update.tar.gz"
#define UPDATE_MANIFEST_NAME "calcscreen-update.manifest"

#define UPDATE_PATH_MAX 512u
#define UPDATE_VER_MAX 32u

typedef struct update_pkg {
    char tar_path[UPDATE_PATH_MAX];      /* absolute path of the tarball */
    char manifest_path[UPDATE_PATH_MAX]; /* absolute path of the manifest */
    char version[UPDATE_VER_MAX];        /* manifest version= (e.g. "0.2.0") */
    char sha256[2u * 32u + 1u];          /* manifest sha256= (64 hex + NUL) */
    uint64_t size;                       /* manifest size= (tarball bytes) */
} update_pkg_t;

/*
 * Look in each immediate subdirectory of drives_root (the /media/dock/<label>
 * mounts) for a well-formed UPDATE_TAR_NAME + UPDATE_MANIFEST_NAME pair, pick
 * the first in sorted directory order, fill *out and return true. Returns
 * false when none is found or the manifest is malformed.
 */
bool update_scan(const char *drives_root, update_pkg_t *out);

/*
 * Parse one manifest buffer (key=value lines) into *out (tar_path is left
 * untouched). Returns true when format/version/file/size/sha256 are present
 * and valid. Exposed for tests.
 */
bool update_parse_manifest(const char *text, size_t len, update_pkg_t *out);

/*
 * Stream the tarball through SHA-256 (64 KiB chunks) and compare it and the
 * file size to the manifest. Returns DOCK_OK, or DOCK_E_* (DOCK_E_IO,
 * DOCK_E_INVALID on a hash/size mismatch).
 */
int update_verify(const update_pkg_t *pkg);

/*
 * Compare two dotted versions (any "+ghash" suffix is ignored). Returns
 * -1 if pkg_ver < cur_ver, 0 if equal, 1 if pkg_ver > cur_ver.
 */
int update_version_cmp(const char *pkg_ver, const char *cur_ver);

#endif /* DOCKD_UPDATE_H */
