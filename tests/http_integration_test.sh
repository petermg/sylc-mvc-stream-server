#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TMP=$(mktemp -d -t sylc-phase6-http-XXXXXX)
PORT=${SYLC_TEST_PORT:-18097}
PID=
cleanup() {
  if [[ -n ${PID:-} ]]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
  rm -rf "$TMP"
}
trap cleanup EXIT
mkdir -p "$TMP/media" "$TMP/phase6/build" "$TMP/state" "$TMP/legacy"
ffmpeg -v error -f lavfi -i color=size=16x16:rate=24 -t 2 -c:v ffv1 -y "$TMP/media/ShortMVC.mkv"
cat > "$TMP/media/ShortMVC.eng.srt" <<'EOF'
1
00:00:00,000 --> 00:00:01,500
SyLC subtitle test
EOF
printf 'fake-udf-image\n' > "$TMP/media/TestDisc3D.iso"
cp "$ROOT/tests/fake-phase5.sh" "$TMP/phase6/run-phase6.sh"
chmod +x "$TMP/phase6/run-phase6.sh"
printf '#!/usr/bin/env bash\nexit 0\n' > "$TMP/phase6/build/sylc_hsbs_pipe"
chmod +x "$TMP/phase6/build/sylc_hsbs_pipe"
cat > "$TMP/phase6/build/sylc_iso_source" <<'PYISO'
#!/usr/bin/env python3
import json, sys
args = sys.argv[1:]
if '--probe' in args:
    print(json.dumps({
        'ok': True, 'sourceType': 'bluray-iso', 'playlist': '00001.mpls',
        'selectionMethod': 'libbluray-titles-all', 'durationSeconds': 5400.0,
        'width': 1920, 'height': 1080, 'fps': 24000/1001, 'hasMVC': True,
        'segmentCount': 2, 'decoysFiltered': 1, 'libblurayAuthoritative': True,
        'libblurayVersion': '1.3.4', 'selectedTitleIndex': 3, 'titleCount': 8,
        'mainTitleHint': 3,
        'audioTracks': [
            {'index': 0, 'format': 'dts', 'profile': 'DTS-HD MA', 'language': 'eng', 'channels': 6, 'sampleRate': 48000, 'supported': True},
            {'index': 1, 'format': 'ac3', 'profile': 'AC-3', 'language': 'spa', 'channels': 6, 'sampleRate': 48000, 'supported': True},
            {'index': 2, 'format': 'truehd', 'profile': 'Dolby TrueHD', 'language': 'eng', 'channels': 8, 'sampleRate': 48000, 'supported': True, 'decodePath': 'native TrueHD extraction and FFmpeg lossless decode', 'truehdMajorSync': True, 'embeddedAc3Core': True},
        ],
        'subtitleTracks': [
            {'index': 0, 'pid': 4608, 'format': 'pgs', 'profile': 'Blu-ray PGS', 'language': 'eng', 'supported': True},
        ],
    }))
elif '--plan-video-seek' in args:
    print(json.dumps({'ok': True, 'requestedStartSeconds': 0, 'actualDemuxStartSeconds': 0, 'firstOutputSeconds': 0, 'skipPairs': 0, 'firstClip': '00001', 'seekDetail': 'fake'}))
PYISO
chmod +x "$TMP/phase6/build/sylc_iso_source"

SYLC_BIND_HOST=127.0.0.1 \
SYLC_PORT="$PORT" \
SYLC_MEDIA_ROOTS="$TMP/media" \
SYLC_STATE_ROOT="$TMP/state" \
SYLC_CONFIG_FILE="$TMP/config.json" \
SYLC_BROWSE_ROOTS="$TMP" \
SYLC_ENGINE_PROJECT="$TMP/phase6" \
SYLC_ENGINE_RUNNER="$TMP/phase6/run-phase6.sh" \
SYLC_ENGINE_WRAPPER="$TMP/phase6/run-phase6.sh" \
SYLC_PHASE5_LEGACY_WORK_ROOT="$TMP/legacy" \
SYLC_PHASE5_LEGACY_REPORT="$TMP/legacy-report.txt" \
SYLC_CLEANUP_INTERVAL_SECONDS=0 \
SYLC_MINIMUM_FREE_GIB=0 \
SYLC_EMERGENCY_FREE_GIB=0 \
PYTHONDONTWRITEBYTECODE=1 \
python3 "$ROOT/app/server.py" >"$TMP/server.log" 2>&1 &
PID=$!

for _ in $(seq 1 50); do
  curl -fsS "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1 && break
  sleep .1
done
health=$(curl -fsS "http://127.0.0.1:$PORT/api/health")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["version"] == "0.7.0-alpha.3"; assert d["setupRequired"] is False; assert len(d["mediaLibraries"]) == 1; assert d["isoSourceAdapterAvailable"]; assert d["nativeTrueHdAudio"] is True; assert "truehd" in d["supportedIsoAudioFormats"]; assert d["pgsSubtitleBurnIn"] is True; assert d["pgsSubtitleCatalogOnly"] is False; assert d["pgsZeroTimelineAnchor"] is True; assert "sup" in d["sidecarSubtitleFormats"]; assert "full-sbs" in d["supportedOutputModes"]; assert "full-ou" in d["supportedOutputModes"]; assert "anaglyph-color" in d["supportedOutputModes"]; assert "anaglyph-dubois" in d["supportedOutputModes"]; assert "passive-rows-left-top" in d["supportedOutputModes"]; assert "passive-rows-right-top" in d["supportedOutputModes"]' <<<"$health"
libraries=$(curl -fsS "http://127.0.0.1:$PORT/api/libraries?refresh=1")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert len(d["libraries"]) == 1; x=d["libraries"][0]; assert x["readable"] is True; assert x["indexedFiles"] == 2' <<<"$libraries"
browse=$(curl -fsS "http://127.0.0.1:$PORT/api/filesystem/directories?path=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "$TMP")")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert any(x["name"] == "media" for x in d["directories"])' <<<"$browse"
media=$(curl -fsS "http://127.0.0.1:$PORT/api/media")
media_id=$(python3 -c 'import json,sys; print(next(x["id"] for x in json.load(sys.stdin)["items"] if x["type"] != "iso"))' <<<"$media")
iso_id=$(python3 -c 'import json,sys; print(next(x["id"] for x in json.load(sys.stdin)["items"] if x["type"] == "iso"))' <<<"$media")
probe=$(curl -fsS -X POST -H 'Content-Type: application/json' \
  -d "{\"mediaId\":\"$media_id\"}" \
  "http://127.0.0.1:$PORT/api/media/probe")
python3 -c 'import json,sys; m=json.load(sys.stdin)["media"]; assert m["durationSeconds"] > 1; tracks=m["subtitleTracks"]; assert any(t["id"] == "sidecar:ShortMVC.eng.srt" and t["supported"] for t in tracks)' <<<"$probe"
curl -fsS -X POST -H 'Content-Type: application/json' \
  -d "{\"mediaId\":\"$media_id\",\"mode\":\"full-sbs\",\"audioStream\":0,\"subtitleId\":\"sidecar:ShortMVC.eng.srt\",\"swapEyes\":true,\"startSeconds\":0}" \
  "http://127.0.0.1:$PORT/api/sessions" >/dev/null

status=
for _ in $(seq 1 80); do
  status=$(curl -fsS "http://127.0.0.1:$PORT/api/sessions/current")
  playable=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["session"]["playable"])' <<<"$status")
  [[ $playable == True ]] && break
  sleep .1
done
session_id=$(python3 -c 'import json,sys; d=json.load(sys.stdin)["session"]; assert d["outputMode"] == "full-sbs"; assert d["swapEyes"] is True; assert d["subtitleId"] == "sidecar:ShortMVC.eng.srt"; print(d["id"])' <<<"$status")
curl -fsS "http://127.0.0.1:$PORT/hls/$session_id/stream.m3u8" > "$TMP/stream.m3u8"
grep -q 'segment-001.ts' "$TMP/stream.m3u8"

for _ in $(seq 1 80); do
  status=$(curl -fsS "http://127.0.0.1:$PORT/api/sessions/current")
  session_state=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["session"]["state"])' <<<"$status")
  [[ $session_state == completed ]] && break
  sleep .1
done
[[ $session_state == completed ]]
curl -fsS "http://127.0.0.1:$PORT/api/sessions/current/report" > "$TMP/report.txt"
grep -q 'Source verification: True' "$TMP/report.txt"

seek=$(curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"startSeconds":1,"mode":"anaglyph-dubois","swapEyes":false}' \
  "http://127.0.0.1:$PORT/api/sessions/current/seek")
replacement_id=$(python3 -c 'import json,sys; d=json.load(sys.stdin); s=d["session"]; print(s["id"]); assert s["replacesSessionId"] == d["previousSessionId"]; assert s["outputMode"] == "anaglyph-dubois"; assert s["swapEyes"] is False; assert s["subtitleId"] == "sidecar:ShortMVC.eng.srt"' <<<"$seek")
[[ $replacement_id != "$session_id" ]]
curl -fsS "http://127.0.0.1:$PORT/hls/$session_id/stream.m3u8" | grep -q 'segment-003.ts'
for _ in $(seq 1 80); do
  status=$(curl -fsS "http://127.0.0.1:$PORT/api/sessions/current")
  session_state=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["session"]["state"])' <<<"$status")
  [[ $session_state == completed ]] && break
  sleep .1
done
[[ $session_state == completed ]]

iso_probe=$(curl -fsS -X POST -H 'Content-Type: application/json' \
  -d "{\"mediaId\":\"$iso_id\"}" \
  "http://127.0.0.1:$PORT/api/media/probe")
python3 -c 'import json,sys; m=json.load(sys.stdin)["media"]; assert m["sourceType"] == "bluray-iso"; assert m["playlist"] == "00001.mpls"; assert len(m["audioTracks"]) == 3; assert m["audioTracks"][2]["format"] == "truehd"; assert m["audioTracks"][2]["supported"] is True; assert len(m["subtitleTracks"]) == 1; assert m["subtitleTracks"][0]["id"] == "iso-pgs:4608"; assert m["subtitleTracks"][0]["supported"] is True' <<<"$iso_probe"

curl -fsS -X POST -H 'Content-Type: application/json' \
  -d "{\"mediaId\":\"$iso_id\",\"mode\":\"half-sbs\",\"audioStream\":1,\"swapEyes\":false,\"startSeconds\":600}" \
  "http://127.0.0.1:$PORT/api/sessions" >/dev/null
for _ in $(seq 1 80); do
  status=$(curl -fsS "http://127.0.0.1:$PORT/api/sessions/current")
  session_state=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["session"]["state"])' <<<"$status")
  [[ $session_state == completed ]] && break
  sleep .1
done
[[ $session_state == completed ]]
python3 -c 'import json,sys; s=json.load(sys.stdin)["session"]; assert s["audioStream"] == 1; assert s["source"]["sourceType"] == "bluray-iso"; assert s["source"]["playlist"] == "00001.mpls"' <<<"$status"
curl -fsS "http://127.0.0.1:$PORT/api/sessions/current/report" | grep -q 'Audio stream: 1'
printf 'HTTP integration: PASS (state=%s, original=%s, replacement=%s)\n' "$session_state" "$session_id" "$replacement_id"
