# calcScreen

Turn a Raspberry Pi 4 or Pi 3 into a docking station for a TI-84 Plus CE. Plug the
calculator into the Pi over USB, a USB keyboard and an HDMI monitor into the
Pi, and:

* the monitor mirrors the calculator's screen,
* the keyboard drives the calculator,
* calculator programs can use the Pi's audio, USB drives and network through a
  small API.

The Pi boots straight into the dock (no login, no console text, fullscreen), is
resilient to power loss, and does no real processing of its own — it is just an
interface. The headline demo plays *Bad Apple* with sound from a USB drive, but
the player handles colour video too — convert any clip with `video-convert.html`.

See **docs/ARCHITECTURE.md** for how it works, **docs/PROTOCOL.md** for the wire
format, and **pi/README.md** for Raspberry Pi setup.

## Layout

| Path | What |
|------|------|
| `core/` | portable C99 shared by the Pi, the calculator and future ports |
| `host/` | the Pi: `dockd` daemon, Linux drivers (`hal_linux`), simulator (`hal_sim`) |
| `calc/` | calculator programs: embedded USB driver (`hal_ce`), SDK, DOCK, BADAPPLE, the experimental resident fast-mirror flash app (`resident/`) and its installer (`residinst/`) — see docs/RESIDENT.md |
| `pi/` | Raspberry Pi OS setup (installer, systemd, udev, boot config) |
| `tools/` | `bavconv` and `make_badapple.sh` (run on a PC to prepare the demo) |
| `video-convert.html` | in-browser converter: any (colour) video → `badapple.bav` + `.wav` |
| `tests/` | host unit tests and the end-to-end simulation |
| `docs/` | protocol, architecture and hardware references |

## Building

```
make test     # build core + run the unit tests (host, with ASan/UBSan)
make host     # build the Pi daemon and simulator (needs libdrm/alsa/libusb/curl)
make calc     # build the calculator programs (needs the CE C toolchain, CEDEV set)
make tools    # build bavconv
```

The portable core carries no license encumbrance; third-party references and
any vendored snippets keep their original notices.
