# Raspberry Pi setup

This guide turns a Raspberry Pi 4 and a blank microSD card into a calcScreen
dock. When it's done, the Pi:

* boots straight into `dockd`, with no login prompt and no console text,
* survives having its power pulled at any time,
* keeps your settings and Wi-Fi networks across reboots, if you add the small
  `dockdata` partition (step 3).

You need:

* a Raspberry Pi 4 and its official 5.1 V / 3 A USB-C power supply,
* a microSD card of 8 GB or more,
* an HDMI monitor with a micro-HDMI cable or adapter,
* a USB keyboard,
* the TI-84 Plus CE and its standard USB cable (USB-A to mini-B, the "unit-to-computer" cable),
* for the dockdata step, a Linux PC with an SD card reader (optional).

## 1. Wiring

```
 TI-84 Plus CE ──(mini-B ↔ USB-A cable)──► Pi USB 2.0 port (black)
 USB keyboard ───────────────────────────► other Pi USB 2.0 port (black)
 USB drive (Bad Apple files) ────────────► Pi USB 3.0 port (blue)
 Monitor ◄──(micro-HDMI)── Pi HDMI0 (the port next to the USB-C power socket)
 Speakers/headphones ◄──── Pi 3.5 mm jack, or the monitor's HDMI audio
```

* Use the cable that came with the calculator. Some cheap mini-USB cables only
  carry power and have no data wires.
* The calculator charges from the Pi while it is plugged in.
* Plug the calculator and keyboard into the black USB 2.0 ports. USB 3.0
  devices are known to interfere with 2.4 GHz Wi-Fi, so keep the blue ports for
  drives.
* Connect the monitor to **HDMI0** before powering on.
* Use the official power supply. Under-voltage is the most common cause of SD
  card corruption and random USB disconnects.

## 2. Flash the SD card

1. Install **Raspberry Pi Imager** on your PC: <https://www.raspberrypi.com/software/>.
2. In Imager, choose:
   * **Device:** Raspberry Pi 4
   * **OS:** *Raspberry Pi OS (other)* → **Raspberry Pi OS Lite (64-bit)** (Bookworm or Trixie)
   * **Storage:** your SD card
3. Click **Edit settings** (OS customisation) and set:
   * **Hostname:** for example `calcscreen`
   * **Username and password:** your admin account. Do not name it `dock`, because the
     installer creates that user.
   * **Wireless LAN:** SSID, password and your Wi-Fi country
   * **Locale:** time zone and keyboard layout
   * **Services:** enable **SSH** (password or public key)
4. Write the card, put it in the Pi and power it on.
5. Wait 1–2 minutes. On the first boot the Pi resizes its filesystem and
   reboots once.
6. Log in from your PC with `ssh <user>@calcscreen.local`. If `.local` names
   don't work on your network, use the IP address shown in your router.

## 3. Optional, recommended: the `dockdata` partition

With the read-only overlay on, everything written to the SD card lands in RAM
and is gone after a reboot. The admin password, join code, audio choice and
Wi-Fi networks have to be stored somewhere real. That place is a small ext4
partition labelled **`dockdata`**:

* It is mounted at `/var/lib/dockd` with the `sync`, `dirsync`, `noatime` and
  `data=journal` options.
* dockd writes its settings to two CRC-checked A/B slots, so a save
  interrupted by a power cut always leaves the previous settings intact.
* NetworkManager's connection files (`/etc/NetworkManager/system-connections`)
  and its state (`/var/lib/NetworkManager`) are bind-mounted from this
  partition. Wi-Fi networks you add, and whether Wi-Fi is on or off, survive
  reboots.

**Without dockdata the dock still works**, but settings and Wi-Fi changes made
after installing are lost at every reboot. The installer warns about this.
The Wi-Fi network you set in Imager keeps working, because it was saved before
the overlay was switched on.

The Pi can't shrink its own root partition while it is running from it. Do this
once, after the first boot (step 2) and before running the installer:

1. Shut the Pi down with `sudo poweroff`.
2. Put the card in a Linux PC.
3. Run the commands below.
   * Find the card with `lsblk` and replace `/dev/sdX` with it.
   * In a built-in SD reader the card is usually `/dev/mmcblk0`, and the
     partitions are `/dev/mmcblk0p2` and `/dev/mmcblk0p3`.
   * **Double-check the device name**: the wrong one erases another disk.

```sh
DEV=/dev/sdX; P2=${DEV}2; P3=${DEV}3
sudo umount ${DEV}?* 2>/dev/null
sudo e2fsck -f $P2                                       # must be clean before shrinking
NEW=$(( $(lsblk -bdno SIZE $P2) / 1048576 - 72 ))        # new size of p2 in MiB
sudo resize2fs $P2 $(( NEW - 64 ))M                      # shrink the filesystem first, with margin
echo ",${NEW}M" | sudo sfdisk --no-reread -N 2 $DEV      # shrink partition 2 (start unchanged)
echo ",,L" | sudo sfdisk --no-reread --append $DEV       # partition 3 = the freed ~72 MiB
sudo partprobe $DEV; sleep 1
sudo resize2fs $P2                                       # grow the filesystem back to fill p2
sudo mkfs.ext4 -L dockdata -m 0 $P3
sync
```

**GParted alternative:** shrink partition 2 by 64 MiB from its end, then
create an **ext4** partition labelled `dockdata` in the free space.

**Free space at the end of the card:** if there is already unallocated space,
for example because you stopped the first-boot resize, run
`sudo ./pi/install.sh --make-data-partition` on the Pi instead. It creates and
formats the partition in that free space and never moves or shrinks anything.

The installer finds the partition by its label, so the partition number and
device name don't matter.

## 4. Install

Run this on the Pi, over SSH or with a keyboard:

```sh
sudo apt update && sudo apt install -y git
git clone https://github.com/tuxisawesome/calcScreen.git
cd calcScreen
sudo ./pi/install.sh --dry-run     # optional: show what would change
sudo ./pi/install.sh               # takes a few minutes
sudo reboot
```

After the reboot the monitor shows the dock's waiting screen, and nothing else
appears on screen during boot.

Installer options:

| option | effect |
|---|---|
| `--dry-run` | print every action and diff, change nothing |
| `--no-overlay` | skip the read-only overlay (for development; not power-loss safe) |
| `--disable-bt` | also turn off the Bluetooth chip (`dtoverlay=disable-bt`) |
| `--force-hotplug` | add `hdmi_force_hotplug=1` |
| `--make-data-partition` | create `dockdata` in free space at the end of the card |
| `--unlock` | same as `sudo dock-unlock --reboot` |
| `--uninstall` | undo the system changes (keeps user `dock`, settings and packages) |

### What the installer changes

* **Packages:**
  * build dependencies: `build-essential pkg-config libdrm-dev libasound2-dev libusb-1.0-0-dev libcurl4-openssl-dev`
  * runtime tools: `kbd` (openvt), `exfatprogs`, `dosfstools`, `alsa-utils`, `overlayroot`, …
* **dockd:**
  * built with `make -C host` as the owner of the checkout,
  * installed to `/usr/local/bin/dockd`,
  * runs from `/etc/systemd/system/dockd.service`. Extra flags go in
    `/etc/default/dockd` as `DOCKD_OPTS`.
* **User `dock`:**
  * the account behind "drop to shell" in the settings page,
  * has passwordless sudo and a locked password, so the only way in is `login -f`
    started by dockd, behind the admin password.
  * `/etc/profile.d/calcscreen.sh` prints a banner. It says whether the root
    filesystem is the read-only overlay, how to `dock-unlock`, and that `exit`
    returns to the dock.
* **Silent boot:**
  * `cmdline.txt` (backed up once as `cmdline.txt.calcscreen.bak`):
    * removes `console=tty1`,
    * appends `console=tty3 quiet loglevel=0 logo.nologo vt.global_cursor_default=0 systemd.show_status=0 rd.systemd.show_status=0 rd.udev.log_level=0 udev.log_priority=0 consoleblank=0`,
    * ensures `fsck.repair=yes`.
    * It stays one line, and `root=`/`rootwait` are kept.
  * `config.txt` gets a marked block at the end with `disable_splash=1`,
    `boot_delay=0`, `dtoverlay=vc4-kms-v3d`, `dtparam=watchdog=on` and
    `dtparam=audio=on`. Overlays that are already loaded aren't added twice.
  * `getty@tty1` is masked, and the default target is `multi-user.target`.
* **Services masked**, and restored by `--uninstall`:
  * Bluetooth: `bluetooth`, `hciuart`
  * `avahi-daemon`, `triggerhappy`, `ModemManager`
  * timers: `apt-daily*`, `man-db`, `dpkg-db-backup`, `e2scrub_*`
  * `rpi-eeprom-update`, `rsyslog`
  * `dphys-swapfile`: no swap on the SD card, because swap writes aren't power-loss safe.
  * NetworkManager and ssh stay on.
* **Watchdogs:**
  * `/etc/systemd/system.conf.d/calcscreen-watchdog.conf` sets
    `RuntimeWatchdogSec=15`, so the hardware watchdog reboots a hung system.
  * `dockd.service` has `WatchdogSec=10`, so a hung dockd is restarted.
* **Logs:** the journal is kept in RAM only (`Storage=volatile`, max 16 MB).
* **USB drives:**
  * `/etc/udev/rules.d/99-calcscreen.rules` and `/usr/local/sbin/dock-automount`
    mount every USB filesystem at `/media/dock/<LABEL>`.
    * Supported filesystems: FAT, exFAT, ext2/3/4 and NTFS (through the kernel's `ntfs3`).
    * Mount options: `sync,noatime`, plus `flush` for FAT, after an automatic `fsck -a`.
    * The label is reduced to `[A-Za-z0-9_-]`, 16 characters at most. If the
      label is empty or already taken, the kernel name is used, for example `sda1`.
  * The calculator (`0451:e008`, and `1209:0001` for the DOCK driver) is
    excluded from USB autosuspend and from ModemManager.
* **Read-only filesystem:** finally, `dock-lock` runs
  `raspi-config nonint do_overlayfs 0`. This puts the root filesystem on an
  `overlayroot=tmpfs` overlay and mounts `/boot/firmware` read-only.

## 5. Audio

dockd plays sound through ALSA. Pick the output in the dock's settings page
(**HDMI** or **3.5 mm**):

* **HDMI:** the monitor's speakers. ALSA card `vc4hdmi0` (HDMI0).
* **3.5 mm:** the Pi's headphone jack. ALSA card `Headphones`; needs
  `dtparam=audio=on`, which the installer sets. Quality is fine for Bad Apple.
* **USB sound card:** use `DOCKD_OPTS="--audio hw:CARD=<name>"` in
  `/etc/default/dockd`. Find names with `aplay -l`.

To test from the dock shell or SSH:

```sh
speaker-test -D hw:CARD=Headphones -c 2 -t wav -l 1
```

## 6. Preparing Bad Apple

Run this on a PC, not on the Pi. See `tools/make_badapple.sh`:

```sh
make -C tools
tools/make_badapple.sh "Bad Apple.mp4" out/
```

Copy `out/badapple.bav` and `out/badapple.wav` to the **root** of a USB drive.

## 7. Updating

The root filesystem is read-only, so unlock it first:

```sh
sudo dock-unlock && sudo reboot
# after the reboot:
cd ~/calcScreen && git pull
sudo ./pi/install.sh           # rebuilds dockd, refreshes the config, locks again at the end
sudo reboot
```

For OS updates, run `sudo apt update && sudo apt full-upgrade` while the system
is unlocked, then `sudo dock-lock && sudo reboot`.

Notes:

* If you run `install.sh --no-overlay`, lock the system yourself with
  `sudo dock-lock`.
* `sudo rpi-eeprom-update -a` updates the bootloader EEPROM while the system is unlocked.

## 8. Day-to-day

* **Dock shell:**
  * Open the settings page (Ctrl+Alt+S) and choose **Drop to shell**.
  * You are user `dock`, with sudo.
  * Type `exit` to return to the dock.
  * Changes are temporary unless you `sudo dock-unlock` and reboot.
* **SSH:** `ssh <user>@<ip>`.
  * The installer turns mDNS (avahi) off, so `calcscreen.local` may stop
    resolving. Use the IP address shown by your router.
  * To turn mDNS back on, unlock and run `sudo systemctl unmask --now avahi-daemon`.
* **Logs:** `journalctl -u dockd -b` for the daemon, `journalctl -t dock-automount -b`
  for the USB drives.

## Troubleshooting

| symptom | what to check |
|---|---|
| **Black screen, no dock** | SSH in: `systemctl status dockd`, `journalctl -u dockd -b`. Is `/usr/local/bin/dockd` there? Run `sudo ./pi/install.sh` again (unlocked). |
| **No picture when the monitor is switched on after the Pi** | Plug in the monitor before powering on, or install with `--force-hotplug`. With full KMS that legacy option may not be enough. Add `video=HDMI-A-1:1280x720@60D` (pick a mode your monitor supports) to the single line in `cmdline.txt` while unlocked. |
| **Boot text or a login prompt appears** | Compare `cmdline.txt` with `pi/cmdline.txt.snippet`. `systemctl is-enabled getty@tty1` must say `masked`. |
| **Lightning-bolt icon or random resets** | Under-voltage: use the official 3 A supply and a short cable. |
| **Calculator not detected** | `lsusb` should list `0451:e008` (TI-OS) or `1209:0001` (DOCK program). If it doesn't: try another cable (data-capable) and wake the calculator. |
| **Keyboard does nothing** | dockd grabs every keyboard. `journalctl -u dockd -b` shows which were found. |
| **USB drive not visible** | `journalctl -t dock-automount -b`, `findmnt /media/dock/*`. Check the filesystem type: FAT32, exFAT, ext4 or NTFS. Some drives have no filesystem on the whole disk and no partition table; reformat them on a PC. |
| **Settings or Wi-Fi forgotten after reboot** | No `dockdata` partition (`blkid -L dockdata`), or it failed to mount: `systemctl status var-lib-dockd.mount`. If `/var/lib/dockd/VOLATILE` exists, the settings are in RAM. |
| **"Changes I made disappeared"** | That is the overlay working. Use `sudo dock-unlock`, reboot, make the change, then `sudo dock-lock` and reboot. |
| **Installer says the overlay is active** | `sudo dock-unlock && sudo reboot`, then run it again. |
| **No sound** | `aplay -l` lists the cards. Pick the right output in settings. HDMI audio needs a monitor with speakers. |

### Recovery without SSH

If the dock doesn't come up and you can't log in:

1. Put the SD card in any PC.
2. On the small FAT partition (`bootfs`), edit `cmdline.txt`:
   * delete `overlayroot=tmpfs` to turn off the overlay, and/or
   * replace `console=tty3` with `console=tty1` and delete `quiet` to see the
     boot messages again.
3. Keep it a single line, then boot.

To bring back a login prompt on the screen, run
`sudo systemctl unmask getty@tty1` while unlocked, or use
`sudo ./pi/install.sh --uninstall`.

## Manual setup (without install.sh)

`config.txt.snippet` and `cmdline.txt.snippet` describe the boot changes.
Install `dockd.service` to `/etc/systemd/system/`, `99-calcscreen.rules` to
`/etc/udev/rules.d/`, and `dock-automount`, `dock-unlock` and `dock-lock` to
`/usr/local/sbin/`. The installer is the reference for everything else.

## Why it is built this way

* **The data store is a partition, not a file:**
  * An image file on the root filesystem would sit under the overlay.
  * `/boot/firmware` is FAT and read-only by design.
  * A separate journaled ext4 partition mounted `sync` survives power cuts.
    Together with dockd's A/B slots, a power cut can't lose both copies of the
    settings.
* **The data partition uses a systemd mount unit, not fstab:**
  * `overlayroot` rewrites `/etc/fstab` at boot.
  * With its default `recurse=1`, it would put every other fstab filesystem
    under a RAM overlay too, and all settings writes would be silently thrown
    away.
  * Native `.mount` units are left alone. A missing partition delays boot by
    at most 10 s. It never drops the Pi into emergency mode, because the unit
    is `nofail` and only *wanted* by `local-fs.target`.
* **Normal priority, not real-time:**
  * dockd runs at `Nice=-10` rather than with real-time scheduling.
  * On an idle Pi 4 that is already well under a millisecond of latency.
  * A real-time process that spins because of a bug would lock out SSH,
    exactly when you need it.
* **USB drives are mounted through systemd:** udev runs its helpers in a
  private mount namespace and kills slow ones. The rule only starts a
  transient unit (`systemd-run --no-block`). That unit asks PID 1 to mount
  the drive (`systemd-mount`), so the mount is visible system-wide.
