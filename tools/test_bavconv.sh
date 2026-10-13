#!/usr/bin/env bash
# test_bavconv.sh - byte-exact tests for bavconv.
#
# usage: tools/test_bavconv.sh [path/to/bavconv]
# Without an argument it builds bavconv with tools/Makefile first.
# Needs python3 (only for generating frames and checking the result).
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ $# -ge 1 ]]; then
    BAVCONV=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
else
    make -s -C "$here" bavconv
    BAVCONV=$here/bavconv
fi
command -v python3 >/dev/null || { echo "test_bavconv: python3 is required" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail=0
pass=0
ok()  { pass=$((pass + 1)); echo "ok   - $*"; }
bad() { fail=$((fail + 1)); echo "FAIL - $*"; }

# Python helper: generate frames / verify output. Pure stdlib.
cat > "$work/h.py" <<'PY'
import struct, sys

BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]

def frame(kind, w, h):
    """Synthetic 8-bit gray frames with known content."""
    b = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            if kind == 0:    # black with a few white marker pixels
                v = 255 if (x, y) in ((0, 0), (1, 0), (7, 0), (8, 0), (w - 1, 0),
                                       (3, 1), (0, h - 1), (w - 1, h - 1)) else 0
            elif kind == 1:  # 1-pixel checkerboard
                v = 255 if (x + y) & 1 else 0
            elif kind == 2:  # horizontal gradient (tests threshold edges)
                v = (x * 255) // (w - 1)
            else:            # vertical stripes 3 px wide
                v = 200 if (x // 3) & 1 else 50
            b[y * w + x] = v
    return bytes(b)

def pack(gray, w, h, t=128, dither=False, invert=False):
    out = bytearray(w * h // 8)
    for y in range(h):
        for x in range(w):
            p = y * w + x
            if dither:
                tt = BAYER8[y & 7][x & 7] * 4 + 2 + (t - 128)
                tt = min(255, max(1, tt))
            else:
                tt = t
            white = gray[p] >= tt
            if invert:
                white = not white
            if white:
                out[p >> 3] |= 1 << (p & 7)
    return bytes(out)

def main():
    cmd = sys.argv[1]
    if cmd == "gen":                       # gen W H KIND...
        w, h = int(sys.argv[2]), int(sys.argv[3])
        sys.stdout.buffer.write(b"".join(frame(int(k), w, h) for k in sys.argv[4:]))
    elif cmd == "check":                   # check FILE W H FPS_MILLI T DITHER INVERT KIND...
        path = sys.argv[2]
        w, h, fpsm, t, d, inv = (int(a) for a in sys.argv[3:9])
        kinds = [int(k) for k in sys.argv[9:]]
        data = open(path, "rb").read()
        errs = []
        magic, ww, hh, bpp, flags, res, fps, n, fb, off, res2 = struct.unpack_from(
            "<4sHHBBHIIIII", data, 0)
        exp = dict(magic=b"BAV1", width=w, height=h, bpp=1, flags=1, reserved=0,
                   fps_milli=fpsm, frame_count=len(kinds), frame_bytes=w * h // 8,
                   data_offset=32, reserved2=0)
        got = dict(magic=magic, width=ww, height=hh, bpp=bpp, flags=flags, reserved=res,
                   fps_milli=fps, frame_count=n, frame_bytes=fb, data_offset=off,
                   reserved2=res2)
        for k in exp:
            if exp[k] != got[k]:
                errs.append("header %s: expected %r got %r" % (k, exp[k], got[k]))
        if len(data) != 32 + len(kinds) * (w * h // 8):
            errs.append("file size %d, expected %d" % (len(data), 32 + len(kinds) * (w * h // 8)))
        for i, k in enumerate(kinds):
            want = pack(frame(k, w, h), w, h, t, bool(d), bool(inv))
            fbytes = data[32 + i * len(want): 32 + (i + 1) * len(want)]
            if fbytes != want:
                pos = next((j for j in range(len(want)) if j >= len(fbytes) or fbytes[j] != want[j]), -1)
                errs.append("frame %d differs at byte %d" % (i, pos))
        if errs:
            print("\n".join(errs))
            sys.exit(1)
    elif cmd == "spot":                    # hand-computed bytes of frame kind 0, 320x240
        data = open(sys.argv[2], "rb").read()
        f = data[32:32 + 9600]
        # row 0: x=0,1,7 -> byte0 = 0b10000011 = 0x83; x=8 -> byte1 = 0x01;
        # x=319 -> byte 39 bit 7 = 0x80. row 1: x=3 -> byte 40 = 0x08.
        # row 239: x=0 -> byte 9560 = 0x01, x=319 -> byte 9599 = 0x80.
        want = {0: 0x83, 1: 0x01, 2: 0x00, 39: 0x80, 40: 0x08, 41: 0x00,
                9560: 0x01, 9599: 0x80, 4800: 0x00}
        errs = ["byte %d: expected 0x%02x got 0x%02x" % (k, v, f[k])
                for k, v in sorted(want.items()) if f[k] != v]
        if sum(bin(b).count("1") for b in f) != 8:
            errs.append("expected exactly 8 white pixels")
        if errs:
            print("\n".join(errs))
            sys.exit(1)
    elif cmd == "hdr":                     # hdr FILE -> print frame_count fps_milli
        data = open(sys.argv[2], "rb").read(32)
        print(*struct.unpack_from("<II", data, 12)[::-1])

main()
PY
py() { python3 -I "$work/h.py" "$@"; }

# 1. 3 frames 320x240 from stdin (pipe), header patched at the end.
py gen 320 240 0 1 2 > "$work/in3.raw"
if py gen 320 240 0 1 2 | "$BAVCONV" -q -f 30 -o "$work/a.bav" &&
   py check "$work/a.bav" 320 240 30000 128 0 0 0 1 2; then
    ok "3 frames via stdin pipe, header + packing"
else bad "3 frames via stdin pipe"; fi

# 2. Hand-computed bytes (independent of the python packer).
if py spot "$work/a.bav"; then ok "hand-computed LSB-first bytes"; else bad "hand-computed bytes"; fi

# 3. Regular input file -> non-seekable stdout (frame count from file size).
if "$BAVCONV" -q -f 29.97 "$work/in3.raw" | cat > "$work/b.bav" &&
   py check "$work/b.bav" 320 240 29970 128 0 0 0 1 2; then
    ok "file input, piped output, fps 29.97"
else bad "file input, piped output"; fi

# 4. Non-seekable output with unknown count must fail cleanly.
if cat "$work/in3.raw" | "$BAVCONV" -q 2>/dev/null | cat > /dev/null; then
    bad "pipe->pipe should be rejected"
else ok "pipe->pipe rejected"; fi

# 5. Threshold, invert, other size, rational fps.
py gen 64 16 2 3 0 2 > "$work/small.raw"
if "$BAVCONV" -q -W 64 -H 16 -t 200 -i -f 30000/1001 -o "$work/c.bav" "$work/small.raw" &&
   py check "$work/c.bav" 64 16 29970 200 0 1 2 3 0 2; then
    ok "64x16, -t 200, -i, 30000/1001"
else bad "64x16 threshold/invert"; fi

# 6. Ordered dithering.
if "$BAVCONV" -q -d -o "$work/d.bav" "$work/in3.raw" &&
   py check "$work/d.bav" 320 240 30000 128 1 0 0 1 2; then
    ok "dithering (-d)"
else bad "dithering"; fi
if "$BAVCONV" -q -d -t 100 -o "$work/d2.bav" "$work/in3.raw" &&
   py check "$work/d2.bav" 320 240 30000 100 1 0 0 1 2; then
    ok "dithering with brightness shift (-d -t 100)"
else bad "dithering -t 100"; fi

# 7. Trailing partial frame is dropped with a warning.
{ cat "$work/in3.raw"; head -c 1000 "$work/in3.raw"; } > "$work/partial.raw"
if py gen 320 240 0 1 2 | cat - <(head -c 1000 "$work/in3.raw") |
       "$BAVCONV" -f 30 -o "$work/e.bav" 2> "$work/e.err" &&
   grep -q "incomplete" "$work/e.err" &&
   py check "$work/e.bav" 320 240 30000 128 0 0 0 1 2 &&
   "$BAVCONV" -q -o "$work/e2.bav" "$work/partial.raw" &&
   py check "$work/e2.bav" 320 240 30000 128 0 0 0 1 2; then
    ok "partial trailing frame dropped (pipe and file)"
else bad "partial trailing frame"; fi

# 8. Empty input -> valid header with 0 frames.
if "$BAVCONV" -q -o "$work/z.bav" < /dev/null && [[ $(py hdr "$work/z.bav") == "0 30000" ]] &&
   [[ $(wc -c < "$work/z.bav") -eq 32 ]]; then
    ok "empty input -> 0 frames"
else bad "empty input"; fi

# 9. fps parsing edge cases and bad arguments.
for spec in "23.976:23976" "25:25000" "59.9401:59940" "59.9406:59941" "24000/1001:23976" "1:1000"; do
    in=${spec%%:*}; want=${spec##*:}
    if "$BAVCONV" -q -f "$in" -o "$work/f.bav" < /dev/null && [[ $(py hdr "$work/f.bav") == "0 $want" ]]; then
        ok "fps $in -> $want"
    else bad "fps $in -> $want (got $(py hdr "$work/f.bav"))"; fi
done
for args in "-W 100" "-W 0" "-t 256" "-f abc" "-f 0" "-f 30/0" "-f -5" "-x" "/nonexistent/in.raw"; do
    # shellcheck disable=SC2086 # intentional word splitting of $args
    if "$BAVCONV" -q $args -o "$work/x.bav" < /dev/null 2>/dev/null; then
        bad "should reject: $args"
    else ok "rejects: $args"; fi
done

echo "test_bavconv: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
