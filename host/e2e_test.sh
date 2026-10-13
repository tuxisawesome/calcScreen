#!/bin/sh
# host/e2e_test.sh - headless end-to-end test of the Pi side.
#
# Builds dockd and dock-sim, prepares a fake USB drive (USB0 with a WAV, a
# BAV and an escaping symlink) and a state directory, starts
#   dockd --transport unix:SOCK --display ppm:MIRROR --audio null ...
# and runs `dock-sim --selftest`, which drives the public calculator API
# through the real protocol engine and dockd. Finally checks that the mirror
# PPM written by dockd is a valid 320x240 image identical to the simulator's
# framebuffer. Exit status is non-zero on any failure.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build}
T=$(mktemp -d "${TMPDIR:-/tmp}/dock-e2e.XXXXXX")
DOCKD_PID=

cleanup() {
    if [ -n "$DOCKD_PID" ] && kill -0 "$DOCKD_PID" 2>/dev/null; then
        kill "$DOCKD_PID" 2>/dev/null || true
        wait "$DOCKD_PID" 2>/dev/null || true
    fi
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

fail() {
    echo "E2E FAIL: $*" >&2
    if [ -f "$T/dockd.log" ]; then
        echo "--- dockd log ---" >&2
        tail -n 40 "$T/dockd.log" >&2
    fi
    exit 1
}

echo "== build"
make -s -C "$ROOT/host" BUILD="$BUILD" all

echo "== prepare drive and state in $T"
mkdir -p "$T/drives" "$T/state"
"$BUILD/dock-sim" --gen-files "$T/drives" || fail "could not create test files"
[ -f "$T/drives/USB0/test.wav" ] && [ -f "$T/drives/USB0/test.bav" ] ||
    fail "test files missing"

echo "== start dockd"
"$BUILD/dockd" -v --transport "unix:$T/sock" --display "ppm:$T/mirror.ppm" \
    --audio null --drives "$T/drives" --state "$T/state" --nmcli none \
    --input-dir "$T/no-input" --usb-sysfs "$T/no-sysfs" \
    >"$T/dockd.log" 2>&1 &
DOCKD_PID=$!

i=0
while [ ! -S "$T/sock" ]; do
    i=$((i + 1))
    [ $i -le 50 ] || fail "dockd did not create its socket"
    kill -0 "$DOCKD_PID" 2>/dev/null || fail "dockd exited early"
    sleep 0.1
done

echo "== run simulator self-test"
if ! timeout 60 "$BUILD/dock-sim" --selftest --expect-ppm "$T/expected.ppm" "$T/sock" \
    >"$T/sim.log" 2>&1; then
    cat "$T/sim.log"
    fail "simulator self-test failed"
fi
cat "$T/sim.log"

echo "== stop dockd"
kill "$DOCKD_PID"
wait "$DOCKD_PID" || fail "dockd exited with an error"
DOCKD_PID=

echo "== check mirror"
[ -f "$T/mirror.ppm" ] || fail "mirror.ppm was not written"
size=$(wc -c <"$T/mirror.ppm")
[ "$size" -eq $((15 + 320 * 240 * 3)) ] || fail "mirror.ppm has size $size"
[ "$(head -c 15 "$T/mirror.ppm")" = "$(printf 'P6\n320 240\n255\n')" ] ||
    fail "mirror.ppm header is not P6 320x240"
cmp -s "$T/mirror.ppm" "$T/expected.ppm" ||
    fail "mirror.ppm differs from the simulator framebuffer"
[ -f "$T/mirror.ppm.ui.ppm" ] || fail "UI snapshot missing"
ls "$T/state"/settings.* >/dev/null 2>&1 || fail "settings were not saved"
grep -q "calc 51a1c0de connected" "$T/dockd.log" || fail "dockd did not log the connection"

echo "E2E PASS"
