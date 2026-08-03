#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TMP=$(mktemp -d -t sylc-truehd-runner-XXXXXX)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/build" "$TMP/session"
printf 'synthetic ISO placeholder\n' > "$TMP/Test3D.iso"

ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'aevalsrc=0.08*sin(2*PI*440*t)|0.08*sin(2*PI*550*t)|0.08*sin(2*PI*660*t)|0.08*sin(2*PI*770*t)|0.08*sin(2*PI*880*t)|0.08*sin(2*PI*990*t):s=48000:d=2' \
  -channel_layout '5.1(side)' -c:a truehd -strict -2 -y "$TMP/audio.thd"

cat > "$TMP/build/sylc_iso_source" <<'PY'
#!/usr/bin/env python3
import json, os, sys
args=sys.argv[1:]
if '--probe' in args:
    print(json.dumps({'ok':True,'sourceType':'bluray-iso','playlist':'00001.mpls','selectionMethod':'synthetic-test','durationSeconds':2.0,'width':16,'height':16,'fps':24.0,'hasMVC':True,'segmentCount':1,'decoysFiltered':0,'audioTracks':[{'index':0,'pid':4352,'streamType':131,'format':'truehd','profile':'Dolby TrueHD','language':'eng','channels':6,'sampleRate':48000,'supported':True,'decodePath':'native TrueHD extraction and FFmpeg lossless decode','truehdMajorSync':True,'embeddedAc3Core':False}]}))
elif '--plan-video-seek' in args:
    print(json.dumps({'ok':True,'requestedStartSeconds':0.0,'actualDemuxStartSeconds':0.0,'firstOutputSeconds':0.0,'skipPairs':0,'firstClip':'00001','seekDetail':'synthetic','recoveryPreflight':'NOT_REQUIRED','recoveryAttempts':0,'recoveryBackoffEntries':0,'recoveryAnchorSeconds':0.0,'recoveryMethod':'zero','calibrationSamples':0,'calibrationInliers':0,'calibratedOffsetMs':0,'phaseShiftFrames':0,'phaseResidualMs':0,'leadingBaseDiscards':0,'leadingDependentDiscards':0,'hiddenStructuralPairs':0,'hiddenStabilizationPairs':0,'hiddenRecoveryPairs':0,'targetPrerollPairs':0,'cleanReleaseSeconds':0.0}))
elif '--audio' in args:
    with open(os.environ['TRUEHD_SAMPLE'],'rb') as handle:
        sys.stdout.buffer.write(handle.read())
elif '--video' in args:
    pass
else:
    raise SystemExit(2)
PY
chmod +x "$TMP/build/sylc_iso_source"

cat > "$TMP/build/sylc_hsbs_pipe" <<'SH2'
#!/usr/bin/env bash
cat >/dev/null
python3 - <<'PY'
import sys
sys.stdout.buffer.write(bytes(16*16*3//2*48))
PY
echo 'RESULT threads=0 nals=0 outputs=48 pairs=48 emitted_pairs=48 skipped_pairs=0 base_only=0 mvc_only=0 poc_mismatches=0 errors=0 retries=0 ignored_tiny_type24=0 wall_s=0.1 cpu_s=0.1 avg_cpu_cores=1 pair_fps=480 realtime_x=20 output_bytes=18432 output_sample_fnv1a64=0x0 dimensions=16x16' >&2
SH2
chmod +x "$TMP/build/sylc_hsbs_pipe"

TRUEHD_SAMPLE="$TMP/audio.thd" \
SYLC_ENGINE_PROJECT="$TMP" SYLC_ENGINE_BUILD_DIR="$TMP/build" \
SYLC_SESSION_DIR="$TMP/session" SYLC_ENCODER_MODE=software \
SYLC_OUTPUT_MODE=half-sbs SYLC_AUDIO_STREAM=0 SYLC_START_SECONDS=0 \
"$ROOT/engine/run-phase6-streaming-session.sh" "$TMP/Test3D.iso" >/dev/null

grep -q 'ISO audio decode path: native TrueHD extraction and FFmpeg lossless decode' "$TMP/session/diagnostic-report.txt"
grep -q 'Input #1, truehd' "$TMP/session/diagnostic-report.txt"
grep -q 'Audio: ac3, 48000 Hz, 5.1(side)' "$TMP/session/diagnostic-report.txt"
ffprobe -v error -show_entries stream=codec_name,channels,channel_layout -of json \
  "$TMP/session/hls/stream.m3u8" | python3 -c 'import json,sys; streams=json.load(sys.stdin)["streams"]; a=next(s for s in streams if s["codec_name"]=="ac3"); assert a["channels"]==6 and a["channel_layout"]=="5.1(side)"'
echo 'Full runner TrueHD bridge integration: PASS'
