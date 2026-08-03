#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${SYLC_TEST_BUILD_DIR:-$ROOT/engine/phase6-streaming/build}
EXTRACT=$BUILD/sylc_truehd_m2ts_extract_test
TMP=$(mktemp -d -t sylc-truehd-XXXXXX)
trap 'rm -rf "$TMP"' EXIT

command -v ffmpeg >/dev/null
command -v ffprobe >/dev/null
[[ -x "$EXTRACT" ]] || { echo "Missing TrueHD M2TS test utility: $EXTRACT" >&2; exit 2; }

# FFmpeg's experimental encoder is used only to make deterministic test input.
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'aevalsrc=0.08*sin(2*PI*440*t)|0.08*sin(2*PI*550*t)|0.08*sin(2*PI*660*t)|0.08*sin(2*PI*770*t)|0.08*sin(2*PI*880*t)|0.08*sin(2*PI*990*t):s=48000:d=2' \
  -channel_layout '5.1(side)' -c:a truehd -strict -2 -y "$TMP/reference.thd"
ffmpeg -hide_banner -loglevel error -i "$TMP/reference.thd" -c:a copy \
  -mpegts_m2ts_mode 1 -f mpegts -y "$TMP/source.m2ts"
"$EXTRACT" "$TMP/source.m2ts" "$TMP/extracted.thd" 2>"$TMP/extract.log"
cmp -s "$TMP/reference.thd" "$TMP/extracted.thd"
ffprobe -v error -show_entries stream=codec_name,channels,channel_layout,sample_rate \
  -of json "$TMP/extracted.thd" | python3 -c 'import json,sys; s=json.load(sys.stdin)["streams"][0]; assert s["codec_name"]=="truehd"; assert int(s["sample_rate"])==48000; assert s["channels"]==6; assert s["channel_layout"]=="5.1(side)"'
ffmpeg -hide_banner -loglevel error -i "$TMP/extracted.thd" -f null -
grep -q 'status=PASS' "$TMP/extract.log"
echo 'Native TrueHD M2TS extraction/decode regression: PASS'
sha256sum "$TMP/reference.thd" "$TMP/extracted.thd"
