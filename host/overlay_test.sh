#!/bin/sh
# host/overlay_test.sh - headless check of the diagnostics performance overlay.
#
# Runs dockd three ways over the Unix transport, each driven by the simulator
# self-test, and compares the resulting mirror PPMs:
#   base  no overlay
#   all   --perf-overlay (every section)
#   sys   --perf-overlay --perf-sections 4 (system stats only, a shorter panel)
# Asserts the overlay changes the top-left pixels (vs base) and that enabling
# fewer sections changes the panel height/content (all vs sys). Headless.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build}
T=$(mktemp -d "${TMPDIR:-/tmp}/dock-ovl.XXXXXX")
DOCKD_PID=

cleanup() {
    if [ -n "$DOCKD_PID" ] && kill -0 "$DOCKD_PID" 2>/dev/null; then
        kill "$DOCKD_PID" 2>/dev/null || true
        wait "$DOCKD_PID" 2>/dev/null || true
    fi
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

fail() { echo "OVERLAY FAIL: $*" >&2; [ -f "$T/dockd.log" ] && tail -n 20 "$T/dockd.log" >&2; exit 1; }

echo "== build"
make -s -C "$ROOT/host" BUILD="$BUILD" all

mkdir -p "$T/drives" "$T/state" "$T/noin" "$T/nosys"
"$BUILD/dock-sim" --gen-files "$T/drives" >/dev/null || fail "could not create test files"

# run_once OUT.ppm [extra dockd args...]
run_once() {
    out=$1; shift
    rm -f "$T/sock"
    "$BUILD/dockd" --transport "unix:$T/sock" --display "ppm:$out" \
        --audio null --drives "$T/drives" --state "$T/state" --nmcli none \
        --input-dir "$T/noin" --usb-sysfs "$T/nosys" "$@" \
        >"$T/dockd.log" 2>&1 &
    DOCKD_PID=$!
    i=0
    while [ ! -S "$T/sock" ]; do
        i=$((i + 1))
        [ $i -le 50 ] || fail "dockd did not create its socket"
        kill -0 "$DOCKD_PID" 2>/dev/null || fail "dockd exited early"
        sleep 0.1
    done
    timeout 60 "$BUILD/dock-sim" --selftest "$T/sock" >/dev/null 2>&1 ||
        fail "simulator self-test failed"
    # Give the loop a moment to stamp the overlay, then stop (close writes ppm).
    sleep 0.3
    kill "$DOCKD_PID" 2>/dev/null || true
    wait "$DOCKD_PID" 2>/dev/null || true
    DOCKD_PID=
    [ -f "$out" ] || fail "$out not written"
}

echo "== render base / overlay-all / overlay-sys"
run_once "$T/base.ppm"
run_once "$T/all.ppm" --perf-overlay
run_once "$T/sys.ppm" --perf-overlay --perf-sections 4

echo "== compare"
cmp -s "$T/base.ppm" "$T/all.ppm" && fail "overlay did not change the mirror"
cmp -s "$T/all.ppm" "$T/sys.ppm" && fail "section toggle did not change the panel"

python3 -I - "$T/base.ppm" "$T/all.ppm" "$T/sys.ppm" <<'PY' || exit 1
import sys
def load(p):
    with open(p, 'rb') as f:
        d = f.read()
    assert d[:15] == b'P6\n320 240\n255\n', "bad ppm header"
    return d[15:]
def px(buf, x, y):
    o = (y * 320 + x) * 3
    return buf[o:o+3]
base, allp, sysp = (load(p) for p in sys.argv[1:4])
# Top-left panel pixel: present with the overlay, not in the plain mirror.
assert px(base, 2, 2) != px(allp, 2, 2), "top-left panel not drawn"
# A row inside the full panel (8 lines ~ 75 px) but below the 3-line sys panel
# (~33 px): the panel height/content must differ.
assert px(allp, 2, 48) != px(sysp, 2, 48), "panel height/content did not change"
print("overlay pixel checks: ok")
PY

echo "OVERLAY PASS"
