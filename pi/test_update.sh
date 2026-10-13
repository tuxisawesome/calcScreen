#!/usr/bin/env bash
# test_update.sh - offline tests for pi/dock-update: the resumable phase
# machine, the progress bar, and power-loss recovery. Runs anywhere, needs no
# root, touches nothing outside a temp dir. Uses env stubs for make/unlock/
# lock/reboot and a fake DESTDIR root, plus a shadow PATH for systemctl/udevadm.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$here/dock-update"

pass=0 fail=0
check() { # check NAME GOT WANT
    if [[ $2 == "$3" ]]; then pass=$((pass + 1)); echo "ok   - $1"
    else fail=$((fail + 1)); printf 'FAIL - %s\n  got:  %q\n  want: %q\n' "$1" "$2" "$3"; fi
}
checkf() { # checkf NAME FILE   (file exists)
    if [[ -e $2 ]]; then pass=$((pass + 1)); echo "ok   - $1"
    else fail=$((fail + 1)); echo "FAIL - $1 (missing $2)"; fi
}
grepc() { # grepc NAME HAYSTACKFILE NEEDLE
    if grep -qF -- "$3" "$2"; then pass=$((pass + 1)); echo "ok   - $1"
    else fail=$((fail + 1)); echo "FAIL - $1 (no '$3')"; fi
}

T=$(mktemp -d "${TMPDIR:-/tmp}/dock-update-test.XXXXXX")
trap 'rm -rf "$T"' EXIT

# ---- fake toolchain / stubs ------------------------------------------------
BIN="$T/bin"
mkdir -p "$BIN"
cat > "$BIN/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >> "$T/calls.log"
EOF
cat > "$BIN/udevadm" <<EOF
#!/bin/sh
echo "udevadm \$*" >> "$T/calls.log"
EOF
cat > "$T/fakemake" <<EOF
#!/bin/bash
set -euo pipefail
dir=""; do_install=0; prefix=""; destdir=""
while [ \$# -gt 0 ]; do
    case \$1 in
        -C) dir=\$2; shift 2;;
        install) do_install=1; shift;;
        PREFIX=*) prefix=\${1#PREFIX=}; shift;;
        DESTDIR=*) destdir=\${1#DESTDIR=}; shift;;
        *) shift;;
    esac
done
root="\$dir/.."
if [ "\$do_install" = 1 ]; then
    mkdir -p "\$destdir\$prefix/bin"
    cp "\$root/build/dockd" "\$destdir\$prefix/bin/dockd"
else
    mkdir -p "\$root/build/obj"
    i=0
    for c in "\$root"/core/*.c "\$root"/host/*/*.c; do
        [ -f "\$c" ] || continue
        : > "\$root/build/obj/u\$i.o"; i=\$((i + 1))
    done
    printf '#!/bin/sh\necho fake-dockd\n' > "\$root/build/dockd"
    chmod +x "\$root/build/dockd"
fi
EOF
for s in unlock lock reboot; do
    cat > "$T/stub-$s" <<EOF
#!/bin/sh
echo "$s \$*" >> "$T/calls.log"
EOF
done
chmod +x "$BIN"/* "$T/fakemake" "$T"/stub-*

export PATH="$BIN:$PATH"
export DOCK_MAKE_CMD="$T/fakemake"
export DOCK_UNLOCK_CMD="$T/stub-unlock"
export DOCK_LOCK_CMD="$T/stub-lock"
export DOCK_REBOOT_CMD="$T/stub-reboot"
export DOCK_DESTDIR="$T/root"
export DOCK_PREFIX=/usr/local
export DOCK_SBIN_DIR=/usr/local/sbin
export DOCK_UNIT_DIR=/etc/systemd/system
export DOCK_UDEV_DIR=/etc/udev/rules.d
export DOCK_CONSOLE="$T/console"  # draw the progress bar where tests can see it

# ---- a fake source bundle + manifest ---------------------------------------
make_bundle() { # make_bundle VERSION  -> prints "TAR MANIFEST"
    local ver=$1
    local src="$T/src.$ver" tar="$T/pkg.$ver.tar.gz" man="$T/man.$ver"
    rm -rf "$src"; mkdir -p "$src/core" "$src/host/dockd" "$src/host/hal_linux" "$src/pi"
    echo 'int a;' > "$src/core/a.c"
    echo 'int b;' > "$src/core/b.c"
    echo 'int m;' > "$src/host/dockd/main.c"
    echo 'int u;' > "$src/host/hal_linux/util.c"
    printf '#!/bin/sh\n:\n' > "$src/pi/dock-update"
    printf '#!/bin/sh\n:\n' > "$src/pi/dock-lock"
    printf '[Unit]\n' > "$src/pi/dockd.service"
    printf '[Unit]\n' > "$src/pi/dock-update.service"
    printf '# rules\n' > "$src/pi/99-calcscreen.rules"
    chmod +x "$src/pi/dock-update" "$src/pi/dock-lock"
    echo "$ver" > "$src/VERSION"
    tar czf "$tar" -C "$src" .
    local sha size
    sha=$(sha256sum "$tar" | cut -d' ' -f1)
    size=$(stat -c%s "$tar")
    cat > "$man" <<EOF2
format=1
product=calcscreen
version=$ver
file=calcscreen-update.tar.gz
size=$size
sha256=$sha
min_format=1
EOF2
    echo "$tar $man"
}

run_update() { # run_update STATE ARGS...  -> stdout to $T/out, stderr to $T/err
    local st=$1; shift
    "$SCRIPT" "$@" --state "$st" > "$T/out" 2> "$T/err" || echo "rc=$?" >> "$T/err"
}

# ===========================================================================
echo "== bash -n / shellcheck"
bash -n "$SCRIPT" && check "bash -n clean" ok ok
if command -v shellcheck >/dev/null 2>&1; then
    if shellcheck "$SCRIPT" >/dev/null 2>&1; then check "shellcheck clean" ok ok
    else check "shellcheck clean" fail ok; fi
else
    echo "ok   - shellcheck not installed (skipped)"; pass=$((pass + 1))
fi

echo "== sourceable library mode"
# shellcheck source=dock-update
CALCSCREEN_LIB_ONLY=1 source "$SCRIPT"
read -r TAR MAN < <(make_bundle 1.0.0)
check "manifest_field version" "$(manifest_field "$MAN" version)" "1.0.0"
check "overlay_status forced active" "$(DOCK_OVERLAY_FORCE=active overlay_status)" "active"
check "overlay_status forced inactive" "$(DOCK_OVERLAY_FORCE=inactive overlay_status)" "inactive"
if (set_dirs "$T/s0"; verify_pkg "$TAR" "$MAN") 2>/dev/null; then
    check "verify good package" ok ok; else check "verify good package" fail ok; fi
cp "$TAR" "$T/bad.tar.gz"; printf 'x' >> "$T/bad.tar.gz"
if (set_dirs "$T/s0"; verify_pkg "$T/bad.tar.gz" "$MAN") 2>/dev/null; then
    check "verify corrupt package rejected" fail ok; else check "verify corrupt package rejected" ok ok; fi

echo "== overlay INACTIVE: stage applies now"
S1="$T/state1"; rm -rf "$T/root" "$S1"
read -r TAR MAN < <(make_bundle 1.0.0)
DOCK_OVERLAY_FORCE=inactive run_update "$S1" stage --pkg "$TAR" --manifest "$MAN"
grepc "stage(inactive) prints PROGRESS" "$T/out" "PROGRESS "
grepc "stage(inactive) prints bar on console" "$T/console" "["
grepc "stage(inactive) RESULT ok 1.0.0" "$T/out" "RESULT ok 1.0.0"
checkf "dockd installed to fake root" "$T/root/usr/local/bin/dockd"
checkf "pi script refreshed" "$T/root/usr/local/sbin/dock-update"
check "phase is done" "$(cat "$S1/update/phase")" "done"
check "result is ok" "$(cat "$S1/update/result")" "ok 1.0.0"

echo "== overlay ACTIVE: stage -> unlock reboot -> apply reboot"
S2="$T/state2"; rm -rf "$T/root" "$S2" "$T/calls.log"
read -r TAR MAN < <(make_bundle 2.1.0)
DOCK_OVERLAY_FORCE=active run_update "$S2" stage --pkg "$TAR" --manifest "$MAN"
grepc "stage(active) STAGED-REBOOT" "$T/out" "STAGED-REBOOT"
check "phase staged" "$(cat "$S2/update/phase")" "staged"
checkf "package staged in pending" "$S2/update/pending/calcscreen-update.tar.gz"

# boot 1: overlay still active -> unlock + reboot
DOCK_OVERLAY_FORCE=active run_update "$S2" resume
grepc "resume#1 UNLOCK-REBOOT" "$T/out" "UNLOCK-REBOOT"
check "phase unlocked" "$(cat "$S2/update/phase")" "unlocked"
grepc "unlock was called" "$T/calls.log" "unlock"
grepc "reboot was called" "$T/calls.log" "reboot"

# boot 2: root now writable -> build/install/relock/reboot
DOCK_OVERLAY_FORCE=inactive run_update "$S2" resume
grepc "resume#2 RESULT ok 2.1.0" "$T/out" "RESULT ok 2.1.0"
check "phase done" "$(cat "$S2/update/phase")" "done"
check "result ok 2.1.0" "$(cat "$S2/update/result")" "ok 2.1.0"
checkf "dockd installed (active path)" "$T/root/usr/local/bin/dockd"
grepc "re-lock was called" "$T/calls.log" "lock"

# boot 3: nothing pending -> no-op
: > "$T/out"
DOCK_OVERLAY_FORCE=inactive run_update "$S2" resume
check "resume#3 is a no-op (no output)" "$(tr -d '[:space:]' < "$T/out")" ""

echo "== resumable: crash with phase=building, re-run continues"
S3="$T/state3"; rm -rf "$T/root" "$S3"
read -r TAR MAN < <(make_bundle 3.0.0)
mkdir -p "$S3/update/pending"
cp "$TAR" "$S3/update/pending/calcscreen-update.tar.gz"
cp "$MAN" "$S3/update/pending/calcscreen-update.manifest"
printf 'building\n' > "$S3/update/phase"   # simulate a power cut mid-build
DOCK_OVERLAY_FORCE=inactive run_update "$S3" resume
grepc "resume from building completes" "$T/out" "RESULT ok 3.0.0"
check "phase done after resume" "$(cat "$S3/update/phase")" "done"

echo "== resume with no state at all is a clean no-op (exit 0)"
: > "$T/out" "$T/err"
run_update "$T/nostate" resume
check "no-state resume no output" "$(tr -d '[:space:]' < "$T/out")" ""
if grep -q 'rc=' "$T/err"; then check "no-state resume exit 0" fail ok; else check "no-state resume exit 0" ok ok; fi

echo "== --dry-run does not touch the system"
S4="$T/state4"; rm -rf "$T/root" "$S4" "$T/calls.log"
read -r TAR MAN < <(make_bundle 4.0.0)
DOCK_OVERLAY_FORCE=active run_update "$S4" stage --pkg "$TAR" --manifest "$MAN" --dry-run
grepc "dry-run stage STAGED-REBOOT" "$T/out" "STAGED-REBOOT"
DOCK_OVERLAY_FORCE=active run_update "$S4" resume --dry-run
grepc "dry-run resume prints [dry-run]" "$T/err" "[dry-run]"
grepc "dry-run resume UNLOCK-REBOOT" "$T/out" "UNLOCK-REBOOT"
if [[ -e $T/calls.log ]]; then check "dry-run invoked no real stubs" fail ok
else check "dry-run invoked no real stubs" ok ok; fi

echo
echo "test_update: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
