# calcScreen architecture

calcScreen turns a Raspberry Pi 4 into a docking station for a TI-84 Plus CE:
plug the calculator into the Pi over USB, plug a USB keyboard and an HDMI
monitor into the Pi, and the Pi mirrors the calculator's screen to the monitor
and forwards the keyboard to the calculator. Calculator programs can also use
the Pi's audio, its USB drives and (optionally) its network, through a small
API. The end-to-end test is playing "Bad Apple" with sound from a USB drive.

```
            USB full-speed (calc = device, 12 Mbit/s)
 TI-84+CE  ─────────────────────────────────►  Raspberry Pi 4 ──► HDMI + audio
            dock protocol  (FAST path)          dockd daemon   ◄── USB keyboard
            TI DUSB 0451:e008 (fallback)                       ◄── USB drives
```

## Two mirroring paths (hybrid)

The CE's USB port is **full speed only** (~1.2 MB/s), so a whole 320×240×16bpp
frame (153.6 KB) cannot be pushed faster than ~8 fps. Speed therefore comes
from sending only the rows that changed. There are two ways the Pi talks to
the calculator:

1. **DUSB fallback** — the Pi speaks TI's own link protocol (the one TI Connect
   CE uses, `core/dusb.c`) to a **stock, unmodified** calculator: it polls
   screenshots and injects key presses. This needs nothing installed on the
   calculator and mirrors the real TI-OS (home screen, graphing, every menu),
   but it is slow (~2 fps) because each frame is a full screenshot. This is the
   path that satisfies "perform all functions of the calculator".

2. **FAST path** — a *dock-aware* calculator program embeds our own USB device
   driver (`calc/hal_ce`) and the portable device engine (`core/dock_dev.c`),
   and streams only changed screen rows straight out of VRAM by DMA, plus uses
   the audio/file/USB/network API. This is fast (tens of fps for typical
   content) and is what BADAPPLE and any future dock-aware app use.

`dockd` auto-detects which is present: a `1209:0001` device → FAST, a
`0451:e008` device → DUSB, nothing → a "waiting" screen.

## Why no resident/flash driver

An earlier design had a resident driver installed into the calculator's flash
so the FAST path could mirror TI-OS itself. That requires unlocking and writing
flash, which this project deliberately does **not** do. Instead:

* TI-OS mirroring uses the DUSB fallback (works on any stock calc, no install).
* The FAST path and the Pi's APIs are available inside dock-aware programs,
  whose USB driver is compiled into the program and runs only while the program
  runs. Nothing is installed; running the program (e.g. through arTIfiCE) is
  enough, and quitting it hands the USB port back to TI-OS.

## Portability

Everything shared lives in `core/` and is strict C99 with no malloc, no global
mutable state, no floating point, no 32-bit-int assumption and explicit
little-endian serialization. A platform is a "HAL": a device platform
implements `core/dock_dev_hal.h` (the calculator does, in `calc/hal_ce`; a Linux
simulator does, in `host/hal_sim`), and a dock platform implements
`core/dock_host_hal.h` (the Pi does, in `host/hal_linux`). Porting calcScreen to
another calculator or microcontroller means writing one new HAL; the protocol,
screen codec and engines are reused unchanged.

See `docs/PROTOCOL.md` for the wire format and `README.md` for setup.
