# Dock protocol

The calculator (device) and the Pi (host) exchange framed messages over a
reliable, ordered byte stream (USB bulk EP1 IN / EP2 OUT at full speed; a Unix
socket or pty in the simulator). Implemented by `core/dock_proto.{h,c}`,
`core/dock_dev.c` (device) and `core/dock_host.c` (host).

## Frame

```
offset  size  field
0       1     0xD0            magic
1       1     0xC5            magic
2       1     type            enum dock_msg_type
3       1     seq             sequence / request id
4       3     len             payload length, little-endian (u24)
7       1     crc8            CRC-8 (poly 0x07) over bytes 0..6
8       len   payload
```

The payload carries no checksum: USB bulk is already CRC-protected, which also
lets the calculator DMA screen rows straight out of VRAM with no copy. The two
magic bytes plus the header CRC let a receiver resynchronise after a hotplug or
a partial read (`dock_rx_feed`). Multi-byte fields are little-endian
everywhere. Strings are NUL-terminated.

## Session

| # | Type | Dir | Payload |
|---|------|-----|---------|
| 0x01 | HELLO   | C→H | u8 proto, u8 flags, u32 calc_id, u16 driver_ver, char join[6] |
| 0x02 | WELCOME | H→C | u8 proto, u8 status, u32 dock_id, u16 caps |
| 0x03 | PING    | ↔   | — |
| 0x04 | PONG    | ↔   | — |
| 0x05 | BYE     | C→H | — (driver releasing the port) |
| 0x06 | LOG     | C→H | UTF-8 text (debug log) |
| 0x07 | RESP    | H→C | u8 req_seq, u8 req_type, u8 status, fields… |

`join[6]` is all `\0` when no code is set. WELCOME `status` is `DOCK_OK`,
`DOCK_E_JOIN_REQUIRED` (code needed/empty) or `DOCK_E_JOIN_BAD` (wrong code).
`caps` is a bitset: AUDIO 0x01, FS 0x02, USB 0x04, NET 0x08, KEYS 0x10.

The calculator sends HELLO on connect and retries every second until WELCOME.
Every request below is C→H and is answered by exactly one RESP with the same
`seq`; RESP begins with `{req_seq, req_type, status}` then the type-specific
fields (omitted on error unless noted). Unknown message types are ignored for
forward compatibility.

## Screen (C→H, except SCR_REFRESH)

| # | Type | Payload |
|---|------|---------|
| 0x10 | SCR_MODE    | u16 w, u16 h, u8 fmt, u8 flags |
| 0x11 | SCR_PALETTE | u8 first, u16 count, count×u16 entries (RGB1555) |
| 0x12 | SCR_ROWS    | u16 y0, u16 count, u16 stride, u8 frame_end, count×stride bytes |
| 0x13 | SCR_REFRESH | H→C: resend mode, palette and all rows |

`fmt` is `enum dock_pix_fmt`: I1/I2/I4/I8 palette-indexed, RGB1555, RGB565,
RGB444. `flags`: BGR 0x01 (red in the most-significant field), BEPO 0x02
(sub-byte pixels, first pixel in the high bits). Rows are raw LCD bytes;
the sender hashes each row and emits only changed bands, with `frame_end=1`
closing a scan pass. The host converts rows to XRGB8888 (`dock_screen.c`).

## Input (H→C)

| # | Type | Payload |
|---|------|---------|
| 0x20 | KEY    | u16 tikey, u8 scancode, u8 flags |
| 0x21 | HIDKEY | u8 hid_usage, u8 modifiers, u8 down, u8 ascii |

`tikey` is a TI GetKey code (0 = none; 2-byte keys are `0xPPXX` with prefix
0xFE/0xFC/0xFB in the high byte, as libticalcs encodes them). `scancode` is the
physical GetCSC code of the key (0 = none). KEY `flags`: ON 0x01 (press
ON/break). HIDKEY delivers raw keyboard events (with ASCII) to dock-aware apps
that want them.

## Audio (C→H requests)

| # | Type | Request fields | RESP fields |
|---|------|----------------|-------------|
| 0x30 | AUD_OPEN      | u32 rate, u8 fmt, u8 channels | — |
| 0x31 | AUD_WRITE     | raw PCM bytes | u24 accepted |
| 0x32 | AUD_PLAY_FILE | path\0 (PCM WAV on a drive) | — |
| 0x33 | AUD_POS       | — | u32 pos_ms, u8 playing |
| 0x34 | AUD_STOP      | — | — |
| 0x35 | AUD_VOLUME    | u8 percent | — |

`fmt`: U8 1, S16LE 2. `AUD_POS` reports audio actually played (used to sync
video to sound). The dock only parses the WAV header; it does no transcoding.

## Files on the dock's USB drives (C→H)

| # | Type | Request fields | RESP fields |
|---|------|----------------|-------------|
| 0x40 | FS_DRIVES  | u8 index | name\0 (or status E_END) |
| 0x41 | FS_OPEN    | u8 mode, path\0 | u8 handle |
| 0x42 | FS_READ    | u8 handle, u24 max | data (len = payload−3) |
| 0x43 | FS_WRITE   | u8 handle, data | u24 written |
| 0x44 | FS_SEEK    | u8 handle, u8 whence, u32 offset | u32 pos |
| 0x45 | FS_CLOSE   | u8 handle | — |
| 0x46 | FS_STAT    | path\0 | u32 size, u8 is_dir |
| 0x47 | FS_READDIR | u16 index, path\0 | u32 size, u8 is_dir, name\0 |
| 0x48 | FS_MKDIR   | path\0 | — |
| 0x49 | FS_REMOVE  | path\0 | — |

Paths are `DRIVE/dir/file` or `dir/file` (first drive). `mode` is a bitset:
READ 0x01, WRITE 0x02, CREATE 0x04, TRUNC 0x08, APPEND 0x10. `whence`: SET 0,
CUR 1, END 2. The host sandboxes every path to the mounted drives. FS_READ data
may be DMA'd by the calculator straight into VRAM.

## Other USB devices (C→H)

| # | Type | Request | RESP |
|---|------|---------|------|
| 0x50 | USB_LIST | u8 index | u16 vid, u16 pid, u8 class, name\0 |

## Network (C→H, reserved; needs Wi-Fi + the network API enabled in settings)

| # | Type | Request | RESP |
|---|------|---------|------|
| 0x60 | NET_STATUS     | — | u8 up, ssid\0, ip\0 |
| 0x61 | NET_HTTP_BEGIN | u8 method, u32 body_len, url\0, headers\0 | u8 handle |
| 0x62 | NET_HTTP_BODY  | u8 handle, data (PUT body chunk) | — |
| 0x63 | NET_HTTP_RESP  | u8 handle | u16 http_status, u32 content_len |
| 0x64 | NET_HTTP_READ  | u8 handle, u24 max | data (empty = EOF) |
| 0x65 | NET_HTTP_END   | u8 handle | — |

`method`: GET 1, PUT 2. Calls return `DOCK_E_NET_DISABLED` when Wi-Fi or the
network API is off.

## Status codes (RESP status, u8)

0 OK · 1 END · 2 NOT_FOUND · 3 IO · 4 INVALID · 5 NO_SPACE · 6 DENIED ·
7 NET_DISABLED · 8 BUSY · 9 UNSUPPORTED · 10 NO_DRIVE · 11 TIMEOUT (local) ·
12 DISCONNECTED (local) · 13 JOIN_REQUIRED · 14 JOIN_BAD · 15 TOO_BIG.

## USB identity

FAST device: VID 0x1209, PID 0x0001 (pid.codes test IDs — replace with a real
allocation before any public release), interface class 0xFF / subclass 0xD0 /
protocol 0xC5, bulk EP 0x81 IN and 0x02 OUT, 64-byte packets, full speed.
DUSB fallback device: VID 0x0451, PID 0xE008 (TI-84 Plus CE).
