# USB updates

The dock updates itself from a USB stick by **rebuilding dockd on-device** from
a source bundle. There is no ARM64 cross-compiler in the build and the Pi
already has the toolchain, so the update ships source, not a binary.

## Package format

`tools/make_dist.sh` produces two files under `dist/` (and a copy belongs on the
root of a FAT32 stick):

* `calcscreen-update.tar.gz` — a deterministic source bundle (`core/ host/ pi/
  tools/ calc/ Makefile VERSION docs/`, plus the prebuilt `*.8xp`), excluding
  `.git/`, `build/`, `obj/` and `dist/` so the tarball never contains itself.
* `calcscreen-update.manifest` — simple `key=value` lines:

  ```
  format=1
  product=calcscreen
  version=0.2.0
  date=2026-10-13T02:25:24Z
  file=calcscreen-update.tar.gz
  size=215287
  sha256=<64 hex>
  min_format=1
  ```

The host parser (`host/dockd/update.c`) requires `format=1`, a `version`, a
`file` equal to `calcscreen-update.tar.gz`, a non-zero `size` and a 64-hex
`sha256`; `min_format` must be `<= 1`. Unknown keys are ignored.

## Trust model

**Integrity only.** The dock verifies the tarball's SHA-256 against the manifest
(and the byte size). It does **not** verify a signature: an update is already
gated by the **admin password** and **physical access** to the dock, which is
the security boundary for every other settings-page action. A corrupt or
truncated file is rejected before anything is applied.

## Flow (dockd side)

`host/dockd/update.c` scans the `/media/dock/<label>` mounts for the pair,
parses + verifies it, and shows a confirm screen (found version vs current
`DOCKD_VERSION`, with a "same/older — reinstall?" note). On confirm, dockd spawns
the privileged applier `/usr/local/sbin/dock-update stage ...` as a child and
streams its stdout in the poll loop (`PROGRESS`, `STAGED-REBOOT`, `RESULT`
markers), never blocking. After the update reboots the dock, dockd reads
`<state>/update/result` once and shows an "Updated to vX" / "Update failed"
banner.

## Phase state machine (`pi/dock-update`)

The root filesystem is a read-only RAM overlay, so installing a new binary needs
the overlay briefly disabled, which costs reboots. The applier is **resumable**
across them via `<state>/update/phase` (default state dir `/var/lib/dockd`, on
the writable `dockdata` partition; each phase write is `sync`-flushed for
power-safety).

```
stage  --pkg T --manifest M --state DIR
  verify sha256 -> copy into DIR/update/pending/ -> phase=staged (sync)
  overlay active?  yes -> print STAGED-REBOOT, exit (boot service resumes)
                   no  -> apply now (root already writable)

resume --state DIR            (run at boot by dock-update.service)
  phase staged  + overlay active -> dock-unlock, phase=unlocked, reboot
  phase unlocked/building        -> build + install, dock-lock, phase=done,
                                    write result, clear pending, reboot
  phase done/failed or no phase  -> no-op (exit 0)

overlay-status                 -> "active" | "inactive"
```

So an overlay-on dock reboots up to three times (stage, unlock, apply+re-lock),
each showing progress on `tty1`; an overlay-off dock applies in one pass with
the bar live in the dock UI.

**overlay-active detection:** the root filesystem is an `overlay` mount, or
`/proc/cmdline` contains `overlayroot=tmpfs` (the same test `pi/install.sh`
uses). "Active" means changes are volatile (land in RAM), which is exactly when
a reboot-with-unlock is required. A test hook `DOCK_OVERLAY_FORCE` overrides it.

**The build + progress bar:** the tarball is extracted to `<state>/update/work`,
then `make -C host` runs in the background while a poll loop counts `*.o` files
under the build dir against the number of `*.c` translation units in the bundle,
mapping that to 10..85 % (0..10 % is verify+extract, 85..100 % is
install+scripts+re-lock). Each step prints `PROGRESS <pct> <label>` (consumed by
dockd) and draws an in-place text bar on the console (the monitor's `tty1` at
boot). The poll loop is bounded by `DOCK_BUILD_TIMEOUT_S` (default 1800 s) so it
never hangs.

## Recovery

* Every state write is `sync`-flushed and the phase file is updated atomically,
  so a power cut mid-update is safe: re-running `dock-update resume` continues
  from the recorded phase.
* The previous binary is kept as `/usr/local/bin/dockd.prev`; on any
  build/install failure the old `dockd` is restored, the overlay is re-locked if
  it had been unlocked, `phase=failed` and `result=fail ...` are written, and
  the dock reboots into the working version with a failure banner.

## Testing

* `tests/test_update.c` — manifest parse, SHA-256 verify (good / corrupt /
  truncated / size-mismatch) and version compare (host unit tests).
* `pi/test_update.sh` — the full phase machine with stubbed make/unlock/lock/
  reboot and a fake root, including power-loss resume (`phase=building` restart)
  and `--dry-run`.
