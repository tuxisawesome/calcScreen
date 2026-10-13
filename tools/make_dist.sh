#!/usr/bin/env bash
#
# make_dist.sh - produce the published release artifacts under dist/:
#   calcscreen-update.tar.gz   source bundle the dock rebuilds on-device
#   calcscreen-update.manifest format/version/date/sha256 of the tarball
#   DOCK.8xp, BADAPPLE.8xp     the latest calculator programs
#   VERSION                    the released version
#
# The tarball is deterministic (sorted entries, fixed mtime/owner) so an
# unchanged tree produces byte-identical output and the committed artifact only
# changes when the source does. The dock's USB updater (pi/dock-update) verifies
# the sha256 and rebuilds dockd from this bundle.
#
# Calculator programs are built when the CE C toolchain (cedev-config) is on
# PATH; otherwise the existing dist/*.8xp are reused with a warning.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/dist"
VERSION="$(cat "$ROOT/VERSION")"
PKG="calcscreen-update.tar.gz"
MANIFEST="calcscreen-update.manifest"
# Fixed timestamp for reproducibility (2020-01-01 UTC).
EPOCH=1577836800

LIST=""
cleanup() { [ -n "$LIST" ] && rm -f "$LIST"; }
trap cleanup EXIT

log() { printf '[make_dist] %s\n' "$*" >&2; }

build_calc() {
    if ! command -v cedev-config >/dev/null 2>&1; then
        log "WARNING: CE toolchain (cedev-config) not found; reusing existing dist/*.8xp"
        return 1
    fi
    log "building calculator programs"
    make -C "$ROOT/calc/dock" clean >/dev/null 2>&1 || true
    make -C "$ROOT/calc/badapple" clean >/dev/null 2>&1 || true
    make -C "$ROOT/calc/dock" >/dev/null
    make -C "$ROOT/calc/badapple" >/dev/null
    return 0
}

main() {
    mkdir -p "$DIST"

    if build_calc; then
        cp "$ROOT/calc/dock/bin/DOCK.8xp" "$DIST/DOCK.8xp"
        cp "$ROOT/calc/badapple/bin/BADAPPLE.8xp" "$DIST/BADAPPLE.8xp"
    elif [ ! -f "$DIST/DOCK.8xp" ] || [ ! -f "$DIST/BADAPPLE.8xp" ]; then
        log "ERROR: no CE toolchain and no existing dist/*.8xp to publish"
        exit 1
    fi

    printf '%s\n' "$VERSION" > "$DIST/VERSION"

    # Source bundle: everything needed to rebuild dockd on the Pi, plus the
    # prebuilt calc programs. Exclude VCS, build output and dist/ itself.
    log "packaging $PKG (version $VERSION)"
    LIST="$(mktemp)"
    ( cd "$ROOT" && git ls-files \
        core host pi tools calc Makefile VERSION README.md docs \
        2>/dev/null ) | sort > "$LIST"
    # Add the prebuilt calc binaries (not tracked by git / gitignored).
    {
        echo "calc/dock/bin/DOCK.8xp"
        echo "calc/badapple/bin/BADAPPLE.8xp"
    } >> "$LIST"
    LC_ALL=C sort -u -o "$LIST" "$LIST"

    tar --create --gzip \
        --owner=0 --group=0 --numeric-owner \
        --mtime="@$EPOCH" \
        --format=gnu \
        --no-recursion \
        --directory="$ROOT" \
        --files-from="$LIST" \
        --file="$DIST/$PKG.tmp"
    mv "$DIST/$PKG.tmp" "$DIST/$PKG"

    local sha size date
    sha="$(sha256sum "$DIST/$PKG" | cut -d' ' -f1)"
    size="$(stat -c%s "$DIST/$PKG")"
    date="$(date -u -d "@$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo "$EPOCH")" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ)"
    cat > "$DIST/$MANIFEST" <<EOF
format=1
product=calcscreen
version=$VERSION
date=$date
file=$PKG
size=$size
sha256=$sha
min_format=1
EOF
    log "done: $PKG ($size bytes), sha256 $sha"
    log "artifacts in $DIST:"
    ls -1 "$DIST" >&2
}

main "$@"
