#!/usr/bin/env bash
# Phase 6 streaming engine: MVC MKV or unencrypted Blu-ray 3D ISO -> edge264 ->
# selectable stereo layout/anaglyph raw-video pipeline -> VA-API H.264 + AC-3 HLS.
# Supports start-at playback by seeking to a safe MVC random-access point and
# discarding complete stereo pairs up to the requested movie-timeline position.
set -Eeuo pipefail

SOURCE_PATH=${1:?source MVC MKV or Blu-ray 3D ISO path required}
ENGINE_PROJECT=${SYLC_ENGINE_PROJECT:-/srv/sylc-mvc-stream/engine/phase6-streaming}
BUILD_DIR=${SYLC_ENGINE_BUILD_DIR:-$ENGINE_PROJECT/build}
BIN=${SYLC_ENGINE_BINARY:-$BUILD_DIR/sylc_hsbs_pipe}
ISO_BIN=${SYLC_ISO_BINARY:-$BUILD_DIR/sylc_iso_source}
SESSION_DIR=${SYLC_SESSION_DIR:-/srv/sylc-mvc-stream/state/manual-$(date +%s)}
WORK_ROOT=${SYLC_WORK_ROOT:-$SESSION_DIR/work}
HLS_DIR=${SYLC_HLS_DIR:-$SESSION_DIR/hls}
REPORT=${SYLC_REPORT:-$SESSION_DIR/diagnostic-report.txt}
THREADS=${SYLC_THREADS:-0}
SWAP_EYES=${SYLC_SWAP_EYES:-0}
AUDIO_STREAM=${SYLC_AUDIO_STREAM:-0}
ENCODER_MODE=${SYLC_ENCODER_MODE:-vaapi}
VAAPI_DEVICE=${SYLC_VAAPI_DEVICE:-/dev/dri/renderD128}
OUTPUT_MODE=${SYLC_OUTPUT_MODE:-half-sbs}
START_SECONDS=${SYLC_START_SECONDS:-0}

DEMUX_LOG=$WORK_ROOT/video-demux.log
PRODUCER_LOG=$WORK_ROOT/hsbs-producer.log
FFMPEG_LOG=$WORK_ROOT/ffmpeg-hls.log
ISO_PROBE_LOG=$WORK_ROOT/iso-probe.log
ISO_PLAN_LOG=$WORK_ROOT/iso-seek-plan.log
ISO_VIDEO_LOG=$WORK_ROOT/iso-video.log
ISO_AUDIO_LOG=$WORK_ROOT/iso-audio.log
ISO_PROBE_JSON=$WORK_ROOT/iso-probe.json
ISO_PLAN_JSON=$WORK_ROOT/iso-seek-plan.json
ISO_AUDIO_FIFO=$WORK_ROOT/iso-audio.pipe
PLAYLIST=$HLS_DIR/stream.m3u8

if [[ ${SYLC_PHASE6_STREAM_INNER:-0} != 1 ]]; then
  export SYLC_PHASE6_STREAM_INNER=1
  mkdir -p "$(dirname "$REPORT")"
  : > "$REPORT"
  set +e
  "$0" "$@" 2>&1 | tee -a "$REPORT"
  status=${PIPESTATUS[0]}
  set -e
  exit "$status"
fi

section() {
  printf '\n\n============================================================\n%s\n============================================================\n' "$1"
}

fail() {
  echo "FATAL: $*" >&2
  exit 1
}

ISO_AUDIO_PID=
cleanup_runtime() {
  local status=$?
  trap - EXIT
  if [[ -n "${ISO_AUDIO_PID:-}" ]] && kill -0 "$ISO_AUDIO_PID" 2>/dev/null; then
    kill "$ISO_AUDIO_PID" 2>/dev/null || true
    wait "$ISO_AUDIO_PID" 2>/dev/null || true
  fi
  rm -f "$ISO_AUDIO_FIFO"
  exit "$status"
}
trap cleanup_runtime EXIT

mkdir -p "$WORK_ROOT" "$HLS_DIR"
rm -f "$DEMUX_LOG" "$PRODUCER_LOG" "$FFMPEG_LOG" "$ISO_PROBE_LOG" "$ISO_PLAN_LOG" "$ISO_VIDEO_LOG" "$ISO_AUDIO_LOG" "$ISO_PROBE_JSON" "$ISO_PLAN_JSON" "$ISO_AUDIO_FIFO"
find "$HLS_DIR" -maxdepth 1 -type f \( -name '*.ts' -o -name '*.m3u8' -o -name '*.tmp' \) -delete

section "SyLC Phase 6 live MVC MKV/ISO -> selectable HLS session"
echo "Generated: $(date --iso-8601=seconds)"
echo "Host: $(hostname)"
echo "User: $(id)"
echo "Engine: $ENGINE_PROJECT"
echo "Sample: $SOURCE_PATH"
echo "Session directory: $SESSION_DIR"
echo "Work directory: $WORK_ROOT"
echo "HLS directory: $HLS_DIR"
echo "Report: $REPORT"
echo "Decoder workers: $THREADS"
echo "Swap eyes: $SWAP_EYES"
echo "Audio stream (relative): $AUDIO_STREAM"
echo "Video encoder mode: $ENCODER_MODE"
echo "Output mode: $OUTPUT_MODE"
echo "Requested start: $START_SECONDS s"
echo "Startup mode: direct streaming; no complete Annex-B temporary file; no duplicate validation pass"

section "Preflight"
for tool in ffmpeg ffprobe python3; do
  command -v "$tool" >/dev/null 2>&1 || fail "$tool is not installed"
  printf '%-10s %s\n' "$tool" "$(command -v "$tool")"
done
[[ -r "$SOURCE_PATH" ]] || fail "Source is missing or unreadable: $SOURCE_PATH"
[[ -x "$BIN" ]] || fail "Streaming decoder/compositor is missing or not executable: $BIN"
[[ "$THREADS" == 0 ]] || fail "The Phase 6 MVC compatibility build currently requires SYLC_THREADS=0"
[[ "$AUDIO_STREAM" =~ ^[0-9]+$ ]] || fail "SYLC_AUDIO_STREAM must be a non-negative relative audio index"
[[ "$SWAP_EYES" == 0 || "$SWAP_EYES" == 1 ]] || fail "SYLC_SWAP_EYES must be 0 or 1"
case "$OUTPUT_MODE" in
  half-sbs|full-sbs|half-ou|full-ou|left-eye|right-eye|anaglyph-color|anaglyph-dubois|passive-rows-left-top|passive-rows-right-top) ;;
  *) fail "SYLC_OUTPUT_MODE must be half-sbs, full-sbs, half-ou, full-ou, left-eye, right-eye, anaglyph-color, anaglyph-dubois, passive-rows-left-top, or passive-rows-right-top" ;;
esac
case "$ENCODER_MODE" in
  vaapi) [[ -r "$VAAPI_DEVICE" ]] || fail "$VAAPI_DEVICE is not readable" ;;
  software) ;;
  *) fail "SYLC_ENCODER_MODE must be vaapi or software" ;;
esac
ls -lh "$SOURCE_PATH"
[[ "$ENCODER_MODE" == vaapi ]] && { echo "VA-API device: $VAAPI_DEVICE"; ls -l "$VAAPI_DEVICE"; } || true

SOURCE_SUFFIX=${SOURCE_PATH##*.}
SOURCE_SUFFIX=${SOURCE_SUFFIX,,}
SOURCE_KIND=mkv
ISO_AUDIO_FORMAT=
ISO_PLAYLIST=
ISO_SEGMENTS=0
ISO_DECOYS_FILTERED=0
ISO_AUDIO_PROFILE=
ISO_AUDIO_DECODE_PATH=
ISO_AUDIO_TRUEHD_MAJOR_SYNC=false
ISO_AUDIO_EMBEDDED_AC3_CORE=false

if [[ "$SOURCE_SUFFIX" == iso ]]; then
  SOURCE_KIND=iso
  [[ -x "$ISO_BIN" ]] || fail "Blu-ray ISO source adapter is missing or not executable: $ISO_BIN"
  if ! "$ISO_BIN" --input "$SOURCE_PATH" --probe >"$ISO_PROBE_JSON" 2> >(tee -a "$ISO_PROBE_LOG" >&2); then
    fail "Blu-ray ISO probe failed; see $ISO_PROBE_LOG"
  fi
  if ! ISO_META=$(python3 - "$ISO_PROBE_JSON" "$AUDIO_STREAM" <<'PYISO'
import json, sys
path, audio_index_raw = sys.argv[1:]
with open(path, 'r', encoding='utf-8') as handle:
    data = json.load(handle)
audio_index = int(audio_index_raw)
tracks = data.get('audioTracks') or []
if not data.get('ok') or not data.get('hasMVC'):
    raise SystemExit('selected ISO title is not a usable MVC feature')
if audio_index < 0 or audio_index >= len(tracks):
    raise SystemExit(f'ISO audio track {audio_index} is unavailable; title exposes {len(tracks)} tracks')
track = tracks[audio_index]
if not track.get('supported') or track.get('format') not in {'dts','ac3','eac3','truehd'}:
    raise SystemExit(f"ISO audio track {audio_index} is unsupported: {track.get('profile','unknown')}")
values = [
    data.get('durationSeconds'), data.get('fps'), data.get('width'), data.get('height'),
    track.get('format'), track.get('channels') or 0, track.get('profile') or '',
    data.get('playlist') or '', data.get('segmentCount') or 0, data.get('decoysFiltered') or 0,
    track.get('decodePath') or '', bool(track.get('truehdMajorSync')),
    bool(track.get('embeddedAc3Core')),
]
for value in values:
    print(value)
PYISO
  ); then
    fail "Blu-ray ISO metadata is incomplete or unsupported"
  fi
  readarray -t ISO_FIELDS <<<"$ISO_META"
  FORMAT_DURATION=${ISO_FIELDS[0]}
  SOURCE_FPS=${ISO_FIELDS[1]}
  WIDTH=${ISO_FIELDS[2]}
  HEIGHT=${ISO_FIELDS[3]}
  ISO_AUDIO_FORMAT=${ISO_FIELDS[4]}
  AUDIO_CHANNELS=${ISO_FIELDS[5]}
  ISO_AUDIO_PROFILE=${ISO_FIELDS[6]}
  ISO_PLAYLIST=${ISO_FIELDS[7]}
  ISO_SEGMENTS=${ISO_FIELDS[8]}
  ISO_DECOYS_FILTERED=${ISO_FIELDS[9]}
  ISO_AUDIO_DECODE_PATH=${ISO_FIELDS[10]}
  ISO_AUDIO_TRUEHD_MAJOR_SYNC=${ISO_FIELDS[11]}
  ISO_AUDIO_EMBEDDED_AC3_CORE=${ISO_FIELDS[12]}
  [[ "$AUDIO_CHANNELS" =~ ^[0-9]+$ ]] || AUDIO_CHANNELS=0
  VIDEO_CODEC=h264
  STEREO_MODE=block_lr
  FPS_EXPR=$SOURCE_FPS
  VIDEO_START=0
  AUDIO_CODEC=$ISO_AUDIO_FORMAT
  AUDIO_START=0
  VIDEO_DURATION_TAG=
else
  VIDEO_CODEC=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  STEREO_MODE=$(ffprobe -v error -select_streams v:0 -show_entries stream_tags=stereo_mode -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  WIDTH=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  HEIGHT=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  FPS_EXPR=$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  VIDEO_START=$(ffprobe -v error -select_streams v:0 -show_entries stream=start_time -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  AUDIO_CODEC=$(ffprobe -v error -select_streams "a:${AUDIO_STREAM}" -show_entries stream=codec_name -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  AUDIO_CHANNELS=$(ffprobe -v error -select_streams "a:${AUDIO_STREAM}" -show_entries stream=channels -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  AUDIO_START=$(ffprobe -v error -select_streams "a:${AUDIO_STREAM}" -show_entries stream=start_time -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  FORMAT_DURATION=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  VIDEO_DURATION_TAG=$(ffprobe -v error -select_streams v:0 -show_entries stream_tags=DURATION -of default=nw=1:nk=1 "$SOURCE_PATH" | head -n1)
  [[ -n "$VIDEO_CODEC" ]] || fail "Video codec could not be resolved"
  if [[ "$VIDEO_CODEC" != h264 ]]; then
    fail "Unsupported video source: codec=$VIDEO_CODEC. Phase 6 MVC decoding requires an original H.264 MVC MKV or Blu-ray 3D ISO."
  fi
  if [[ "$STEREO_MODE" != block_lr ]]; then
    fail "Unsupported H.264 source: stereo_mode=${STEREO_MODE:-missing}. Phase 6 requires an original MakeMKV MVC track marked stereo_mode=block_lr."
  fi
  [[ "$AUDIO_CHANNELS" =~ ^[0-9]+$ ]] || fail "Audio stream a:${AUDIO_STREAM} is missing or unreadable"
  [[ -n "$AUDIO_CODEC" ]] || fail "Audio codec could not be resolved"
fi
[[ "$WIDTH" =~ ^[0-9]+$ && "$HEIGHT" =~ ^[0-9]+$ ]] || fail "Could not resolve source dimensions"
[[ $((WIDTH % 2)) -eq 0 && $((HEIGHT % 2)) -eq 0 ]] || fail "Source dimensions must be even"
[[ -n "$FPS_EXPR" && "$FPS_EXPR" != 0/0 ]] || FPS_EXPR=24000/1001

PRODUCER_MODE=$OUTPUT_MODE
PRODUCER_WIDTH=$WIDTH
PRODUCER_HEIGHT=$HEIGHT
VIDEO_POST_FILTER=
VIDEO_POST_METHOD=
ANAGLYPH_METHOD=
case "$OUTPUT_MODE" in
  half-sbs)
    OUTPUT_LABEL="half-SBS"
    OUTPUT_WIDTH=$WIDTH
    OUTPUT_HEIGHT=$HEIGHT
    VIDEO_LEVEL=4.1
    ;;
  full-sbs)
    OUTPUT_LABEL="full-SBS"
    OUTPUT_WIDTH=$((WIDTH * 2))
    OUTPUT_HEIGHT=$HEIGHT
    PRODUCER_WIDTH=$OUTPUT_WIDTH
    VIDEO_LEVEL=5.1
    ;;
  half-ou)
    OUTPUT_LABEL="half-OU"
    OUTPUT_WIDTH=$WIDTH
    OUTPUT_HEIGHT=$HEIGHT
    VIDEO_LEVEL=4.1
    ;;
  full-ou)
    OUTPUT_LABEL="full-OU"
    OUTPUT_WIDTH=$WIDTH
    OUTPUT_HEIGHT=$((HEIGHT * 2))
    PRODUCER_HEIGHT=$OUTPUT_HEIGHT
    VIDEO_LEVEL=5.1
    ;;
  left-eye)
    OUTPUT_LABEL="left eye only"
    OUTPUT_WIDTH=$WIDTH
    OUTPUT_HEIGHT=$HEIGHT
    VIDEO_LEVEL=4.1
    ;;
  right-eye)
    OUTPUT_LABEL="right eye only"
    OUTPUT_WIDTH=$WIDTH
    OUTPUT_HEIGHT=$HEIGHT
    VIDEO_LEVEL=4.1
    ;;
  anaglyph-color)
    OUTPUT_LABEL="anaglyph color (red/cyan)"
    OUTPUT_WIDTH=$WIDTH
    OUTPUT_HEIGHT=$HEIGHT
    PRODUCER_MODE=full-sbs
    PRODUCER_WIDTH=$((WIDTH * 2))
    VIDEO_POST_FILTER="stereo3d=sbsl:arcc"
    VIDEO_POST_METHOD="FFmpeg stereo3d red/cyan color (arcc)"
    ANAGLYPH_METHOD="$VIDEO_POST_METHOD"
    VIDEO_LEVEL=4.1
    ;;
  anaglyph-dubois)
    OUTPUT_LABEL="anaglyph Dubois (red/cyan)"
    OUTPUT_WIDTH=$WIDTH
    OUTPUT_HEIGHT=$HEIGHT
    PRODUCER_MODE=full-sbs
    PRODUCER_WIDTH=$((WIDTH * 2))
    VIDEO_POST_FILTER="stereo3d=sbsl:arcd"
    VIDEO_POST_METHOD="FFmpeg stereo3d red/cyan Dubois (arcd)"
    ANAGLYPH_METHOD="$VIDEO_POST_METHOD"
    VIDEO_LEVEL=4.1
    ;;
  passive-rows-left-top)
    OUTPUT_LABEL="passive 4K rows (left eye on top/even row)"
    OUTPUT_WIDTH=$((WIDTH * 2))
    OUTPUT_HEIGHT=$((HEIGHT * 2))
    PRODUCER_MODE=full-sbs
    PRODUCER_WIDTH=$((WIDTH * 2))
    VIDEO_POST_FILTER="stereo3d=sbsl:irl,scale=${OUTPUT_WIDTH}:${OUTPUT_HEIGHT}:flags=neighbor"
    VIDEO_POST_METHOD="FFmpeg row interleave (left top/even) followed by horizontal nearest-neighbor expansion"
    VIDEO_LEVEL=5.1
    ;;
  passive-rows-right-top)
    OUTPUT_LABEL="passive 4K rows (right eye on top/even row)"
    OUTPUT_WIDTH=$((WIDTH * 2))
    OUTPUT_HEIGHT=$((HEIGHT * 2))
    PRODUCER_MODE=full-sbs
    PRODUCER_WIDTH=$((WIDTH * 2))
    VIDEO_POST_FILTER="stereo3d=sbsl:irr,scale=${OUTPUT_WIDTH}:${OUTPUT_HEIGHT}:flags=neighbor"
    VIDEO_POST_METHOD="FFmpeg row interleave (right top/even) followed by horizontal nearest-neighbor expansion"
    VIDEO_LEVEL=5.1
    ;;
esac
[[ $((OUTPUT_WIDTH % 2)) -eq 0 && $((OUTPUT_HEIGHT % 2)) -eq 0 ]] || fail "Output dimensions must be even"
[[ $((PRODUCER_WIDTH % 2)) -eq 0 && $((PRODUCER_HEIGHT % 2)) -eq 0 ]] || fail "Producer dimensions must be even"
if [[ -n "$VIDEO_POST_FILTER" ]]; then
  ffmpeg -hide_banner -filters 2>/dev/null | grep -E '(^|[[:space:]])stereo3d[[:space:]]' >/dev/null || fail "This FFmpeg build does not provide the stereo3d filter required for the selected output mode"
fi

if ! CALC_OUTPUT=$(python3 - "$FPS_EXPR" "$VIDEO_START" "$AUDIO_START" "$FORMAT_DURATION" "$VIDEO_DURATION_TAG" "$START_SECONDS" <<'PY'
import math
import sys
fps_expr, vs, aus, format_duration_raw, video_duration_tag, requested_raw = sys.argv[1:]
def number(value, default=0.0):
    try: return float(value)
    except Exception: return default
try:
    a, b = fps_expr.split('/', 1)
    fps = float(a) / float(b)
except Exception:
    fps = float(fps_expr)
video_start = number(vs)
audio_start = number(aus)
duration = number(format_duration_raw, -1.0)
if video_duration_tag:
    try:
        hours, minutes, seconds = video_duration_tag.split(':', 2)
        tagged = float(hours) * 3600 + float(minutes) * 60 + float(seconds)
        if tagged > 0:
            duration = tagged
    except Exception:
        pass
requested = number(requested_raw, -1.0)
if not math.isfinite(requested) or requested < 0:
    raise SystemExit("SYLC_START_SECONDS must be a finite non-negative number")
if duration > 0 and requested >= duration:
    raise SystemExit(f"Requested start {requested:.3f}s is at or beyond source duration {duration:.3f}s")
delta = audio_start - video_start
print(f"{fps:.9f}")
print(f"{video_start:.6f}")
print(f"{audio_start:.6f}")
print(str(int(round(delta * 1000.0))))
print(f"{duration:.6f}")
print(f"{requested:.6f}")
PY
); then
  fail "Invalid requested start position"
fi
readarray -t CALC <<<"$CALC_OUTPUT"
SOURCE_FPS=${CALC[0]}
VIDEO_START=${CALC[1]}
AUDIO_START=${CALC[2]}
AUDIO_DELTA_MS=${CALC[3]}
SOURCE_DURATION=${CALC[4]}
START_SECONDS=${CALC[5]}

KEYFRAME_START=0.000000
SKIP_PAIRS=0
EFFECTIVE_START=$START_SECONDS
if [[ "$SOURCE_KIND" == mkv ]] && python3 - "$START_SECONDS" <<'PY'
import sys
raise SystemExit(0 if float(sys.argv[1]) > 0 else 1)
PY
then
  if ! SEEK_OUTPUT=$(python3 - "$SOURCE_PATH" "$START_SECONDS" <<'PY'
from fractions import Fraction
import subprocess
import sys

source = sys.argv[1]
target = float(sys.argv[2])
EPS = 0.0005

# Ask the same FFmpeg demuxer command used by the live pipeline which packet it
# will actually start with. Matroska input seeking can land before the requested
# timestamp even when that timestamp itself is a keyframe, so guessing from a
# separate ffprobe seek is not exact enough.
cmd = [
    "ffmpeg", "-v", "error", "-ss", f"{target:.6f}", "-copyts",
    "-i", source, "-map", "0:v:0", "-c", "copy", "-frames:v", "1",
    "-f", "framecrc", "-",
]
completed = subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=90, check=False)
if completed.returncode != 0:
    raise SystemExit(completed.stderr.strip() or "FFmpeg could not resolve the seek start")
time_base = None
packet_line = None
for line in completed.stdout.splitlines():
    stripped = line.strip()
    if stripped.startswith("#tb 0:"):
        time_base = Fraction(stripped.split(":", 1)[1].strip())
    elif stripped and not stripped.startswith("#"):
        packet_line = stripped
        break
if time_base is None or packet_line is None:
    raise SystemExit("FFmpeg did not report the first packet at the requested seek")
parts = [part.strip() for part in packet_line.split(",")]
if len(parts) < 3:
    raise SystemExit("Unexpected FFmpeg framecrc output")
first_pts = float(Fraction(int(parts[2])) * time_base)

probe_start = max(0.0, first_pts - 2.0)
probe_duration = target - probe_start + 8.0
probe_cmd = [
    "ffprobe", "-v", "error", "-select_streams", "v:0",
    "-read_intervals", f"{probe_start:.6f}%+{probe_duration:.6f}",
    "-show_packets", "-show_entries", "packet=pts_time,flags",
    "-of", "csv=p=0", source,
]
probed = subprocess.run(probe_cmd, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=90, check=False)
if probed.returncode != 0:
    raise SystemExit(probed.stderr.strip() or "ffprobe could not count seek preroll packets")
packets = []
for line in probed.stdout.splitlines():
    fields = [field.strip() for field in line.split(",")]
    try:
        pts = float(fields[0])
    except Exception:
        continue
    flags = fields[1] if len(fields) > 1 else ""
    packets.append((pts, flags))
matching = [(pts, flags) for pts, flags in packets if abs(pts - first_pts) <= 0.002]
if not matching or not any("K" in flags for _, flags in matching):
    raise SystemExit(f"Resolved FFmpeg seek start {first_pts:.6f}s is not an indexed keyframe")
skip = sum(1 for pts, _ in packets if pts >= first_pts - EPS and pts < target - EPS)
future = [pts for pts, _ in packets if pts >= target - EPS]
effective = min(future) if future else target
print(f"{first_pts:.6f}")
print(skip)
print(f"{effective:.6f}")
PY
); then
    fail "Could not resolve a safe MVC seek point"
  fi
  readarray -t SEEK_INFO <<<"$SEEK_OUTPUT"
  KEYFRAME_START=${SEEK_INFO[0]}
  SKIP_PAIRS=${SEEK_INFO[1]}
  EFFECTIVE_START=${SEEK_INFO[2]}
fi
ISO_RECOVERY_BACKOFF=0
ISO_RECOVERY_ATTEMPTS=0
ISO_RECOVERY_PREFLIGHT="not required"
ISO_TARGET_PREROLL_PAIRS=0
ISO_RECOVERY_HIDDEN_PAIRS=0
ISO_RECOVERY_STRUCTURAL_PAIRS=0
ISO_RECOVERY_STABILIZATION_PAIRS=0
ISO_RECOVERY_ANCHOR=0.000000
ISO_RECOVERY_METHOD="zero-time"
ISO_RECOVERY_CALIBRATION_SAMPLES=0
ISO_RECOVERY_CALIBRATION_INLIERS=0
ISO_RECOVERY_OFFSET_MS=0
ISO_RECOVERY_PHASE_SHIFT=0
ISO_RECOVERY_PHASE_RESIDUAL_MS=0
ISO_RECOVERY_LEADING_BASE_DISCARDS=0
ISO_RECOVERY_LEADING_DEP_DISCARDS=0

if [[ "$SOURCE_KIND" == iso ]]; then
  ISO_PLAN_SELECTED=0
  ISO_START_NONZERO=0
  if python3 - "$START_SECONDS" <<'PYISONZ'
import sys
raise SystemExit(0 if float(sys.argv[1]) > 0 else 1)
PYISONZ
  then
    ISO_START_NONZERO=1
  fi

  if [[ "$ISO_START_NONZERO" == 1 ]]; then
    # A nonzero ISO start is validated with a bounded fresh-process recovery
    # preflight. Each attempt uses a fresh SSIF demuxer and fresh edge264 decoder.
    # Attempt 1 trusts the preceding CLPI anchor; attempts 2/3 move 16/48 entries
    # farther back, matching the proven Android recovery policy.
    for ISO_RECOVERY_BACKOFF_CANDIDATE in 0 16 48; do
      ISO_RECOVERY_ATTEMPTS=$((ISO_RECOVERY_ATTEMPTS + 1))
      CANDIDATE_PLAN="$ISO_PLAN_JSON.attempt-${ISO_RECOVERY_ATTEMPTS}"
      CANDIDATE_PLAN_LOG="$ISO_PLAN_LOG.attempt-${ISO_RECOVERY_ATTEMPTS}"
      CANDIDATE_PREFLIGHT_LOG="$WORK_ROOT/iso-recovery-preflight-attempt-${ISO_RECOVERY_ATTEMPTS}.log"
      if ! "$ISO_BIN" --input "$SOURCE_PATH" --plan-video-seek \
          --start-seconds "$START_SECONDS" \
          --recovery-backoff-entries "$ISO_RECOVERY_BACKOFF_CANDIDATE" \
          >"$CANDIDATE_PLAN" 2> >(tee -a "$CANDIDATE_PLAN_LOG" >&2); then
        echo "ISO MVC recovery attempt $ISO_RECOVERY_ATTEMPTS planning failed; trying an earlier CLPI anchor." >&2
        continue
      fi
      if ! CANDIDATE_SKIP=$(python3 - "$CANDIDATE_PLAN" <<'PYISOSKIP'
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as handle:
    data=json.load(handle)
if not data.get('ok'):
    raise SystemExit(1)
value=int(data.get('skipPairs', -1))
if value < 0:
    raise SystemExit(1)
print(value)
PYISOSKIP
      ); then
        echo "ISO MVC recovery attempt $ISO_RECOVERY_ATTEMPTS produced invalid planning data." >&2
        continue
      fi

      set +e
      "$ISO_BIN" --input "$SOURCE_PATH" --video \
          --start-seconds "$START_SECONDS" \
          --recovery-backoff-entries "$ISO_RECOVERY_BACKOFF_CANDIDATE" \
          2>>"$CANDIDATE_PREFLIGHT_LOG" | \
      "$BIN" --input - --threads "$THREADS" --source-fps "$SOURCE_FPS" \
          --skip-pairs "$CANDIDATE_SKIP" --max-pairs 1 --mode half-sbs \
          --discard-output 2>>"$CANDIDATE_PREFLIGHT_LOG"
      CANDIDATE_STATUSES=("${PIPESTATUS[@]}")
      set -e
      CANDIDATE_SOURCE_STATUS=${CANDIDATE_STATUSES[0]:-99}
      CANDIDATE_DECODER_STATUS=${CANDIDATE_STATUSES[1]:-99}
      CANDIDATE_RESULT=$(grep '^RESULT ' "$CANDIDATE_PREFLIGHT_LOG" | tail -n1 || true)
      if [[ $CANDIDATE_SOURCE_STATUS -eq 0 && $CANDIDATE_DECODER_STATUS -eq 0 \
          && -n "$CANDIDATE_RESULT" ]] \
          && ! grep -Eq 'base_only=[1-9]|mvc_only=[1-9]|poc_mismatches=[1-9]|errors=[1-9]' \
              <<<"$CANDIDATE_RESULT"; then
        cp "$CANDIDATE_PLAN" "$ISO_PLAN_JSON"
        cat "$CANDIDATE_PLAN_LOG" >>"$ISO_PLAN_LOG" 2>/dev/null || true
        ISO_RECOVERY_BACKOFF=$ISO_RECOVERY_BACKOFF_CANDIDATE
        ISO_RECOVERY_PREFLIGHT="PASS"
        ISO_PLAN_SELECTED=1
        break
      fi
      echo "ISO MVC recovery attempt $ISO_RECOVERY_ATTEMPTS failed its fresh edge264 preflight; trying an earlier CLPI anchor." >&2
    done
    [[ "$ISO_PLAN_SELECTED" == 1 ]] || fail "Blu-ray ISO MVC seek recovery failed all three bounded CLPI-anchor attempts"
  else
    ISO_RECOVERY_ATTEMPTS=0
    if ! "$ISO_BIN" --input "$SOURCE_PATH" --plan-video-seek --start-seconds "$START_SECONDS" \
        --recovery-backoff-entries 0 \
        >"$ISO_PLAN_JSON" 2> >(tee -a "$ISO_PLAN_LOG" >&2); then
      fail "Blu-ray ISO MVC seek planning failed; see $ISO_PLAN_LOG"
    fi
    ISO_PLAN_SELECTED=1
  fi

  if ! ISO_SEEK_OUTPUT=$(python3 - "$ISO_PLAN_JSON" <<'PYISOSEEK'
import json
import math
import sys

with open(sys.argv[1], 'r', encoding='utf-8') as handle:
    data = json.load(handle)
if not data.get('ok'):
    raise SystemExit('ISO video seek plan did not succeed')
actual = float(data['actualDemuxStartSeconds'])
effective = float(data.get('cleanReleaseSeconds', data['firstOutputSeconds']))
skip = int(data['skipPairs'])
if not math.isfinite(actual) or actual < 0:
    raise SystemExit('ISO demux start is invalid')
if not math.isfinite(effective) or effective < 0:
    raise SystemExit('ISO clean-release timestamp is invalid')
if skip < 0:
    raise SystemExit('ISO skip-pair count is invalid')
values = [
    f"{actual:.6f}", str(skip), f"{effective:.6f}",
    data.get('firstClip') or '', data.get('seekDetail') or '',
    str(int(data.get('targetPrerollPairs', 0))),
    str(int(data.get('recoveryHiddenPairs', 0))),
    str(int(data.get('recoveryStructuralPairs', 0))),
    str(int(data.get('recoveryStabilizationPairs', 0))),
    f"{float(data.get('recoveryAnchorSeconds', 0.0)):.6f}",
    data.get('recoverySeekMethod') or '',
    str(int(data.get('calibrationSamples', 0))),
    str(int(data.get('calibrationInliers', 0))),
    str(int(data.get('calibratedDependentMinusBaseMs', 0))),
    str(int(data.get('phaseShiftFrames', 0))),
    str(int(data.get('phaseResidualMs', 0))),
    str(int(data.get('leadingBaseDiscards', 0))),
    str(int(data.get('leadingDependentDiscards', 0))),
]
for value in values:
    print(value)
PYISOSEEK
  ); then
    fail "Blu-ray ISO seek plan JSON is incomplete or invalid"
  fi
  readarray -t ISO_SEEK_FIELDS <<<"$ISO_SEEK_OUTPUT"
  KEYFRAME_START=${ISO_SEEK_FIELDS[0]}
  SKIP_PAIRS=${ISO_SEEK_FIELDS[1]}
  EFFECTIVE_START=${ISO_SEEK_FIELDS[2]}
  ISO_FIRST_CLIP=${ISO_SEEK_FIELDS[3]}
  ISO_SEEK_DETAIL=${ISO_SEEK_FIELDS[4]}
  ISO_TARGET_PREROLL_PAIRS=${ISO_SEEK_FIELDS[5]}
  ISO_RECOVERY_HIDDEN_PAIRS=${ISO_SEEK_FIELDS[6]}
  ISO_RECOVERY_STRUCTURAL_PAIRS=${ISO_SEEK_FIELDS[7]}
  ISO_RECOVERY_STABILIZATION_PAIRS=${ISO_SEEK_FIELDS[8]}
  ISO_RECOVERY_ANCHOR=${ISO_SEEK_FIELDS[9]}
  ISO_RECOVERY_METHOD=${ISO_SEEK_FIELDS[10]}
  ISO_RECOVERY_CALIBRATION_SAMPLES=${ISO_SEEK_FIELDS[11]}
  ISO_RECOVERY_CALIBRATION_INLIERS=${ISO_SEEK_FIELDS[12]}
  ISO_RECOVERY_OFFSET_MS=${ISO_SEEK_FIELDS[13]}
  ISO_RECOVERY_PHASE_SHIFT=${ISO_SEEK_FIELDS[14]}
  ISO_RECOVERY_PHASE_RESIDUAL_MS=${ISO_SEEK_FIELDS[15]}
  ISO_RECOVERY_LEADING_BASE_DISCARDS=${ISO_SEEK_FIELDS[16]}
  ISO_RECOVERY_LEADING_DEP_DISCARDS=${ISO_SEEK_FIELDS[17]}
fi
[[ "$SKIP_PAIRS" =~ ^[0-9]+$ ]] || fail "Resolved skip-pair count is invalid"
echo "Source kind: $SOURCE_KIND"
echo "Video: codec=$VIDEO_CODEC stereo_mode=$STEREO_MODE ${WIDTH}x${HEIGHT} at $FPS_EXPR ($SOURCE_FPS fps), start=$VIDEO_START s"
if [[ "$SOURCE_KIND" == iso ]]; then
  echo "Blu-ray playlist: $ISO_PLAYLIST; segments=$ISO_SEGMENTS; replay decoys filtered=$ISO_DECOYS_FILTERED"
  echo "ISO source adapter: $ISO_BIN"
  echo "ISO seek clip: ${ISO_FIRST_CLIP:-unknown}"
  echo "ISO seek detail: ${ISO_SEEK_DETAIL:-unavailable}"
  echo "MVC seek recovery enabled: $([[ "$START_SECONDS" != 0.000000 ]] && echo yes || echo no)"
  echo "MVC recovery fresh-process attempts/preflight: $ISO_RECOVERY_ATTEMPTS / $ISO_RECOVERY_PREFLIGHT"
  echo "MVC recovery selected CLPI backoff entries: $ISO_RECOVERY_BACKOFF"
  echo "MVC recovery anchor/method: $ISO_RECOVERY_ANCHOR s / $ISO_RECOVERY_METHOD"
  echo "MVC source-offset calibration samples/inliers/offset: $ISO_RECOVERY_CALIBRATION_SAMPLES / $ISO_RECOVERY_CALIBRATION_INLIERS / ${ISO_RECOVERY_OFFSET_MS} ms"
  echo "MVC eye-phase correction frames/residual: $ISO_RECOVERY_PHASE_SHIFT / ${ISO_RECOVERY_PHASE_RESIDUAL_MS} ms"
  echo "MVC leading base/dependent discards: $ISO_RECOVERY_LEADING_BASE_DISCARDS / $ISO_RECOVERY_LEADING_DEP_DISCARDS"
  echo "MVC hidden structural/stabilization/total pairs: $ISO_RECOVERY_STRUCTURAL_PAIRS / $ISO_RECOVERY_STABILIZATION_PAIRS / $ISO_RECOVERY_HIDDEN_PAIRS"
  echo "MVC target-preroll pairs: $ISO_TARGET_PREROLL_PAIRS"
fi
echo "Output: mode=$OUTPUT_MODE label=$OUTPUT_LABEL dimensions=${OUTPUT_WIDTH}x${OUTPUT_HEIGHT} swap_eyes=$SWAP_EYES"
if [[ "$PRODUCER_MODE" != "$OUTPUT_MODE" || "$PRODUCER_WIDTH" != "$OUTPUT_WIDTH" || "$PRODUCER_HEIGHT" != "$OUTPUT_HEIGHT" ]]; then
  echo "Intermediate compositor: mode=$PRODUCER_MODE dimensions=${PRODUCER_WIDTH}x${PRODUCER_HEIGHT}"
fi
[[ -n "$ANAGLYPH_METHOD" ]] && echo "Anaglyph method: $ANAGLYPH_METHOD"
echo "Audio a:${AUDIO_STREAM}: codec=$AUDIO_CODEC channels=${AUDIO_CHANNELS:-unknown} start=$AUDIO_START s profile=${ISO_AUDIO_PROFILE:-n/a}"
if [[ "$SOURCE_KIND" == iso ]]; then
  echo "ISO audio decode path: ${ISO_AUDIO_DECODE_PATH:-native compressed-audio decode}"
  if [[ "$ISO_AUDIO_PROFILE" == "Dolby TrueHD" ]]; then
    echo "TrueHD major sync detected: $ISO_AUDIO_TRUEHD_MAJOR_SYNC"
    echo "Embedded AC-3 companion detected: $ISO_AUDIO_EMBEDDED_AC3_CORE"
  fi
fi
echo "Audio-minus-video start delta: ${AUDIO_DELTA_MS} ms"
echo "Source duration: $SOURCE_DURATION s"
echo "Requested start: $START_SECONDS s"
echo "Actual MVC demux keyframe start: $KEYFRAME_START s"
echo "Stereo pairs discarded before output: $SKIP_PAIRS"
echo "Clean visible release source timestamp: $EFFECTIVE_START s"

AUDIO_CHANNELS=${AUDIO_CHANNELS:-0}
if (( AUDIO_CHANNELS == 0 || AUDIO_CHANNELS > 2 )); then AUDIO_BITRATE=640k; else AUDIO_BITRATE=192k; fi
AUDIO_CHANNEL_ARGS=()
AUDIO_LAYOUT_FILTER=""
AUDIO_OUTPUT_POLICY="source layout preserved"
if [[ "$SOURCE_KIND" == iso && "$ISO_AUDIO_FORMAT" == truehd ]]; then
  AUDIO_OUTPUT_POLICY="native TrueHD lossless decode followed by AC-3 compatibility encode"
elif [[ "$SOURCE_KIND" == iso && "$ISO_AUDIO_PROFILE" == "Dolby TrueHD" && "$ISO_AUDIO_FORMAT" == ac3 ]]; then
  AUDIO_OUTPUT_POLICY="embedded AC-3 companion fallback from Dolby TrueHD PID"
fi
if (( AUDIO_CHANNELS >= 6 )); then
  # Force a deterministic AC-3-compatible 5.1(side) layout. Named-channel pan
  # keeps FL/FR/FC and LFE intact and folds side/back surround pairs together.
  # Missing named channels are treated as silence, so this is safe for native
  # 5.1, 5.1(back), and 7.1 Blu-ray decoder layouts. The '<' operator
  # normalizes each folded pair to avoid clipping.
  AUDIO_LAYOUT_FILTER=",pan=5.1(side)|FL=FL|FR=FR|FC=FC|LFE=LFE|SL<SL+BL|SR<SR+BR"
  AUDIO_CHANNEL_ARGS=(-ac:a 6)
  if [[ "$SOURCE_KIND" == iso && "$ISO_AUDIO_FORMAT" == truehd ]]; then
    AUDIO_OUTPUT_POLICY="native TrueHD lossless decode; explicit 5.1(side) rematrix; LFE preserved; side/back surrounds folded; AC-3 640 kbps output"
  else
    AUDIO_OUTPUT_POLICY="explicit 5.1(side) rematrix; LFE preserved; side/back surrounds folded"
  fi
fi

if [[ "$SOURCE_KIND" == iso ]]; then
  AUDIO_FILTER="[1:a:0]asetpts=PTS-STARTPTS${AUDIO_LAYOUT_FILTER},apad[aout]"
else
  if (( AUDIO_DELTA_MS >= 0 )); then
    AUDIO_FILTER="[1:a:${AUDIO_STREAM}]asetpts=PTS-STARTPTS,adelay=${AUDIO_DELTA_MS}:all=1${AUDIO_LAYOUT_FILTER},apad[aout]"
  else
    AUDIO_TRIM=$(python3 - "$AUDIO_DELTA_MS" <<'PYAUDIO'
import sys
print(f"{abs(int(sys.argv[1])) / 1000.0:.6f}")
PYAUDIO
)
    AUDIO_FILTER="[1:a:${AUDIO_STREAM}]atrim=start=${AUDIO_TRIM},asetpts=PTS-STARTPTS${AUDIO_LAYOUT_FILTER},apad[aout]"
  fi
fi
echo "Audio output policy: $AUDIO_OUTPUT_POLICY"

VIDEO_ARGS=()
VIDEO_FILTER_ARGS=()
if [[ -n "$VIDEO_POST_FILTER" ]]; then
  if [[ "$ENCODER_MODE" == vaapi ]]; then
    VIDEO_FILTER_ARGS=(-vf "${VIDEO_POST_FILTER},format=nv12,hwupload")
  else
    VIDEO_FILTER_ARGS=(-vf "$VIDEO_POST_FILTER")
  fi
elif [[ "$ENCODER_MODE" == vaapi ]]; then
  VIDEO_FILTER_ARGS=(-vf format=nv12,hwupload)
fi
case "$ENCODER_MODE" in
  vaapi)
    VIDEO_ARGS=(
      -vaapi_device "$VAAPI_DEVICE"
      -c:v h264_vaapi -profile:v high -level:v "$VIDEO_LEVEL" -qp 22
      -g 48 -bf 2
    )
    ;;
  software)
    VIDEO_ARGS=(
      -c:v libx264 -preset ultrafast -crf 23 -pix_fmt yuv420p
      -profile:v high -level:v "$VIDEO_LEVEL" -g 48 -keyint_min 48 -sc_threshold 0
    )
    ;;
esac

COMMON_ARGS=(--input - --threads "$THREADS" --source-fps "$SOURCE_FPS" --skip-pairs "$SKIP_PAIRS" --mode "$PRODUCER_MODE")
if [[ "$SWAP_EYES" == 1 ]]; then COMMON_ARGS+=(--swap-eyes); fi
VIDEO_SEEK_ARGS=()
AUDIO_SEEK_ARGS=()
if [[ "$START_SECONDS" != 0.000000 ]]; then
  VIDEO_SEEK_ARGS=(-ss "$START_SECONDS")
  AUDIO_SEEK_ARGS=(-ss "$EFFECTIVE_START")
fi

section "Live pipeline"
if [[ "$SOURCE_KIND" == iso ]]; then
  SOURCE_PIPE_LABEL="UDF ISO -> physical SSIF MVC + base M2TS audio"
else
  SOURCE_PIPE_LABEL="Matroska demux -> Annex-B"
fi
if [[ -n "$VIDEO_POST_FILTER" ]]; then
  echo "$SOURCE_PIPE_LABEL -> edge264/pair discard/full-SBS intermediate -> $VIDEO_POST_METHOD -> H.264/AC-3 HLS"
else
  echo "$SOURCE_PIPE_LABEL -> edge264/pair discard/$OUTPUT_LABEL compositor -> raw-video pipe -> H.264/AC-3 HLS"
fi
echo "HLS becomes available while this pipeline is still running."
PIPE_START=$(date +%s%N)
AUDIO_SOURCE_STATUS=0
set +e
if [[ "$SOURCE_KIND" == iso ]]; then
  mkfifo "$ISO_AUDIO_FIFO"
  "$ISO_BIN" --input "$SOURCE_PATH" --audio --audio-track "$AUDIO_STREAM" --start-seconds "$EFFECTIVE_START" \
    >"$ISO_AUDIO_FIFO" 2> >(tee -a "$ISO_AUDIO_LOG" >&2) &
  ISO_AUDIO_PID=$!
  "$ISO_BIN" --input "$SOURCE_PATH" --video --start-seconds "$START_SECONDS" \
    --recovery-backoff-entries "$ISO_RECOVERY_BACKOFF" \
    2> >(tee -a "$ISO_VIDEO_LOG" >&2) | \
  "$BIN" "${COMMON_ARGS[@]}" \
    2> >(tee -a "$PRODUCER_LOG" >&2) | \
  ffmpeg -hide_banner -loglevel info -stats -y \
    -thread_queue_size 32 -f rawvideo -pix_fmt yuv420p -video_size "${PRODUCER_WIDTH}x${PRODUCER_HEIGHT}" \
    -framerate "$FPS_EXPR" -i pipe:0 \
    -thread_queue_size 8192 -f "$ISO_AUDIO_FORMAT" -i "$ISO_AUDIO_FIFO" \
    -filter_complex "$AUDIO_FILTER" \
    -map 0:v:0 -map '[aout]' \
    "${VIDEO_FILTER_ARGS[@]}" \
    "${VIDEO_ARGS[@]}" \
    -c:a ac3 -b:a "$AUDIO_BITRATE" "${AUDIO_CHANNEL_ARGS[@]}" \
    -shortest -max_muxing_queue_size 4096 \
    -f hls -hls_time 2 -hls_list_size 0 -hls_playlist_type event \
    -hls_flags independent_segments+temp_file \
    -hls_segment_filename "$HLS_DIR/segment-%05d.ts" \
    "$PLAYLIST" \
    2> >(tee -a "$FFMPEG_LOG" >&2)
  PIPE_STATUSES=("${PIPESTATUS[@]}")
  wait "$ISO_AUDIO_PID"
  AUDIO_SOURCE_STATUS=$?
  ISO_AUDIO_PID=
  rm -f "$ISO_AUDIO_FIFO"
else
  ffmpeg -hide_banner -loglevel warning "${VIDEO_SEEK_ARGS[@]}" -i "$SOURCE_PATH" \
    -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb \
    -an -sn -dn -f h264 pipe:1 \
    2> >(tee -a "$DEMUX_LOG" >&2) | \
  "$BIN" "${COMMON_ARGS[@]}" \
    2> >(tee -a "$PRODUCER_LOG" >&2) | \
  ffmpeg -hide_banner -loglevel info -stats -y \
    -thread_queue_size 32 -f rawvideo -pix_fmt yuv420p -video_size "${PRODUCER_WIDTH}x${PRODUCER_HEIGHT}" \
    -framerate "$FPS_EXPR" -i pipe:0 \
    -thread_queue_size 128 "${AUDIO_SEEK_ARGS[@]}" -i "$SOURCE_PATH" \
    -filter_complex "$AUDIO_FILTER" \
    -map 0:v:0 -map '[aout]' \
    "${VIDEO_FILTER_ARGS[@]}" \
    "${VIDEO_ARGS[@]}" \
    -c:a ac3 -b:a "$AUDIO_BITRATE" "${AUDIO_CHANNEL_ARGS[@]}" \
    -shortest -max_muxing_queue_size 4096 \
    -f hls -hls_time 2 -hls_list_size 0 -hls_playlist_type event \
    -hls_flags independent_segments+temp_file \
    -hls_segment_filename "$HLS_DIR/segment-%05d.ts" \
    "$PLAYLIST" \
    2> >(tee -a "$FFMPEG_LOG" >&2)
  PIPE_STATUSES=("${PIPESTATUS[@]}")
fi
set -e
PIPE_END=$(date +%s%N)
DEMUX_STATUS=${PIPE_STATUSES[0]:-99}
PRODUCER_STATUS=${PIPE_STATUSES[1]:-99}
FFMPEG_STATUS=${PIPE_STATUSES[2]:-99}

echo "Video source/demux status: $DEMUX_STATUS"
[[ "$SOURCE_KIND" == iso ]] && echo "ISO audio source status: $AUDIO_SOURCE_STATUS"
echo "MVC producer status: $PRODUCER_STATUS"
echo "HLS encoder status: $FFMPEG_STATUS"
[[ $DEMUX_STATUS -eq 0 ]] || fail "$SOURCE_KIND video source failed with status $DEMUX_STATUS"
[[ $AUDIO_SOURCE_STATUS -eq 0 ]] || fail "ISO audio source failed with status $AUDIO_SOURCE_STATUS"
[[ $PRODUCER_STATUS -eq 0 ]] || fail "MVC decoder/compositor failed with status $PRODUCER_STATUS"
[[ $FFMPEG_STATUS -eq 0 ]] || fail "HLS encoder/muxer failed with status $FFMPEG_STATUS"
[[ -s "$PLAYLIST" ]] || fail "No HLS playlist was created"

RESULT_LINE=$(grep '^RESULT ' "$PRODUCER_LOG" | tail -n1 || true)
[[ -n "$RESULT_LINE" ]] || fail "No final decoder RESULT line was produced"
echo "$RESULT_LINE"
for bad in 'base_only=[1-9]' 'mvc_only=[1-9]' 'poc_mismatches=[1-9]' 'errors=[1-9]'; do
  if grep -Eq "$bad" <<<"$RESULT_LINE"; then fail "Stereo-pair correctness check failed: $RESULT_LINE"; fi
done
ACTUAL_SKIPPED=$(sed -nE 's/.* skipped_pairs=([0-9]+).*/\1/p' <<<"$RESULT_LINE")
[[ "$ACTUAL_SKIPPED" == "$SKIP_PAIRS" ]] || fail "Seek discard mismatch: requested $SKIP_PAIRS pairs, decoder discarded ${ACTUAL_SKIPPED:-unknown}"
ACTUAL_DIMENSIONS=$(sed -nE 's/.* dimensions=([0-9]+x[0-9]+).*/\1/p' <<<"$RESULT_LINE")
EXPECTED_PRODUCER_DIMENSIONS="${PRODUCER_WIDTH}x${PRODUCER_HEIGHT}"
EXPECTED_DIMENSIONS="${OUTPUT_WIDTH}x${OUTPUT_HEIGHT}"
[[ "$ACTUAL_DIMENSIONS" == "$EXPECTED_PRODUCER_DIMENSIONS" ]] || fail "Compositor dimension mismatch: expected $EXPECTED_PRODUCER_DIMENSIONS, produced ${ACTUAL_DIMENSIONS:-unknown}"

section "Completed stream validation"
SEGMENTS=$(find "$HLS_DIR" -maxdepth 1 -type f -name 'segment-*.ts' | wc -l)
FIRST_SEGMENT="$HLS_DIR/segment-00000.ts"
if [[ ! -s "$FIRST_SEGMENT" ]]; then
  FIRST_SEGMENT=$(find "$HLS_DIR" -maxdepth 1 -type f -name 'segment-*.ts' -print -quit)
fi
[[ "$SEGMENTS" -gt 0 && -n "$FIRST_SEGMENT" ]] || fail "No complete HLS segments were created"
ffprobe -v error -show_entries stream=index,codec_type,codec_name,width,height,channels,channel_layout \
  -of default=noprint_wrappers=1 "$FIRST_SEGMENT"
mapfile -t ENCODED_DIMENSION_LINES < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
  -of csv=s=x:p=0 "$FIRST_SEGMENT")
ENCODED_DIMENSIONS=${ENCODED_DIMENSION_LINES[0]:-}
[[ "$ENCODED_DIMENSIONS" == "$EXPECTED_DIMENSIONS" ]] || fail "Encoded HLS dimensions mismatch: expected $EXPECTED_DIMENSIONS, encoded ${ENCODED_DIMENSIONS:-unknown}"
ENCODED_AUDIO=$(ffprobe -v error -select_streams a:0 -show_entries stream=channels,channel_layout \
  -of csv=s=,:p=0 "$FIRST_SEGMENT" | head -n1)
if (( AUDIO_CHANNELS >= 6 )); then
  [[ "$ENCODED_AUDIO" == "6,5.1(side)" ]] || fail "Encoded HLS audio mismatch: expected 6,5.1(side), encoded ${ENCODED_AUDIO:-unknown}"
fi
ELAPSED=$(python3 - "$PIPE_START" "$PIPE_END" <<'PY'
import sys
print(f"{(int(sys.argv[2]) - int(sys.argv[1])) / 1_000_000_000:.3f}")
PY
)
echo "Segments: $SEGMENTS"
echo "Pipeline wall time: $ELAPSED s"
echo "Output mode: $OUTPUT_MODE ($OUTPUT_LABEL)"
echo "Output dimensions: $EXPECTED_DIMENSIONS"
echo "Intermediate compositor: $PRODUCER_MODE ($EXPECTED_PRODUCER_DIMENSIONS)"
echo "Encoded audio: $ENCODED_AUDIO"
[[ -n "$VIDEO_POST_METHOD" ]] && echo "Video post-process: $VIDEO_POST_METHOD"
[[ -n "$ANAGLYPH_METHOD" ]] && echo "Anaglyph method: $ANAGLYPH_METHOD"
echo "Playlist: $PLAYLIST"
echo "Validation: PASS"
