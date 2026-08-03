#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${SYLC_TEST_BUILD_DIR:-$ROOT/engine/phase6-streaming/build}
BIN=$BUILD/sylc_hsbs_pipe
SOURCE=${1:-${SYLC_SHORTMVC_SOURCE:-}}
TMP=$(mktemp -d -t sylc-hardening-XXXXXX)
trap 'rm -rf "$TMP"' EXIT

command -v ffmpeg >/dev/null
command -v ffprobe >/dev/null
[[ -x "$BIN" ]] || { echo "Missing built compositor: $BIN" >&2; exit 2; }

PAN='pan=5.1(side)|FL=FL|FR=FR|FC=FC|LFE=LFE|SL<SL+BL|SR<SR+BR'

# Verify an actual 7.1 input becomes AC-3 5.1(side), and that a signal placed
# only in the source LFE channel remains present in the output LFE channel.
ffmpeg -hide_banner -v error \
  -f lavfi -i 'aevalsrc=0|0|0|0.5*sin(2*PI*80*t)|0|0|0|0:s=48000:d=1:channel_layout=7.1' \
  -af "$PAN" -c:a ac3 -b:a 640k -ac 6 -y "$TMP/lfe.ac3"
probe=$(ffprobe -v error -show_entries stream=channels,channel_layout -of csv=p=0 "$TMP/lfe.ac3")
[[ "$probe" == '6,5.1(side)' ]]
volume=$(ffmpeg -hide_banner -v info -i "$TMP/lfe.ac3" \
  -af 'pan=mono|c0=LFE,volumedetect' -f null - 2>&1 \
  | sed -nE 's/.*max_volume: ([^ ]+) dB.*/\1/p' | tail -n1)
[[ -n "$volume" && "$volume" != '-inf' ]]
echo "7.1 -> AC-3 5.1(side) with LFE preservation: PASS (LFE max ${volume} dB)"

# The same named-channel mapping must remain valid for a native 5.1(side) input.
ffmpeg -hide_banner -v error \
  -f lavfi -i 'anullsrc=channel_layout=5.1(side):sample_rate=48000:d=0.2' \
  -af "$PAN" -c:a ac3 -b:a 640k -ac 6 -y "$TMP/native51.ac3"
probe=$(ffprobe -v error -show_entries stream=channels,channel_layout -of csv=p=0 "$TMP/native51.ac3")
[[ "$probe" == '6,5.1(side)' ]]
echo 'Native 5.1 rematrix compatibility: PASS'

if [[ -n "$SOURCE" ]]; then
  [[ -r "$SOURCE" ]] || { echo "ShortMVC source is unreadable: $SOURCE" >&2; exit 2; }
  ffmpeg -v error -i "$SOURCE" -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb \
    -an -sn -dn -f h264 "$TMP/original.h264"
  python3 - "$TMP/original.h264" "$TMP/injected.h264" <<'PY'
from pathlib import Path
import sys
src = Path(sys.argv[1]).read_bytes()
starts=[]
i=0
while i < len(src)-3:
    if src[i:i+4] == b'\x00\x00\x00\x01':
        starts.append((i,4)); i += 4
    elif src[i:i+3] == b'\x00\x00\x01':
        starts.append((i,3)); i += 3
    else:
        i += 1
if not starts:
    raise SystemExit('No Annex-B start codes found')
out=bytearray()
for index,(pos,length) in enumerate(starts):
    end = starts[index+1][0] if index+1 < len(starts) else len(src)
    out += b'\x00\x00\x00\x01\x78'  # tiny nal_unit_type=24 framing marker
    out += src[pos:end]
Path(sys.argv[2]).write_bytes(out)
PY
  "$BIN" --input "$TMP/original.h264" --threads 0 --source-fps 23.976023976 \
    --mode half-sbs --discard-output 2>"$TMP/original.log"
  "$BIN" --input "$TMP/injected.h264" --threads 0 --source-fps 23.976023976 \
    --mode half-sbs --discard-output 2>"$TMP/injected.log"
  python3 - "$TMP/original.log" "$TMP/injected.log" <<'PY'
import re, sys

def result(path):
    lines=[line for line in open(path, encoding='utf-8', errors='replace') if line.startswith('RESULT ')]
    if not lines: raise SystemExit(f'No RESULT line in {path}')
    return dict(re.findall(r'(\w+)=([^ ]+)', lines[-1]))
a=result(sys.argv[1]); b=result(sys.argv[2])
for key in ('pairs','emitted_pairs','output_sample_fnv1a64','dimensions'):
    if a.get(key) != b.get(key): raise SystemExit(f'{key} changed: {a.get(key)} != {b.get(key)}')
if a.get('errors') != '0' or b.get('errors') != '0': raise SystemExit('Unexpected decoder error')
if int(b.get('ignored_tiny_type24','0')) <= 0: raise SystemExit('Injected markers were not counted')
print('Tiny type-24 framing regression: PASS')
print('Output hash:', b['output_sample_fnv1a64'])
PY
else
  echo 'Tiny type-24 ShortMVC regression: SKIPPED (no source argument)'
fi
