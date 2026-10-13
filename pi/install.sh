#!/bin/bash
# install.sh - turn a Raspberry Pi OS Lite (64-bit, Bookworm or Trixie) Pi
# into a calcScreen dock. Run it ON the Pi, from the cloned repository:
#
#   sudo ./pi/install.sh [options]
#
# It is idempotent: run it again after `git pull` to update.
#
# Options:
#   --dry-run              show what would be done, change nothing
#   --no-overlay           do not enable the read-only root overlay at the end
#   --disable-bt           also turn the Bluetooth hardware off (dtoverlay=disable-bt)
#   --force-hotplug        add hdmi_force_hotplug=1 (output even with no monitor at boot)
#   --make-data-partition  create the 'dockdata' partition in free space at the end
#                          of the SD card if there is some (never shrinks anything;
#                          otherwise prints how to make room on a PC)
#   --unlock               disable the overlay and read-only /boot, then reboot
#   --uninstall            undo the system changes (keeps user 'dock' and settings)
#   -h, --help             this help
#
# See pi/README.md for the full setup guide.
set -euo pipefail

# ---------------------------------------------------------------------------
# constants
# ---------------------------------------------------------------------------
SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")

BIN_DIR=/usr/local/bin
SBIN_DIR=/usr/local/sbin
UNIT_DIR=/etc/systemd/system
STATE_DIR=/var/lib/dockd
MEDIA_DIR=/media/dock
CS_ETC=/etc/calcscreen
DOCK_USER=dock
DATA_LABEL=dockdata
DATA_MIN_MIB=64
BAK=.calcscreen.bak

DATA_MOUNT_UNIT=var-lib-dockd.mount
# systemd-escape --path /etc/NetworkManager/system-connections
NM_CONN_UNIT='etc-NetworkManager-system\x2dconnections.mount'
NM_STATE_UNIT=var-lib-NetworkManager.mount
DATA_DEV_UNIT='dev-disk-by\x2dlabel-dockdata.device'
DATA_FSCK_UNIT='systemd-fsck@dev-disk-by\x2dlabel-dockdata.service'

CONFIG_BEGIN='# >>> calcScreen (managed by pi/install.sh, do not edit inside this block) >>>'
CONFIG_END='# <<< calcScreen <<<'

# Kernel command line we want (console on tty3 so tty1 stays clean for dockd).
CMDLINE_ADD=(console=tty3 quiet loglevel=0 logo.nologo vt.global_cursor_default=0
             systemd.show_status=0 rd.systemd.show_status=0 rd.udev.log_level=0
             udev.log_priority=0 consoleblank=0)
# Added only when missing: repair the root fs automatically after power loss.
CMDLINE_ENSURE=(fsck.repair=yes)

# Services that are not needed on an appliance (masked; unmasked by --uninstall).
UNNEEDED_UNITS=(bluetooth.service hciuart.service avahi-daemon.service avahi-daemon.socket
                triggerhappy.service triggerhappy.socket ModemManager.service
                apt-daily.timer apt-daily-upgrade.timer man-db.timer dpkg-db-backup.timer
                e2scrub_reap.service e2scrub_all.timer dphys-swapfile.service
                rpi-eeprom-update.service rsyslog.service)

BUILD_DEPS=(build-essential pkg-config libdrm-dev libasound2-dev libusb-1.0-0-dev
            libcurl4-openssl-dev)
RUNTIME_DEPS=(kbd sudo util-linux dosfstools exfatprogs e2fsprogs alsa-utils
              network-manager raspi-config overlayroot)

# ---------------------------------------------------------------------------
# options
# ---------------------------------------------------------------------------
DRY_RUN=0
NO_OVERLAY=0
DISABLE_BT=0
FORCE_HOTPLUG=0
MAKE_DATA=0
MODE=install

usage() { sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

parse_args() {
    local a
    for a in "$@"; do
        case $a in
            --dry-run) DRY_RUN=1 ;;
            --no-overlay) NO_OVERLAY=1 ;;
            --disable-bt) DISABLE_BT=1 ;;
            --force-hotplug) FORCE_HOTPLUG=1 ;;
            --make-data-partition) MAKE_DATA=1 ;;
            --unlock) MODE=unlock ;;
            --uninstall) MODE=uninstall ;;
            -h|--help) usage; exit 0 ;;
            *) echo "install.sh: unknown option '$a' (try --help)" >&2; exit 2 ;;
        esac
    done
}

# ---------------------------------------------------------------------------
# logging and change helpers
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    C_B=$'\e[1m' C_G=$'\e[32m' C_Y=$'\e[33m' C_R=$'\e[31m' C_0=$'\e[0m'
else
    C_B='' C_G='' C_Y='' C_R='' C_0=''
fi
step() { echo; echo "${C_B}==> $*${C_0}"; }
info() { echo "    $*"; }
ok()   { echo "    ${C_G}ok${C_0} $*"; }
warn() { echo "${C_Y}WARNING:${C_0} $*" >&2; }
die()  { echo "${C_R}ERROR:${C_0} $*" >&2; exit 1; }

# run CMD...: execute, or only print it with --dry-run.
run() {
    if ((DRY_RUN)); then
        printf '    [dry-run]'
        printf ' %q' "$@"
        echo
    else
        "$@"
    fi
}

# backup_once FILE: keep the very first original as FILE.calcscreen.bak.
backup_once() {
    local f=$1
    [[ -e $f && ! -e $f$BAK ]] || return 0
    run cp -a -- "$f" "$f$BAK"
    info "backup: $f$BAK"
}

# write_file DEST MODE  (content on stdin). MODE "-" = leave mode alone (FAT).
# Writes to a temp file in the same directory, fsyncs, then renames.
write_file() {
    local dest=$1 mode=$2 content tmp
    content=$(cat; printf x)
    content=${content%x}
    if [[ -f $dest ]] && [[ "$(cat -- "$dest"; printf x)" == "${content}x" ]]; then
        if [[ $mode != - && $(stat -c %a -- "$dest") != "${mode#0}" ]]; then
            run chmod "$mode" -- "$dest"
        fi
        ok "unchanged: $dest"
        return 0
    fi
    if ((DRY_RUN)); then
        if [[ -f $dest ]]; then
            info "[dry-run] would change $dest:"
            diff -u --label "$dest" --label "$dest (new)" -- "$dest" <(printf '%s' "$content") |
                sed 's/^/        /' || true
        else
            info "[dry-run] would create $dest ($(printf '%s' "$content" | wc -l | tr -d ' ') lines)"
        fi
        return 0
    fi
    mkdir -p -- "$(dirname -- "$dest")"
    tmp=$(mktemp -- "$dest.XXXXXX")
    printf '%s' "$content" > "$tmp"
    if [[ $mode != - ]]; then
        chmod "$mode" -- "$tmp"
        chown root:root -- "$tmp"
    fi
    sync -- "$tmp" 2>/dev/null || sync
    mv -f -- "$tmp" "$dest"
    sync
    ok "wrote $dest"
}

install_file() { write_file "$2" "$3" < "$1"; }

dry_suffix() { if ((DRY_RUN)); then echo " (dry run: nothing is changed)"; fi; }

remove_path() {
    local p
    for p in "$@"; do
        if [[ -e $p || -L $p ]]; then
            run rm -rf -- "$p"
            info "removed $p"
        fi
    done
}

unit_exists() { [[ -n $(systemctl list-unit-files --no-legend -- "$1" 2>/dev/null) ]]; }

# ---------------------------------------------------------------------------
# environment detection
# ---------------------------------------------------------------------------
BOOT=
detect_boot() {
    if [[ -n ${CALCSCREEN_BOOT_DIR:-} ]]; then
        BOOT=$CALCSCREEN_BOOT_DIR      # test hook
    elif [[ -f /boot/firmware/cmdline.txt ]]; then
        BOOT=/boot/firmware            # Bookworm, Trixie
    elif [[ -f /boot/cmdline.txt ]]; then
        BOOT=/boot                     # Bullseye and older
    else
        if ((DRY_RUN)); then
            warn "no cmdline.txt in /boot/firmware or /boot (not a Raspberry Pi?)"
            BOOT=/boot/firmware
            return 0
        fi
        die "no cmdline.txt in /boot/firmware or /boot - is this Raspberry Pi OS?"
    fi
}

overlay_active() {
    [[ $(findmnt -no FSTYPE / 2>/dev/null) == overlay ]] || grep -qw 'overlayroot=tmpfs' /proc/cmdline
}

# whole disk that holds the boot partition, e.g. /dev/mmcblk0
boot_disk() {
    local src pk
    src=$(findmnt -nvo SOURCE -M "$BOOT" 2>/dev/null || true)
    pk=$(lsblk -dno PKNAME "$src" 2>/dev/null || true)
    if [[ -n $pk ]]; then echo "/dev/$pk"; else echo /dev/mmcblk0; fi
}

boot_is_ro() { findmnt -no OPTIONS -M "$BOOT" 2>/dev/null | grep -qE '(^|,)ro(,|$)'; }

preflight() {
    if [[ $EUID -ne 0 ]]; then
        if ((DRY_RUN)); then
            warn "not root: dry-run only, some checks may be incomplete"
        else
            die "run as root: sudo $0 $*"
        fi
    fi
    detect_boot
    info "repository: $REPO"
    info "boot partition: $BOOT"

    local codename="" arch=""
    if [[ -r /etc/os-release ]]; then
        codename=$(. /etc/os-release && echo "${VERSION_CODENAME:-}")
    fi
    case $codename in
        bookworm|trixie) info "OS: Raspberry Pi OS / Debian $codename" ;;
        *) warn "untested OS release '${codename:-unknown}' (expected bookworm or trixie)" ;;
    esac
    arch=$(dpkg --print-architecture 2>/dev/null || uname -m)
    [[ $arch == arm64 || $arch == aarch64 ]] ||
        warn "architecture is '$arch'; Raspberry Pi OS Lite 64-bit (arm64) is recommended"
    if [[ -r /proc/device-tree/model ]]; then
        info "hardware: $(tr -d '\0' < /proc/device-tree/model)"
    fi

    if overlay_active; then
        if ((DRY_RUN)); then
            warn "the read-only overlay is active: a real run would refuse"
        else
            die "the root filesystem is a read-only overlay; changes would be lost.
       Run 'sudo dock-unlock' (or '$0 --unlock'), reboot, then run this again."
        fi
    fi
    if boot_is_ro; then
        info "$BOOT is mounted read-only, remounting read-write for this run"
        run mount -o remount,rw "$BOOT"
    fi
}

# ---------------------------------------------------------------------------
# kernel command line (cmdline.txt must stay ONE line)
# ---------------------------------------------------------------------------
cmdline_owned() {
    case $1 in
        console=tty1|console=tty3|quiet|logo.nologo|loglevel=*|vt.global_cursor_default=*|\
        systemd.show_status=*|rd.systemd.show_status=*|rd.udev.log_level=*|\
        udev.log_priority=*|consoleblank=*) return 0 ;;
    esac
    return 1
}

# cmdline_install OLD -> new single line on stdout
cmdline_install() {
    local -a toks=() out=()
    local t e found
    read -r -a toks <<< "$(printf '%s' "$1" | tr '\r\n\t' '   ')"
    for t in "${toks[@]}"; do
        cmdline_owned "$t" || out+=("$t")
    done
    for e in "${CMDLINE_ENSURE[@]}"; do
        found=0
        for t in "${out[@]}"; do [[ ${t%%=*} == "${e%%=*}" ]] && found=1; done
        ((found)) || out+=("$e")
    done
    out+=("${CMDLINE_ADD[@]}")
    printf '%s\n' "${out[*]}"
}

# cmdline_uninstall CURRENT [BACKUP] -> restored single line on stdout.
# Drops every token we own or added unless the original (backup) had it,
# and puts back owned tokens the original had (e.g. console=tty1).
cmdline_uninstall() {
    local -a cur=() orig=() out=()
    local t have_backup=0
    read -r -a cur <<< "$(printf '%s' "$1" | tr '\r\n\t' '   ')"
    if [[ -n ${2:-} ]]; then
        have_backup=1
        read -r -a orig <<< "$(printf '%s' "$2" | tr '\r\n\t' '   ')"
    fi
    in_orig() { local x; for x in "${orig[@]}"; do [[ $x == "$1" ]] && return 0; done; return 1; }
    ours() { cmdline_owned "$1" || [[ " ${CMDLINE_ENSURE[*]} " == *" $1 "* ]]; }
    if ((have_backup)); then
        # Nothing else changed since the install: give back the original verbatim.
        local -a a=() b=()
        for t in "${cur[@]}"; do ours "$t" || a+=("$t"); done
        for t in "${orig[@]}"; do ours "$t" || b+=("$t"); done
        if [[ "${a[*]}" == "${b[*]}" ]]; then
            printf '%s\n' "${orig[*]}"
            return 0
        fi
    fi
    for t in "${cur[@]}"; do
        if cmdline_owned "$t" || { ((have_backup)) && [[ " ${CMDLINE_ENSURE[*]} " == *" $t "* ]]; }; then
            if ((have_backup)) && in_orig "$t"; then out+=("$t"); fi
        else
            out+=("$t")
        fi
    done
    if ((have_backup)); then
        for t in "${orig[@]}"; do
            if cmdline_owned "$t" && [[ " ${out[*]} " != *" $t "* ]]; then out+=("$t"); fi
        done
    elif [[ " ${out[*]} " != *" console=tty1 "* ]]; then
        out+=(console=tty1)
    fi
    printf '%s\n' "${out[*]}"
}

cmdline_check() {
    # refuse to write a command line that cannot boot
    [[ $1 == *root=* ]] || die "refusing to write a cmdline.txt without root= : '$1'"
    [[ $1 != *$'\n'*$'\n'* ]] || die "cmdline would span several lines"
}

edit_cmdline() {
    local f=$BOOT/cmdline.txt old new
    [[ -f $f ]] || { ((DRY_RUN)) && { warn "$f missing"; return 0; }; die "$f missing"; }
    old=$(cat -- "$f")
    if [[ $MODE == uninstall ]]; then
        local bak=""
        [[ -f $f$BAK ]] && bak=$(cat -- "$f$BAK")
        if [[ -n $bak ]]; then
            local r1 r2
            r1=$(grep -o 'root=[^ ]*' <<< "$old" || true)
            r2=$(grep -o 'root=[^ ]*' <<< "$bak" || true)
            [[ $r1 == "$r2" ]] || warn "root= changed since install; restoring only our own tokens"
        fi
        if [[ -z $bak && " $old " != *" console=tty3 "* ]]; then
            ok "unchanged: $f (no calcScreen settings found)"
            return 0
        fi
        new=$(cmdline_uninstall "$old" "$bak")
    else
        new=$(cmdline_install "$old")
    fi
    cmdline_check "$new"
    if [[ $new == "$old" ]]; then ok "unchanged: $f"; return 0; fi
    info "cmdline.txt: $new"
    backup_once "$f"
    printf '%s\n' "$new" | write_file "$f" -
}

# ---------------------------------------------------------------------------
# config.txt (one marked block at the end, under [all])
# ---------------------------------------------------------------------------
config_strip() {
    # (callers capture this with $(...), which drops trailing blank lines)
    awk -v b="${CONFIG_BEGIN:0:16}" -v e="${CONFIG_END:0:16}" '
        index($0, b) == 1 { skip = 1; next }
        skip && index($0, e) == 1 { skip = 0; next }
        !skip { print }' <<< "$1"
}

# config_install OLD -> new content on stdout
config_install() {
    local base line name
    local -a want=(disable_splash=1 boot_delay=0 dtoverlay=vc4-kms-v3d dtparam=watchdog=on
                   dtparam=audio=on)
    ((DISABLE_BT)) && want+=(dtoverlay=disable-bt)
    ((FORCE_HOTPLUG)) && want+=(hdmi_force_hotplug=1)
    base=$(config_strip "$1")
    printf '%s\n\n' "$base"
    printf '%s\n' "$CONFIG_BEGIN" "[all]"
    for line in "${want[@]}"; do
        if [[ $line == dtoverlay=* ]]; then
            # loading the same overlay twice is not harmless: skip if present
            name=${line#dtoverlay=}
            if grep -qE "^[[:space:]]*dtoverlay=${name}([,[:space:]]|$)" <<< "$base"; then
                printf '# %s  (already set above)\n' "$line"
                continue
            fi
        fi
        printf '%s\n' "$line"
    done
    printf '%s\n' "$CONFIG_END"
}

edit_config() {
    local f=$BOOT/config.txt old new
    [[ -f $f ]] || { ((DRY_RUN)) && { warn "$f missing"; return 0; }; die "$f missing"; }
    old=$(cat -- "$f")
    if [[ $MODE == uninstall ]]; then
        new=$(config_strip "$old")
    else
        new=$(config_install "$old")
        grep -q 'vc4-fkms-v3d' <<< "$new" &&
            warn "config.txt enables vc4-fkms-v3d; dockd needs full KMS (vc4-kms-v3d)"
    fi
    if [[ $new == "$old" ]]; then ok "unchanged: $f"; return 0; fi
    backup_once "$f"
    printf '%s\n' "$new" | write_file "$f" -
}

# ---------------------------------------------------------------------------
# install steps
# ---------------------------------------------------------------------------
install_packages() {
    step "Installing packages"
    local missing=() p
    for p in "${BUILD_DEPS[@]}" "${RUNTIME_DEPS[@]}"; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' || missing+=("$p")
    done
    if ((${#missing[@]} == 0)); then
        ok "all packages present"
        return 0
    fi
    info "missing: ${missing[*]}"
    run env DEBIAN_FRONTEND=noninteractive apt-get update ||
        die "apt-get update failed - is the Pi online? (sudo nmtui to set up Wi-Fi)"
    run env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing[@]}" ||
        die "apt-get install failed"
}

build_dockd() {
    step "Building dockd"
    if [[ ! -f $REPO/host/Makefile ]]; then
        ((DRY_RUN)) && { warn "$REPO/host/Makefile not found (skipping build in dry run)"; return 0; }
        die "$REPO/host/Makefile not found - run this from the calcScreen repo"
    fi
    local owner
    owner=$(stat -c %U -- "$REPO")
    # build as the owner of the checkout so `git pull` keeps working
    if [[ $owner != root ]] && id "$owner" >/dev/null 2>&1; then
        run runuser -u "$owner" -- make -C "$REPO/host" -j"$(nproc)"
    else
        run make -C "$REPO/host" -j"$(nproc)"
    fi
    local bin=""
    for bin in "$REPO/build/dockd" "$REPO/host/build/dockd" ""; do
        [[ -z $bin || -x $bin ]] && break
    done
    if [[ -z $bin ]]; then
        ((DRY_RUN)) && { info "[dry-run] would install build/dockd to $BIN_DIR/dockd"; return 0; }
        die "build finished but build/dockd was not found"
    fi
    run install -D -m 0755 -o root -g root -- "$bin" "$BIN_DIR/dockd"
    ok "installed $BIN_DIR/dockd (from $bin)"
}

setup_user() {
    step "User '$DOCK_USER' (for \"drop to shell\")"
    if id "$DOCK_USER" >/dev/null 2>&1; then
        ok "user exists"
    else
        run useradd --create-home --shell /bin/bash --comment "calcScreen dock shell" "$DOCK_USER"
    fi
    local g groups=()
    for g in sudo video audio input plugdev netdev dialout render; do
        getent group "$g" >/dev/null && groups+=("$g")
    done
    run usermod -aG "$(IFS=,; echo "${groups[*]}")" "$DOCK_USER"
    # no password: only `login -f` from dockd (behind the admin password) gets in
    run passwd -l "$DOCK_USER"
    local sudoers=/etc/sudoers.d/010_calcscreen-dock tmp
    local content="# calcScreen: the dock shell user may administer the Pi without a password.
# Reaching this shell requires physical access and the dock's admin password.
$DOCK_USER ALL=(ALL) NOPASSWD: ALL
"
    if ! ((DRY_RUN)); then
        tmp=$(mktemp)
        printf '%s' "$content" > "$tmp"
        visudo -cqf "$tmp" || { rm -f "$tmp"; die "generated sudoers file is invalid"; }
        rm -f "$tmp"
    fi
    printf '%s' "$content" | write_file "$sudoers" 0440
}

install_files() {
    step "Installing scripts, systemd unit and udev rules"
    install_file "$SCRIPT_DIR/dock-unlock" "$SBIN_DIR/dock-unlock" 0755
    install_file "$SCRIPT_DIR/dock-lock" "$SBIN_DIR/dock-lock" 0755
    install_file "$SCRIPT_DIR/dock-automount" "$SBIN_DIR/dock-automount" 0755
    install_file "$SCRIPT_DIR/dock-update" "$SBIN_DIR/dock-update" 0755
    install_file "$SCRIPT_DIR/dockd.service" "$UNIT_DIR/dockd.service" 0644
    install_file "$SCRIPT_DIR/dock-update.service" "$UNIT_DIR/dock-update.service" 0644
    install_file "$SCRIPT_DIR/99-calcscreen.rules" /etc/udev/rules.d/99-calcscreen.rules 0644

    write_file /etc/tmpfiles.d/calcscreen.conf 0644 <<EOF
# calcScreen: mount root for USB drives, state of the automount helper
d $MEDIA_DIR 0755 root root -
d /run/dock-automount 0755 root root -
EOF
    if [[ ! -e /etc/default/dockd ]]; then
        write_file /etc/default/dockd 0644 <<'EOF'
# Extra command-line options for dockd (see `dockd --help`), e.g.
#   DOCKD_OPTS="-v --audio hw:CARD=Headphones"
DOCKD_OPTS=""
EOF
    else
        ok "keeping /etc/default/dockd"
    fi

    # Hardware watchdog: reboot if the whole system hangs (bcm2835 max ~15 s).
    write_file /etc/systemd/system.conf.d/calcscreen-watchdog.conf 0644 <<'EOF'
# calcScreen: feed the hardware watchdog from PID 1; a hung system reboots.
[Manager]
RuntimeWatchdogSec=15
EOF
    # Journal in RAM only, bounded (the root fs is a RAM overlay anyway).
    write_file /etc/systemd/journald.conf.d/calcscreen.conf 0644 <<'EOF'
# calcScreen: keep logs in RAM, never write them to the SD card.
[Journal]
Storage=volatile
RuntimeMaxUse=16M
EOF

    step "Shell banner and login messages"
    write_file /etc/profile.d/calcscreen.sh 0644 <<'EOF'
# calcScreen dock shell banner (installed by pi/install.sh)
case $- in *i*) ;; *) return 0 2>/dev/null || exit 0 ;; esac
if [ -z "${CALCSCREEN_BANNER_SHOWN:-}" ]; then
    export CALCSCREEN_BANNER_SHOWN=1
    echo
    echo "  calcScreen dock shell"
    if [ "$(findmnt -no FSTYPE / 2>/dev/null)" = overlay ]; then
        echo "  * The root filesystem is a READ-ONLY OVERLAY: changes are lost at reboot."
        echo "    For permanent changes run 'sudo dock-unlock', then reboot."
        echo "    Afterwards 'sudo dock-lock' + reboot protects the SD card again."
    else
        echo "  * The root filesystem is UNLOCKED (writable)."
        echo "    Run 'sudo dock-lock' and reboot when you are done."
    fi
    if [ "$(id -un)" = dock ]; then
        echo "  * Type 'exit' to return to the dock."
    fi
    echo
fi
EOF
    if [[ -s /etc/motd ]]; then
        backup_once /etc/motd
        printf '' | write_file /etc/motd 0644
    fi
    if [[ -x /etc/update-motd.d/10-uname ]]; then
        run chmod -x /etc/update-motd.d/10-uname
        info "silenced /etc/update-motd.d/10-uname"
    fi
}

# Creates the dockdata partition in unallocated space at the end of the boot
# disk. Never moves or shrinks an existing partition.
make_data_partition() {
    step "Creating the '$DATA_LABEL' partition"
    if blkid -L "$DATA_LABEL" >/dev/null 2>&1; then
        ok "a filesystem labelled $DATA_LABEL already exists: $(blkid -L "$DATA_LABEL")"
        return 0
    fi
    local disk dev_sectors last_end free_mib nparts label before
    disk=$(boot_disk)
    [[ -b $disk ]] || die "cannot find the disk holding $BOOT"
    dev_sectors=$(blockdev --getsz "$disk")   # 512-byte sectors
    last_end=$(sfdisk -d "$disk" | awk -F'[=,]' '/start=/ { s = $2 + 0; z = $4 + 0;
                    if (s + z > m) m = s + z } END { print m + 0 }')
    nparts=$(sfdisk -d "$disk" | grep -c 'start=' || true)
    label=$(sfdisk -d "$disk" | awk '/^label:/ { print $2 }')
    free_mib=$(( (dev_sectors - last_end) * 512 / 1048576 ))
    info "disk $disk: $free_mib MiB unallocated after the last partition"
    if ((free_mib < DATA_MIN_MIB + 4)) || { [[ $label == dos ]] && ((nparts >= 4)); }; then
        print_partition_howto "$disk"
        die "no room for a $DATA_MIN_MIB MiB partition on $disk (nothing was changed)"
    fi
    info "creating partition $((nparts + 1)) in the free space (no existing partition is touched)"
    if ((DRY_RUN)); then
        info "[dry-run] echo ',,L' | sfdisk --append --no-reread $disk; partx -a; mkfs.ext4 -L $DATA_LABEL"
        return 0
    fi
    before=$(lsblk -lnpo NAME,TYPE "$disk" | awk '$2 == "part" { print $1 }')
    echo ',,L' | sfdisk --append --no-reread --quiet "$disk"
    # the disk is in use, so tell the kernel about the new partition only
    partx -a "$disk" 2>/dev/null || true
    udevadm settle
    local newpart
    newpart=$(lsblk -lnpo NAME,TYPE "$disk" | awk '$2 == "part" { print $1 }' | grep -vxF -- "$before" | tail -n 1 || true)
    [[ -n $newpart && -b $newpart ]] || die "the new partition did not show up; reboot and run with --make-data-partition again"
    mkfs.ext4 -F -q -L "$DATA_LABEL" -m 0 "$newpart"
    udevadm settle
    ok "created $newpart ($DATA_LABEL)"
}

print_partition_howto() {
    local disk=${1:-/dev/mmcblk0}
    cat <<EOF

  To keep settings and Wi-Fi networks across reboots, the SD card needs a small
  extra partition labelled '$DATA_LABEL' (64 MiB is plenty). The Pi cannot shrink
  its own mounted root partition, so do this ONCE with the card in a Linux PC
  (after the Pi has booted at least once). Replace /dev/sdX with the card
  (check with 'lsblk'; for a built-in reader it is /dev/mmcblk0 and the
  partitions are /dev/mmcblk0p2 and /dev/mmcblk0p3):

    DEV=/dev/sdX; P2=\${DEV}2; P3=\${DEV}3
    sudo umount \${DEV}?* 2>/dev/null
    sudo e2fsck -f \$P2
    NEW=\$(( \$(lsblk -bdno SIZE \$P2) / 1048576 - 72 ))     # MiB, leaves 72 MiB
    sudo resize2fs \$P2 \$(( NEW - 64 ))M                     # shrink fs below target
    echo ",\${NEW}M" | sudo sfdisk --no-reread -N 2 \$DEV      # shrink partition 2
    echo ",,L" | sudo sfdisk --no-reread --append \$DEV       # partition 3 = the rest
    sudo partprobe \$DEV; sleep 1
    sudo resize2fs \$P2                                        # grow fs to fill p2 again
    sudo mkfs.ext4 -L $DATA_LABEL -m 0 \$P3

  Then boot the Pi and run this installer again. (GParted works too: shrink
  partition 2 by 64 MiB from the end, create an ext4 partition labelled
  '$DATA_LABEL' in the free space.)
  Detected boot disk on this Pi: $disk

EOF
}

setup_data_store() {
    step "Persistent settings store ($STATE_DIR)"
    local dev fstype opts
    dev=$(blkid -L "$DATA_LABEL" 2>/dev/null || true)

    if ! mountpoint -q "$STATE_DIR"; then
        run mkdir -p "$STATE_DIR"
        # Marker in the directory UNDER the mount point: dockd sees it only
        # when the data partition is not mounted, i.e. settings will be lost.
        write_file "$STATE_DIR/VOLATILE" 0644 <<'EOF'
This directory is on the read-only overlay (RAM). Anything written here is
lost at reboot. Create a partition labelled 'dockdata' (see pi/README.md)
and re-run pi/install.sh to keep settings and Wi-Fi networks.
EOF
    fi

    if [[ -z $dev ]]; then
        remove_path "$UNIT_DIR/$DATA_MOUNT_UNIT" "$UNIT_DIR/$NM_CONN_UNIT" "$UNIT_DIR/$NM_STATE_UNIT" \
            "$UNIT_DIR/$DATA_DEV_UNIT.d"
        echo
        warn "*****************************************************************"
        warn "No partition labelled '$DATA_LABEL' was found."
        warn "Settings (admin password, join code, audio output) and Wi-Fi"
        warn "networks will be LOST AT EVERY REBOOT once the overlay is on."
        warn "*****************************************************************"
        print_partition_howto "$(boot_disk)" >&2
        return 0
    fi

    fstype=$(blkid -o value -s TYPE "$dev")
    case $fstype in
        ext4) opts="rw,sync,dirsync,noatime,nodev,nosuid,noexec,data=journal,nofail" ;;
        vfat) opts="rw,sync,flush,noatime,nodev,nosuid,noexec,umask=0077,nofail" ;;
        *) die "$dev ($DATA_LABEL) is '$fstype'; reformat it: sudo mkfs.ext4 -L $DATA_LABEL $dev" ;;
    esac
    info "found $dev ($fstype)"

    # A native mount unit, NOT an fstab line: overlayroot rewrites fstab and
    # (recurse=1, its default) would put every fstab mount under a RAM overlay
    # too, silently discarding all writes. Unit files are left alone.
    write_file "$UNIT_DIR/$DATA_MOUNT_UNIT" 0644 <<EOF
# calcScreen settings partition. Installed by pi/install.sh.
[Unit]
Description=calcScreen persistent settings (LABEL=$DATA_LABEL)
Requires=$DATA_FSCK_UNIT
After=$DATA_FSCK_UNIT
Before=dockd.service

[Mount]
What=/dev/disk/by-label/$DATA_LABEL
Where=$STATE_DIR
Type=$fstype
# sync+dirsync: every write is on the card before it returns; data=journal:
# file data goes through the ext4 journal too. dockd additionally writes its
# settings as two CRC-checked A/B slots, so a torn write never loses both.
Options=$opts
DirectoryMode=0755

[Install]
WantedBy=local-fs.target
EOF
    # Do not wait 90 s at boot if the partition ever disappears.
    write_file "$UNIT_DIR/$DATA_DEV_UNIT.d/calcscreen.conf" 0644 <<'EOF'
[Unit]
JobRunningTimeoutSec=10s
EOF
    write_file "$UNIT_DIR/$NM_CONN_UNIT" 0644 <<EOF
# Wi-Fi/network connections live on the settings partition so they survive
# the read-only overlay. Installed by pi/install.sh.
[Unit]
Description=calcScreen persistent NetworkManager connections
Requires=$DATA_MOUNT_UNIT
After=$DATA_MOUNT_UNIT
Before=NetworkManager.service
ConditionPathIsDirectory=$STATE_DIR/nm-connections

[Mount]
What=$STATE_DIR/nm-connections
Where=/etc/NetworkManager/system-connections
Type=none
Options=bind

[Install]
WantedBy=NetworkManager.service
EOF
    write_file "$UNIT_DIR/$NM_STATE_UNIT" 0644 <<EOF
# NetworkManager state (Wi-Fi radio on/off, leases) on the settings partition.
[Unit]
Description=calcScreen persistent NetworkManager state
Requires=$DATA_MOUNT_UNIT
After=$DATA_MOUNT_UNIT
Before=NetworkManager.service
ConditionPathIsDirectory=$STATE_DIR/nm-state

[Mount]
What=$STATE_DIR/nm-state
Where=/var/lib/NetworkManager
Type=none
Options=bind

[Install]
WantedBy=NetworkManager.service
EOF
    run systemctl daemon-reload

    if ((DRY_RUN)); then
        info "[dry-run] would mount $dev on $STATE_DIR and seed nm-connections / nm-state"
    else
        if ! mountpoint -q "$STATE_DIR"; then
            # carry over anything dockd already wrote to the plain directory
            local f has_files=0
            for f in "$STATE_DIR"/* "$STATE_DIR"/.[!.]*; do
                if [[ -e $f && ${f##*/} != VOLATILE ]]; then has_files=1; fi
            done
            if ((has_files)); then
                local tmpm
                tmpm=$(mktemp -d)
                mount "$dev" "$tmpm"
                cp -an "$STATE_DIR/." "$tmpm/"
                rm -f "$tmpm/VOLATILE"
                sync
                umount "$tmpm"
                rmdir "$tmpm"
                info "copied existing $STATE_DIR contents to $DATA_LABEL"
            fi
            systemctl start "$DATA_MOUNT_UNIT" || die "could not mount $dev on $STATE_DIR"
        fi
        mountpoint -q "$STATE_DIR" || die "$STATE_DIR is not mounted"
        rm -f "$STATE_DIR/VOLATILE"
        install -d -m 0700 "$STATE_DIR/nm-connections"
        install -d -m 0755 "$STATE_DIR/nm-state"
        # Seed once from the current system (e.g. Wi-Fi set up by Raspberry Pi Imager).
        if [[ -z $(ls -A "$STATE_DIR/nm-connections") ]] && [[ -d /etc/NetworkManager/system-connections ]]; then
            cp -a /etc/NetworkManager/system-connections/. "$STATE_DIR/nm-connections/"
            info "copied existing Wi-Fi connections to $DATA_LABEL"
        fi
        if [[ -z $(ls -A "$STATE_DIR/nm-state") ]] && [[ -d /var/lib/NetworkManager ]]; then
            cp -a /var/lib/NetworkManager/. "$STATE_DIR/nm-state/"
        fi
        sync
    fi
    run systemctl enable "$DATA_MOUNT_UNIT" "$NM_CONN_UNIT" "$NM_STATE_UNIT"
    # Working area for the USB updater (staged package + phase/result).
    run install -d -m 0755 "$STATE_DIR/update"
    ok "settings and Wi-Fi networks are stored on $dev"
}

configure_boot() {
    step "Silent boot ($BOOT)"
    edit_cmdline
    edit_config
}

configure_services() {
    step "Services"
    run systemctl daemon-reload
    run systemctl set-default multi-user.target
    # tty1 belongs to dockd: no login prompt there.
    run systemctl mask getty@tty1.service autovt@tty1.service
    run systemctl enable NetworkManager.service 2>/dev/null || warn "NetworkManager not available"
    systemctl is-enabled ssh.service >/dev/null 2>&1 ||
        warn "ssh is not enabled; enable it ('sudo systemctl enable ssh') to keep remote access"

    # no swap on the SD card: swap writes are not power-loss safe
    if command -v dphys-swapfile >/dev/null; then
        run dphys-swapfile swapoff || true
        run dphys-swapfile uninstall || true
    fi

    local u state list=()
    run mkdir -p "$CS_ETC"
    for u in "${UNNEEDED_UNITS[@]}"; do
        unit_exists "$u" || continue
        state=$(systemctl is-enabled "$u" 2>/dev/null || true)
        if [[ $state == masked ]]; then
            # masked by an earlier run: keep it on our list; masked by the
            # user: not ours to unmask later
            if grep -qsx -- "$u" "$CS_ETC/masked-units"; then list+=("$u"); fi
            continue
        fi
        if run systemctl mask --now "$u"; then
            list+=("$u")
            info "masked $u (was: ${state:-unknown})"
        else
            warn "could not mask $u"
        fi
    done
    # remember what we masked so --uninstall can restore exactly that
    printf '%s\n' "${list[@]}" | write_file "$CS_ETC/masked-units" 0644
    if swapon --show=NAME,TYPE --noheadings 2>/dev/null | awk '$2 == "file"' | grep -q .; then
        warn "a swap FILE is still active (swapon --show); disable it, swap on the SD card is not power-loss safe"
    fi

    run systemctl enable dockd.service
    # Resume a staged USB update at boot (a no-op when nothing is staged).
    run systemctl enable dock-update.service
    run systemd-tmpfiles --create /etc/tmpfiles.d/calcscreen.conf
    run udevadm control --reload
    ok "dockd enabled (starts on the next boot)"
}

enable_overlay() {
    if ((NO_OVERLAY)); then
        step "Read-only overlay: skipped (--no-overlay)"
        warn "without the overlay, pulling the power can corrupt the SD card"
        return 0
    fi
    step "Read-only root overlay + read-only $BOOT"
    run "$SBIN_DIR/dock-lock"
}

do_install() {
    step "calcScreen installer$(dry_suffix)"
    preflight "$@"
    if ((MAKE_DATA)); then make_data_partition; fi
    install_packages
    build_dockd
    setup_user
    install_files
    setup_data_store
    configure_boot
    configure_services
    enable_overlay
    run sync

    step "Done"
    if ((DRY_RUN)); then
        info "dry run finished; nothing was changed"
        return 0
    fi
    info "Reboot to start the dock:  sudo reboot"
    if ! blkid -L "$DATA_LABEL" >/dev/null 2>&1; then
        warn "settings will NOT survive reboots (no '$DATA_LABEL' partition, see above)"
    fi
}

do_uninstall() {
    step "Uninstalling calcScreen$(dry_suffix)"
    preflight "$@"

    if grep -qw 'overlayroot=tmpfs' "$BOOT/cmdline.txt" 2>/dev/null; then
        step "Disabling the read-only overlay"
        run raspi-config nonint do_overlayfs 1
    fi
    if grep -E "[[:space:]]${BOOT}[[:space:]]" /etc/fstab | grep -qE 'defaults.*,ro([[:space:],]|$)'; then
        run raspi-config nonint disable_bootro || warn "could not make $BOOT writable in /etc/fstab"
    fi

    step "Services"
    run systemctl disable dockd.service 2>/dev/null || true
    run systemctl disable dock-update.service 2>/dev/null || true
    run systemctl unmask getty@tty1.service autovt@tty1.service
    local u
    if [[ -f $CS_ETC/masked-units ]]; then
        while read -r u; do
            [[ -n $u ]] || continue
            run systemctl unmask "$u" && info "unmasked $u"
        done < "$CS_ETC/masked-units"
    fi
    for u in "$DATA_MOUNT_UNIT" "$NM_CONN_UNIT" "$NM_STATE_UNIT"; do
        [[ -f $UNIT_DIR/$u ]] && run systemctl disable "$u" 2>/dev/null
    done

    step "Files"
    remove_path "$UNIT_DIR/dockd.service" "$UNIT_DIR/dock-update.service" \
        "$UNIT_DIR/$DATA_MOUNT_UNIT" "$UNIT_DIR/$NM_CONN_UNIT" \
        "$UNIT_DIR/$NM_STATE_UNIT" "$UNIT_DIR/$DATA_DEV_UNIT.d" \
        /etc/udev/rules.d/99-calcscreen.rules /etc/tmpfiles.d/calcscreen.conf \
        /etc/systemd/system.conf.d/calcscreen-watchdog.conf \
        /etc/systemd/journald.conf.d/calcscreen.conf /etc/profile.d/calcscreen.sh \
        /etc/sudoers.d/010_calcscreen-dock "$SBIN_DIR/dock-unlock" "$SBIN_DIR/dock-lock" \
        "$SBIN_DIR/dock-automount" "$SBIN_DIR/dock-update" "$BIN_DIR/dockd" \
        "$BIN_DIR/dockd.prev" "$CS_ETC"
    if [[ -f /etc/motd$BAK ]]; then
        run cp -a /etc/motd$BAK /etc/motd
        remove_path /etc/motd$BAK
    fi
    [[ -f /etc/update-motd.d/10-uname ]] && run chmod +x /etc/update-motd.d/10-uname

    step "Boot configuration"
    edit_cmdline
    edit_config
    run systemctl daemon-reload
    run udevadm control --reload

    step "Done"
    info "Kept: user '$DOCK_USER' (remove with: sudo userdel -r $DOCK_USER),"
    info "      $STATE_DIR contents and the '$DATA_LABEL' partition, /etc/default/dockd,"
    info "      backups *$BAK, installed packages."
    info "NetworkManager files stay on '$DATA_LABEL' until you copy them back:"
    info "      sudo cp -a $STATE_DIR/nm-connections/. /etc/NetworkManager/system-connections/  (after reboot)"
    info "Reboot to get the normal console back:  sudo reboot"
}

main() {
    parse_args "$@"
    case $MODE in
        unlock)
            [[ -x $SCRIPT_DIR/dock-unlock ]] || die "$SCRIPT_DIR/dock-unlock missing"
            if ((DRY_RUN)); then run "$SCRIPT_DIR/dock-unlock" --reboot; exit 0; fi
            exec "$SCRIPT_DIR/dock-unlock" --reboot
            ;;
        uninstall) do_uninstall "$@" ;;
        install) do_install "$@" ;;
    esac
}

# Sourcing with CALCSCREEN_LIB_ONLY=1 only defines the functions (tests).
if [[ ${CALCSCREEN_LIB_ONLY:-0} != 1 ]]; then
    main "$@"
fi
