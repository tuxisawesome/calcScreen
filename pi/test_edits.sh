#!/usr/bin/env bash
# test_edits.sh - offline tests for the cmdline.txt / config.txt editing in
# pi/install.sh (the part that can make a Pi unbootable if it goes wrong).
# Runs anywhere, needs no root and touches nothing outside a temp dir.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR source=install.sh
CALCSCREEN_LIB_ONLY=1 source "$here/install.sh"
set +e

pass=0 fail=0
check() { # check NAME GOT WANT
    if [[ $2 == "$3" ]]; then pass=$((pass + 1)); echo "ok   - $1"
    else fail=$((fail + 1)); printf 'FAIL - %s\n  got:  %q\n  want: %q\n' "$1" "$2" "$3"; fi
}

ADD="console=tty3 quiet loglevel=0 logo.nologo vt.global_cursor_default=0 systemd.show_status=0 rd.systemd.show_status=0 rd.udev.log_level=0 udev.log_priority=0 consoleblank=0"

# --- cmdline.txt ------------------------------------------------------------
stock="console=serial0,115200 console=tty1 root=PARTUUID=4e639091-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB"
exp="console=serial0,115200 root=PARTUUID=4e639091-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB $ADD"
got=$(cmdline_install "$stock")
check "cmdline: stock Bookworm" "$got" "$exp"
check "cmdline: idempotent" "$(cmdline_install "$got")" "$exp"
check "cmdline: single line" "$(printf '%s\n' "$got" | wc -l | tr -d ' ')" "1"

check "cmdline: CRLF, tabs, extra spaces, trailing newline" \
    "$(cmdline_install $'console=tty1\troot=/dev/mmcblk0p2   rootwait quiet loglevel=3\r\n')" \
    "root=/dev/mmcblk0p2 rootwait fsck.repair=yes $ADD"

check "cmdline: keeps overlayroot=tmpfs first, no duplicate fsck.repair" \
    "$(cmdline_install "overlayroot=tmpfs $stock")" "overlayroot=tmpfs $exp"

check "cmdline: replaces other values of our keys" \
    "$(cmdline_install "root=X rootwait fsck.repair=no loglevel=7 consoleblank=600 udev.log_priority=3 vt.global_cursor_default=1")" \
    "root=X rootwait fsck.repair=no $ADD"

check "cmdline: uninstall with backup restores original verbatim" \
    "$(cmdline_uninstall "$exp" "$stock")" "$stock"
check "cmdline: uninstall keeps later edits (overlay token) and console=tty1" \
    "$(cmdline_uninstall "foo=1 $exp" "$stock")" \
    "foo=1 console=serial0,115200 root=PARTUUID=4e639091-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB console=tty1"
check "cmdline: uninstall drops the fsck.repair we added" \
    "$(cmdline_uninstall "root=X rootwait fsck.repair=yes $ADD" "root=X rootwait console=tty1")" \
    "root=X rootwait console=tty1"
check "cmdline: uninstall without backup" \
    "$(cmdline_uninstall "root=X rootwait fsck.repair=yes $ADD")" \
    "root=X rootwait fsck.repair=yes console=tty1"

if (cmdline_check "console=tty1 rootwait") 2>/dev/null; then
    check "cmdline: refuses a line without root=" "accepted" "refused"
else check "cmdline: refuses a line without root=" "refused" "refused"; fi

# --- config.txt ---------------------------------------------------------------
cfg_stock='# For more options and information see
# http://rptl.io/configtxt
dtparam=audio=on
camera_auto_detect=1
display_auto_detect=1
auto_initramfs=1

# Enable DRM VC4 V3D driver
dtoverlay=vc4-kms-v3d
max_framebuffers=2
disable_fw_kms_setup=1
arm_64bit=1
disable_overscan=1
arm_boost=1

[cm4]
otg_mode=1

[cm5]
dtoverlay=dwc2,dr_mode=host

[all]'
block="$CONFIG_BEGIN
[all]
disable_splash=1
boot_delay=0
# dtoverlay=vc4-kms-v3d  (already set above)
dtparam=watchdog=on
dtparam=audio=on
$CONFIG_END"
got=$(config_install "$cfg_stock")
check "config: block appended" "$got" "$cfg_stock

$block"
check "config: idempotent" "$(config_install "$got")" "$got"
check "config: uninstall restores original" "$(config_strip "$got")" "$cfg_stock"
DISABLE_BT=1 FORCE_HOTPLUG=1
got2=$(config_install "$got")
check "config: --disable-bt --force-hotplug" "$(grep -cE '^(dtoverlay=disable-bt|hdmi_force_hotplug=1)$' <<< "$got2")" "2"
check "config: still one block" "$(grep -c '^# >>> calcScreen' <<< "$got2")" "1"
# shellcheck disable=SC2034 # read by config_install from install.sh
DISABLE_BT=0 FORCE_HOTPLUG=0
check "config: flags removed again" "$(config_install "$got2")" "$got"
check "config: adds kms overlay when missing" \
    "$(config_install 'arm_64bit=1' | grep -c '^dtoverlay=vc4-kms-v3d$')" "1"
check "config: user lines after our block survive" \
    "$(config_strip "$got
gpu_mem=64")" "$cfg_stock

gpu_mem=64"

echo "test_edits: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
