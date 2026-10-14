# Resident fast-mirror driver (RESIDOCK) — EXPERIMENTAL

> **Status: unverified on hardware. CEmu-first.** Nothing in here has run on a
> physical calculator. The USB-from-a-hook servicing, the fixed state region,
> and especially the flash installer are research-grade. Validate every stage in
> the CEmu emulator (which can be reset freely) before you ever flash a real
> TI-84 Plus CE. See [§7 CEmu test plan](#7-cemu-test-plan) and
> [§8 risks](#8-risks-and-why-the-installer-is-not-shipped-as-code).

## 1. What problem this solves

The hybrid mirror has one gap. The **fast** USB path only runs while a
dock-aware program (DOCK, BADAPPLE) is running, because that program owns the
USB controller and polls it in a tight loop. The moment you quit to the bare
TI-OS homescreen, the fast driver is gone and the Pi falls back to the slower
DUSB/TI-Connect path (now with the typing-lag fix, so it is usable, but it is
not fast).

A **resident** driver closes that gap: it keeps the fast USB mirror and native
key injection alive on the homescreen itself, with no program running. On the
TI-84 Plus CE the only way to run code "in the background" is a **TI-OS hook**,
and — as of OS 5.8.3 — hook code may only live inside a **flash application**.
So the resident driver is a flash app, `RESIDOCK`, that installs a **GetCSC
hook**. This is exactly how Cesium runs its homescreen shortcuts.

## 2. How it works

```
 [apps] -> run RESIDOCK  ──install──►  GetCSC hook armed, USB taken over, quit to home
                                              │
 TI-OS key-scan loop (idle) ── calls ─► resident_hook ($83)         (src/hook.S)
                                              │  on every pass
                                              ▼
                                        resident_tick()              (src/servicer.c)
                                          dock_dev_poll()   service USB EP0/bulk + Pi requests
                                          dock_dev_screen_push()  diff live VRAM, stream rows
                                          dock_dev_key_pop()  stash a Pi keystroke
                                              │
                        hook returns the stashed scan code ──► TI-OS acts on the key
```

- **Code** lives in flash (stable address), so it survives after the installer
  program exits. **Mutable state** (the USB driver context, the dock engine
  context, the pending-key byte) is relocated to a fixed RAM home in the OS USB
  buffer area, which is free while our driver owns the controller (the OS USB
  stack stays disabled for the whole resident session). The portable core keeps
  all engine state in a caller-owned `dock_dev_t`, and `usb_ce.c`'s context is
  moved to the fixed address by `-DDOCK_RESIDENT_USB_CTX`, so nothing the hook
  relies on sits in reclaimable program bss. Layout is in `src/resident.h`.
- The Pi side needs **no change**: RESIDOCK speaks the same dock protocol over
  the same USB VID/PID as the DOCK app's fast path, so `dockd`'s existing FAST
  mode just keeps working — it never sees the calculator disconnect.
- **Uninstall** (run RESIDOCK again, pick remove) clears the hook and hands USB
  back to TI-OS. A **RAM clear** does the same automatically: the hook pointer
  and active flag live in RAM, so a reset leaves a completely normal calculator
  (the app stays in flash until you delete it from `[mem]`). **Nothing in the
  running driver writes flash** — only the one-time installer does.

## 3. The GetCSC hook ABI (what the code assumes)

Reconstructed from working CE projects and graded in
[`docs/TIOS_HOOKS_SPEC.md`](TIOS_HOOKS_SPEC.md). The essentials the code relies
on (`src/hook.S`):

| Fact | Value |
|------|-------|
| Hook marker | first byte `$83`; the OS executes from byte +1 |
| Install | `SetGetCSCHook` = `0x0213E0`, `HL` = pointer to the `$83` byte |
| Uninstall | `ClrGetKeyHook` = `0x0213E4` |
| Pointer slot | `getKeyHookPtr` = `0xD025DE` (GetCSC shares the GetKey slot) |
| Active flag | bit 0 of `(iy + $34)`, i.e. `0xD000B4` |
| Entry `A` | `$1A` pre-scan (fires every idle loop pass) / `$1B` post-scan |
| Entry `B` | on `$1B`, the scan code just read (`0` = none) |
| `IY` | `= osFlags` (`0xD00080`); must be preserved |
| Return | `A` = scan code (NZ) to deliver/inject a key, or `A` = 0 (Z) for none; for `$1A` return `A` unchanged (NZ) |

The hook services the dock on **every** call (both events, so the mirror ticks
while idle) and, on `$1B` with no physical key down, returns any scan code the
Pi queued.

## 4. Build

```
cd calc/resident && make          # -> bin/RESIDOCK.8ek   (needs CEDEV on PATH)
```

`APPLICATION=YES` makes the CE toolchain emit a flash app (`.8ek`) with an
embedded relocation table — the standard CE app format. The makefile passes
`-DDOCK_RESIDENT_USB_CTX=0xD13FE8`, which **must** equal `RESIDENT_USB_CTX` in
`src/resident.h`. Normal DOCK/BADAPPLE builds do not define it and are
unaffected.

## 5. Fixed RAM state region (tunable — validate first)

`src/resident.h` places all persistent state in the OS USB area starting at
`0xD13FD8`:

| Address | Contents |
|---------|----------|
| `0xD13FD8` | pending injected scan code (`u8`) |
| `0xD13FDC` | last screen-push timestamp (`u32`) |
| `0xD13FE8` | `usb_ce.c` driver context (`ctx_t`, ~1.3 KB) |
| `0xD147E8` | `dock_dev_t` engine context |

This region is the usbArea the OS reserves for its own USB stack; it is free
while our driver holds that stack disabled. **It is the single most likely thing
to need retuning** — see the spec's §6 for the candidates (`appData`,
`pixelShadow`, `saveSScreen`) and their trade-offs. If the mirror corrupts or
the calc crashes shortly after install, suspect this first.

## 6. The installer (`calc/residinst` → RESIDINS, EXPERIMENTAL / CEmu-only)

**This is now implemented** as `calc/residinst/installer.asm` (an fasmg program
that embeds `RESIDOCK.8ek` and installs it). It writes flash, so the warning at
the top of this file applies in full: **CEmu-validate the whole install → run →
uninstall cycle before running it on a physical calculator.** It assembles
clean (fasmg, 18748 bytes with the current RESIDOCK).

Getting an **unsigned** flash app into flash is the one unavoidable
flash-writing step, and it is the proven `app_create` mechanism from
MateoConLechuga, as shipped by Cesium and AsmHook2 (the latter is the modern
OS-5.8.3+ tool, installed by arTIfiCE v3). `installer.asm` is an independent
reimplementation of it (not copied), and performs the exact sequence:

1. **Locate the flash-unlock gadget by pattern-scanning the OS.** The unlock
   routine's address changes between OS versions, so `portSetup` searches the
   running OS for a known byte pattern and records the address
   (AsmHook2 `src/ports.asm` `portSetup`/`portPattern`). This is what makes it
   survive OS updates — and what makes a hard-coded version brittle and
   dangerous.
2. **`portUnlock`** — jump to that gadget, then poke the flash-controller ports
   (`out0 ($24)`, `($06)`, `($28)`, `($22)`) to disable write-protect.
3. **Find a free app slot**: walk installed app headers from `0x3B0000` via the
   OS routines at `0x21100` (find app by name) and `0x22044` (`_ArcChk`+4, next
   app header).
4. **Write the app image** with `_WriteFlash` (`0x0002E0`, corroborated in the
   CE equates) and **`portLock`** again.
5. **Apply relocations**: walk the relocation table embedded at the app's
   `table_offset`, and for each entry add the chosen flash base to the 3-byte
   pointer stored at `code_offset + entry`, rewriting it through `_WriteFlash`.

`calc/tools/app2c.py` turns `RESIDOCK.8ek` into either a C header or (`--raw`)
the bare image bytes that `installer.asm` embeds via fasmg's `file` directive.

**Build it:**

```
make -C calc/resident                       # RESIDOCK.8ek first
make residinst FASMG=/path/to/fasmg \
     INCLUDE=/path/to/ce-asm-include         # -> calc/residinst/RESIDINS.8xp
```

`INCLUDE` must hold the standard CE asm includes (`ez80.inc`, `tiformat.inc`,
`ti84pceg.inc`) shipped with the CE assembly tools; they are intentionally not
vendored. Send `RESIDINS.8xp` to the calculator and run it (via arTIfiCE) to
install RESIDOCK into `[apps]`.

**What is and isn't verified.** The relocation logic — the part most likely to
silently corrupt flash — was checked against the real image: RESIDOCK has 509
six-byte entries `{patch_offset, value}` (both relative to the code base), and
every patch site is a `0xFFFFFF` placeholder, so each relocated address is
written over erased flash exactly as the installer assumes. What is **not**
verified (needs CEmu): the pattern-scanned flash unlock on a live OS, the
free-slot search, `_WriteFlash` behavior, and that convbin's relocation values
are code-base-relative (assumed from the entry layout). A bug in the unlock or
placement still corrupts flash and forces a boot-code OS reinstall to recover —
hence CEmu first, always.

## 7. CEmu test plan

In CEmu (Debug build, with a legally-dumped OS ROM), first validate the
**installer** against the emulated flash (which you can reset freely):

0. **Install.** Run `RESIDINS` (via arTIfiCE). Confirm it reports success,
   `RESIDOCK` appears in `[apps]`, flash is re-locked afterward, and the app
   header + relocated code look right in the debugger. Re-run it and confirm it
   reports "already installed". Snapshot flash before each attempt so a bad
   write is recoverable.

Then validate the resident driver, in order:

1. **Hook fires.** Put a breakpoint on `resident_hook`; confirm it is hit
   repeatedly while sitting idle at the homescreen (the `$1A`/`$1B` ticks).
2. **State region is safe.** Watch `0xD13FD8…0xD150E8`; confirm TI-OS does not
   overwrite it while you use the homescreen, menus and the editor. If it does,
   retune the region (§5).
3. **USB from the hook.** With a virtual/host dock attached, confirm
   `dock_dev_poll` enumerates and the mirror updates on the Pi while no program
   runs.
4. **Hook frequency / responsiveness.** Measure how often `resident_hook` runs;
   confirm the homescreen stays responsive (if typing lags, lengthen
   `RESIDENT_PUSH_MS`).
5. **Key injection.** Have the Pi send a key; confirm it appears at the
   homescreen with no physical press.
6. **Uninstall + RAM clear.** Confirm both fully restore a normal calculator and
   hand USB back to TI-OS.
7. **Other programs.** Confirm launching another program (which the mirror can
   not service) pauses the mirror cleanly rather than crashing, and that it
   resumes at the homescreen.

## 8. Risks and why the installer is not shipped as code

- **Flash install can brick** (recoverable via boot-code OS reinstall) — §6.
- **The state region may be unsafe** on some OS versions — §5.
- **The hook may not fire often enough** for a smooth mirror; it does not fire
  at all during computation, other programs, or BASIC `getKey` loops (spec §5).
- **USB-from-a-hook concurrency** with TI-OS interrupts is unproven; the driver
  keeps the OS USB/timer-3 interrupts masked so the OS cannot fight it, but this
  is a departure from the foreground driver's "fully `di`" policy.
- **`time_ce` cleanup** cannot restore the GP timer after the installer exits
  (its saved-state statics are gone); `time_ce_ms()` still works (free-running
  counter). Minor, noted for completeness.

None of these risk the Pi or your data — the worst case is on the calculator,
and short of the flash-install step everything is RAM-clear-recoverable.
