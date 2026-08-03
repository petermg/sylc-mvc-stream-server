#!/usr/bin/env bash
set -Eeuo pipefail
source_path=${1:?}
work=${SYLC_WORK_ROOT:?}
hls=${SYLC_HLS_DIR:?}
report=${SYLC_REPORT:?}
start=${SYLC_START_SECONDS:-0}
mode=${SYLC_OUTPUT_MODE:-half-sbs}
swap=${SYLC_SWAP_EYES:-0}
audio=${SYLC_AUDIO_STREAM:-0}
subtitle=${SYLC_SUBTITLE_ID:-off}
subtitle_kind=${SYLC_SUBTITLE_KIND:-none}
subtitle_stream=${SYLC_SUBTITLE_STREAM:--1}
subtitle_path=${SYLC_SUBTITLE_PATH:-}
subtitle_format=${SYLC_SUBTITLE_FORMAT:-}
mkdir -p "$work" "$hls"
cat > "$report" <<EOF
============================================================
Fake Phase 6 streaming test runner
============================================================
Sample: $source_path
Requested start: $start s
Output mode: $mode
Swap eyes: $swap
Audio stream: $audio
Subtitle: $subtitle
Subtitle kind: $subtitle_kind
Subtitle stream: $subtitle_stream
Subtitle path: $subtitle_path
Subtitle format: $subtitle_format
EOF
for i in 0 1 2 3; do
  printf 'fake-segment-%s\n' "$i" > "$hls/segment-$(printf '%03d' "$i").ts.tmp"
  mv "$hls/segment-$(printf '%03d' "$i").ts.tmp" "$hls/segment-$(printf '%03d' "$i").ts"
  {
    echo '#EXTM3U'
    echo '#EXT-X-VERSION:3'
    echo '#EXT-X-TARGETDURATION:2'
    echo '#EXT-X-MEDIA-SEQUENCE:0'
    for n in $(seq 0 "$i"); do
      echo '#EXTINF:2.000000,'
      echo "segment-$(printf '%03d' "$n").ts"
    done
  } > "$hls/stream.m3u8.tmp"
  mv "$hls/stream.m3u8.tmp" "$hls/stream.m3u8"
  printf 'frame=%5d fps= 36 q=-0.0 size=N/A time=00:00:0%d.00 bitrate=N/A speed=1.50x\r' "$(( (i+1)*48 ))" "$(( (i+1)*2 ))"
  sleep 0.15
done
echo '#EXT-X-ENDLIST' >> "$hls/stream.m3u8"
case "$mode" in
  full-sbs) dimensions=3840x1080 ;;
  full-ou) dimensions=1920x2160 ;;
  anaglyph-color|anaglyph-dubois) dimensions=1920x1080 ;;
  passive-rows-left-top|passive-rows-right-top) dimensions=3840x2160 ;;
  *) dimensions=1920x1080 ;;
esac
echo "RESULT threads=0 nals=100 outputs=192 pairs=192 emitted_pairs=192 skipped_pairs=0 base_only=0 mvc_only=0 poc_mismatches=0 errors=0 retries=0 wall_s=5.0 cpu_s=4.8 avg_cpu_cores=0.96 pair_fps=38.400 realtime_x=1.600 output_bytes=1 output_sample_fnv1a64=0x1 dimensions=$dimensions"
