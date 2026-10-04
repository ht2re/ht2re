#!/bin/sh
# make_video.sh - one-time conversion of the original Indeo Video 5 movies
# (original/kotr.avi, original/kotrDemo.avi) into the engine's decoder-free
# frame pack + a WAV of the audio track, written to build/media/.
#
#   tools/make_video.sh                     # both clips, 512 px wide (default)
#   tools/make_video.sh --native            # original 640 px resolution
#   tools/make_video.sh --width 384         # smaller still
#   tools/make_video.sh --fps 12 --raw      # explicit rate, no RLE
#   tools/make_video.sh --force kotr        # ignore the up-to-date check
#
# Output:  build/media/<clip>.rvid   RGB565 frames, RLE coded (docs/video.md)
#          build/media/<clip>.wav    PCM s16le audio, as shipped in the AVI
#
# Idempotent: a clip is skipped when its .rvid and .wav are both newer than the
# AVI *and* were built with the same parameters (recorded in <clip>.rvid.cmd),
# so changing --width rebuilds instead of silently keeping old geometry.
#
# Measured pack sizes on the reference machine (ffmpeg 9, RLE on, 12 fps):
#   width   kotr (1308 f)   kotrDemo (1848 f)   total    wall clock per clip
#   640     484.8 MB        1005.7 MB           1.49 GB  ~1.4 s / 2.2 s
#   512     312.5 MB         647.3 MB           0.96 GB  ~1.2 s / 1.9 s
#   384     176.6 MB         365.4 MB           0.54 GB  ~0.9 s / 1.4 s
# 512 is the default: the demo footage is noisy enough that RLE only buys 1.03x
# there, so resolution is the only real size lever.  --native keeps the exact
# source resolution (the intro is 640x480 @12fps).
set -e

cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT_DIR="$ROOT/build/media"
PACK_SRC="$ROOT/tools/rvid_pack.cpp"
PACK_BIN="$ROOT/tools/out/rvid_pack"

WIDTH=512      # 0 or --native = keep the source resolution (best fidelity)
REQFPS=0       # 0 = keep the source frame rate
RLE=1
FORCE=0
CLIPS=""

usage() {
    awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --width)  WIDTH=$2; shift 2 ;;
        --native) WIDTH=0; shift ;;
        --fps)    REQFPS=$2; shift 2 ;;
        --raw)    RLE=0; shift ;;
        --force)  FORCE=1; shift ;;
        -h|--help) usage ;;
        -*) echo "make_video.sh: unknown option '$1'" >&2; exit 2 ;;
        *) CLIPS="$CLIPS $1"; shift ;;
    esac
done
[ -n "$CLIPS" ] || CLIPS="kotr kotrDemo"

# ---- locate the tools (ffmpeg is not on PATH in every shell) ----------------
FFMPEG=${FFMPEG:-}
if [ -z "$FFMPEG" ]; then
    if command -v ffmpeg >/dev/null 2>&1; then FFMPEG=$(command -v ffmpeg)
    elif [ -x /opt/homebrew/bin/ffmpeg ]; then FFMPEG=/opt/homebrew/bin/ffmpeg
    elif [ -x /usr/local/bin/ffmpeg ]; then FFMPEG=/usr/local/bin/ffmpeg
    else
        echo "make_video.sh: ffmpeg not found; set FFMPEG=/path/to/ffmpeg" >&2
        exit 1
    fi
fi
FFPROBE=${FFPROBE:-$(dirname "$FFMPEG")/ffprobe}
[ -x "$FFPROBE" ] || { echo "make_video.sh: ffprobe not found next to $FFMPEG" >&2; exit 1; }
"$FFMPEG" -hide_banner -codecs 2>/dev/null | grep -q indeo5 || \
    echo "make_video.sh: warning: this ffmpeg does not list the indeo5 decoder" >&2

# ---- rvid_pack: compile it once, rebuild when the source changes ------------
mkdir -p "$OUT_DIR" "$ROOT/tools/out"
CXX=${CXX:-c++}
if [ ! -x "$PACK_BIN" ] || [ "$PACK_SRC" -nt "$PACK_BIN" ]; then
    echo "== compiling tools/rvid_pack.cpp"
    "$CXX" -std=c++17 -O2 -o "$PACK_BIN" "$PACK_SRC"
fi

# up-to-date test:  need <clip> <params> <avi>
need() {
    rvid="$OUT_DIR/$1.rvid"; wav="$OUT_DIR/$1.wav"; cmd="$OUT_DIR/$1.rvid.cmd"
    if [ "$FORCE" = 1 ]; then return 0; fi
    if [ ! -f "$rvid" ] || [ ! -f "$wav" ] || [ ! -f "$cmd" ]; then return 0; fi
    if [ "$3" -nt "$rvid" ] || [ "$3" -nt "$wav" ]; then return 0; fi
    if [ "$(cat "$cmd")" = "$2" ]; then return 1; fi   # up to date
    return 0
}

overall_start=$(date +%s)

for clip in $CLIPS; do
    AVI="$ROOT/original/$clip.avi"
    if [ ! -f "$AVI" ]; then
        echo "make_video.sh: $AVI is missing - skipping '$clip'" >&2
        continue
    fi

    SW=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=width  -of csv=p=0 "$AVI" | head -1)
    SH=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$AVI" | head -1)
    SRATE=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$AVI" | head -1)
    SFRAMES=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=nb_frames -of csv=p=0 "$AVI" | head -1)
    SFPS=$(awk -v r="$SRATE" 'BEGIN{ n=split(r,a,"/"); print (n==2)? a[1]/a[2] : r+0 }' | awk '{printf "%d", $1+0.5}')
    SFRAMES=$(awk -v f="$SFRAMES" 'BEGIN{ printf "%d", f+0 }')
    if [ -z "$SW" ] || [ "$SW" = 0 ] || [ -z "$SH" ] || [ "$SH" = 0 ]; then
        echo "make_video.sh: cannot probe $AVI" >&2
        continue
    fi

    W=$SW; H=$SH; FPS=$SFPS
    if [ "$WIDTH" -gt 0 ] && [ "$WIDTH" != "$SW" ]; then
        W=$WIDTH
        H=$(awk -v w="$WIDTH" -v sw="$SW" -v sh="$SH" 'BEGIN{ h=int(w*sh/sw+0.5); if (h%2) h++; print h }')
    fi
    if [ "$REQFPS" -gt 0 ]; then FPS=$REQFPS; fi

    PARAMS="v1 w=$W h=$H fps=$FPS rle=$RLE src=$clip.avi"
    if ! need "$clip" "$PARAMS" "$AVI"; then
        echo "== $clip: up to date ($OUT_DIR/$clip.rvid, $(( $(wc -c < "$OUT_DIR/$clip.rvid") / 1000000 )) MB)"
        continue
    fi

    echo "== $clip: source ${SW}x${SH} @${SFPS}fps, ${SFRAMES} frames -> pack ${W}x${H} @${FPS}fps, rle=$RLE"
    RAW="$OUT_DIR/.$clip.raw565"
    trap 'rm -f "$RAW"' EXIT INT TERM

    # native size -> no scaler at all (no resample, best fidelity)
    SCALE=""
    if [ "$W" != "$SW" ] || [ "$H" != "$SH" ]; then SCALE="-vf scale=$W:$H:flags=lanczos"; fi
    RATE=""
    FPSMODE="-fps_mode passthrough"
    if [ "$FPS" != "$SFPS" ]; then RATE="-r $FPS"; FPSMODE="-fps_mode cfr"; fi

    t0=$(date +%s)
    # 1) video -> raw RGB565 frames, top-down, no frame duplication
    #    (SCALE/RATE are deliberately unquoted: they are empty or two words)
    echo "   $FFMPEG -v error -y -i original/$clip.avi $SCALE $RATE $FPSMODE -an -sn -pix_fmt rgb565le -f rawvideo <raw>"
    # shellcheck disable=SC2086
    "$FFMPEG" -v error -y -i "$AVI" $SCALE $RATE $FPSMODE -an -sn \
        -pix_fmt rgb565le -f rawvideo "$RAW"
    raw_bytes=$(wc -c < "$RAW" | tr -d ' ')
    frame_bytes=$((W * H * 2))
    frames=$((raw_bytes / frame_bytes))

    # 2) raw RGB565 -> .rvid (RLE coded, per-frame raw fallback)
    if [ "$RLE" = 1 ]; then
        "$PACK_BIN" "$W" "$H" "$FPS" "$RAW" "$OUT_DIR/$clip.rvid"
    else
        "$PACK_BIN" --raw "$W" "$H" "$FPS" "$RAW" "$OUT_DIR/$clip.rvid"
    fi

    # 3) audio track -> .wav (as shipped: PCM s16le, so this is a remux)
    "$FFMPEG" -v error -y -i "$AVI" -vn -sn -acodec pcm_s16le -f wav "$OUT_DIR/$clip.wav"

    rm -f "$RAW"
    trap - EXIT INT TERM
    t1=$(date +%s)

    printf '%s\n' "$PARAMS" > "$OUT_DIR/$clip.rvid.cmd"
    size=$(wc -c < "$OUT_DIR/$clip.rvid" | tr -d ' ')
    awk -v n="$clip" -v s="$size" -v r="$raw_bytes" -v f="$frames" -v t="$((t1 - t0))" \
        'BEGIN{ printf "   -> %s.rvid: %.1f MB, %d frames (%.2fx smaller than raw RGB565), %ds\n", n, s/1e6, f, r/s, t }'
    [ "$frames" = "$SFRAMES" ] || echo "   note: probed $SFRAMES frames, packed $frames" >&2
done

echo "== done in $(( $(date +%s) - overall_start ))s: $OUT_DIR"
ls -l "$OUT_DIR"/*.rvid "$OUT_DIR"/*.wav 2>/dev/null | awk '{ printf "   %10.1f MB  %s\n", $5/1000000, $9 }'
