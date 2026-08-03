#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TMP=$(mktemp -d -t sylc-setup-http-XXXXXX)
PORT=${SYLC_SETUP_TEST_PORT:-18107}
PID=
cleanup() {
  if [[ -n ${PID:-} ]]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
  rm -rf "$TMP"
}
trap cleanup EXIT
mkdir -p "$TMP/media-a" "$TMP/media-b" "$TMP/phase/build" "$TMP/state"
printf x > "$TMP/media-a/One.mkv"
printf x > "$TMP/media-b/Two.iso"
printf '#!/bin/sh\nexit 0\n' > "$TMP/runner.sh"; chmod +x "$TMP/runner.sh"
for bin in sylc_hsbs_pipe sylc_iso_source; do printf '#!/bin/sh\nexit 0\n' > "$TMP/phase/build/$bin"; chmod +x "$TMP/phase/build/$bin"; done

SYLC_BIND_HOST=127.0.0.1 \
SYLC_PORT="$PORT" \
SYLC_MEDIA_ROOTS= \
SYLC_CONFIG_FILE="$TMP/config.json" \
SYLC_BROWSE_ROOTS="$TMP" \
SYLC_STATE_ROOT="$TMP/state" \
SYLC_ENGINE_PROJECT="$TMP/phase" \
SYLC_ENGINE_RUNNER="$TMP/runner.sh" \
SYLC_ENGINE_WRAPPER="$TMP/runner.sh" \
SYLC_CLEANUP_INTERVAL_SECONDS=0 \
SYLC_MINIMUM_FREE_GIB=0 \
SYLC_EMERGENCY_FREE_GIB=0 \
PYTHONDONTWRITEBYTECODE=1 \
python3 "$ROOT/app/server.py" >"$TMP/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -fsS "http://127.0.0.1:$PORT/api/setup/status" >/dev/null 2>&1 && break; sleep .1; done
status=$(curl -fsS "http://127.0.0.1:$PORT/api/setup/status")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["setupRequired"] is True; assert d["tokenConfigured"] is False' <<<"$status"

curl -fsS -X POST -H 'Content-Type: application/json' \
  -d "{\"name\":\"Main\",\"path\":\"$TMP/media-a\",\"recursive\":true,\"apiToken\":\"alpha-secret\"}" \
  "http://127.0.0.1:$PORT/api/setup" >/dev/null

code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/media")
[[ $code == 401 ]]
media=$(curl -fsS -H 'X-SyLC-Token: alpha-secret' "http://127.0.0.1:$PORT/api/media")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert len(d["items"]) == 1; assert d["items"][0]["libraryName"] == "Main"' <<<"$media"

curl -fsS -X POST -H 'Content-Type: application/json' -H 'Authorization: Bearer alpha-secret' \
  -d "{\"name\":\"Second\",\"path\":\"$TMP/media-b\",\"recursive\":false}" \
  "http://127.0.0.1:$PORT/api/libraries" >/dev/null
libraries=$(curl -fsS -H 'X-SyLC-Token: alpha-secret' "http://127.0.0.1:$PORT/api/libraries?refresh=1")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert len(d["libraries"]) == 2; assert sum(x["indexedFiles"] for x in d["libraries"]) == 2' <<<"$libraries"

curl -fsS -X POST -H 'Content-Type: application/json' -H 'X-SyLC-Token: alpha-secret' \
  -d '{"apiToken":""}' "http://127.0.0.1:$PORT/api/settings/token" >/dev/null
curl -fsS "http://127.0.0.1:$PORT/api/media" >/dev/null

grep -q 'apiTokenSha256' "$TMP/config.json"
printf 'Setup/auth integration: PASS\n'
