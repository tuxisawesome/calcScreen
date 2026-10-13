#!/usr/bin/env bash
# make_badapple.sh - turn a video into badapple.bav + badapple.wav for the
# calcScreen BADAPPLE demo.
#
# Run this on a PC (Linux, macOS or WSL), NOT on the Pi: the Pi only moves
# bytes. Needs ffmpeg and a C compiler (for bavconv, built automatically).
#
# usage: tools/make_badapple.sh [options] INPUT_VIDEO OUTDIR
#   -f FPS     frame rate of the .bav (default 30; the calc plays at most ~30)
#   -t LEVEL   black/white threshold 0..255 (default 128)
#   -d         ordered dithering (for videos that are not pure black/white)
#   -r RATE    audio sample rate (default 22050)
#
# Then copy OUTDIR/badapple.bav and OUTDIR/badapple.wav to the ROOT of a USB
# drive (FAT32, exFAT or ext4), plug it into the Pi and run BADAPPLE on the
# calculator.
set -euo pipefail

FPS=30
THRESH=128
DITHER=()
RATE=22050
W=320
H=240

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "make_badapple: error: $*" >&2; exit 1; }

while getopts ":f:t:dr:h" opt; do
    case $opt in
        f) FPS=$OPTARG ;;
        t) THRESH=$OPTARG ;;
        d) DITHER=(-d) ;;
        r) RATE=$OPTARG ;;
        h) usage; exit 0 ;;
        :) die "option -$OPTARG needs a value" ;;
        *) usage >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 2 ]] || { usage >&2; exit 2; }

IN=$1
OUT=$2
[[ -f $IN ]] || die "input video '$IN' not found"
[[ $FPS =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "invalid fps '$FPS'"
[[ $THRESH =~ ^[0-9]+$ ]] && ((THRESH <= 255)) || die "invalid threshold '$THRESH'"
[[ $RATE =~ ^[0-9]+$ ]] || die "invalid sample rate '$RATE'"

if [[ -r /proc/device-tree/model ]] && grep -qi raspberry /proc/device-tree/model 2>/dev/null; then
    echo "make_badapple: warning: this looks like a Raspberry Pi. Converting works, but is" >&2
    echo "               slow here; the conversion is meant to run on a PC." >&2
fi

command -v ffmpeg >/dev/null ||
    die "ffmpeg not found. Install it: Debian/Ubuntu 'sudo apt install ffmpeg', macOS 'brew install ffmpeg', Windows: use WSL"
command -v ffprobe >/dev/null || echo "make_badapple: note: ffprobe not found, skipping input checks" >&2

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BAVCONV=$here/bavconv
if [[ ! -x $BAVCONV || $here/bavconv.c -nt $BAVCONV ]]; then
    command -v "${CC:-cc}" >/dev/null || command -v gcc >/dev/null || command -v clang >/dev/null ||
        die "no C compiler found to build bavconv (install build-essential / Xcode command line tools)"
    echo "make_badapple: building bavconv..." >&2
    make -s -C "$here" bavconv || die "building bavconv failed"
fi

if command -v ffprobe >/dev/null; then
    ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 "$IN" |
        grep -q video || die "'$IN' has no video stream"
    if ! ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 "$IN" |
            grep -q audio; then
        die "'$IN' has no audio stream (BADAPPLE needs badapple.wav for its clock)"
    fi
fi

mkdir -p "$OUT"
BAV=$OUT/badapple.bav
WAV=$OUT/badapple.wav
tmp_bav=$BAV.part
trap 'rm -f "$tmp_bav"' EXIT

echo "make_badapple: video -> $BAV (${W}x${H}, 1 bpp, $FPS fps)" >&2
# scale to fit 320x240 keeping the aspect ratio, letterbox with black, gray.
vf="fps=$FPS,scale=$W:$H:force_original_aspect_ratio=decrease:flags=area"
vf+=",pad=$W:$H:(ow-iw)/2:(oh-ih)/2:color=black,format=gray"
ffmpeg -nostdin -hide_banner -loglevel error -stats -i "$IN" -an -sn -vf "$vf" \
    -f rawvideo -pix_fmt gray - |
    "$BAVCONV" -W "$W" -H "$H" -f "$FPS" -t "$THRESH" "${DITHER[@]}" -o "$tmp_bav"
mv -f "$tmp_bav" "$BAV"

echo "make_badapple: audio -> $WAV (PCM s16le, mono, $RATE Hz)" >&2
ffmpeg -nostdin -hide_banner -loglevel error -y -i "$IN" -vn -sn \
    -ac 1 -ar "$RATE" -c:a pcm_s16le "$WAV"

bav_size=$(wc -c < "$BAV" | tr -d ' ')
wav_size=$(wc -c < "$WAV" | tr -d ' ')
cat >&2 <<EOF

Done:
  $BAV  ($bav_size bytes)
  $WAV  ($wav_size bytes)

Next: copy BOTH files to the root folder of a USB drive (FAT32, exFAT or ext4),
eject it safely, plug it into the Pi, then run BADAPPLE on the calculator.
EOF
