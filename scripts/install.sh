#!/usr/bin/env bash
set -Eeuo pipefail

DEST=${DEST:-/srv/sylc-mvc-stream}
ENV_FILE=${ENV_FILE:-/etc/sylc-mvc-stream.env}
CONFIG_DIR=${CONFIG_DIR:-/var/lib/sylc-mvc-stream}
CONFIG_FILE=${CONFIG_FILE:-$CONFIG_DIR/config.json}
UNIT_FILE=${UNIT_FILE:-/etc/systemd/system/sylc-mvc-stream.service}
SERVICE_NAME=sylc-mvc-stream.service
SERVICE_USER=${SERVICE_USER:-}
BIND_HOST=${BIND_HOST:-}
PORT=${PORT:-}
VAAPI_DEVICE=${VAAPI_DEVICE:-}
ENCODER_MODE=${ENCODER_MODE:-}
BUNDLE_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ENGINE_DEST=$DEST/engine/phase6-streaming
RUNNER_DEST=$DEST/engine/run-phase6-streaming-session.sh

usage() {
  cat <<'EOF'
Usage: sudo ./scripts/install.sh [options]

Options:
  --service-user USER   Linux user that runs SyLC (default: preserve existing,
                        otherwise the sudo-invoking user, otherwise create sylc)
  --bind-host ADDRESS   Listen address (default: preserve existing or 0.0.0.0)
  --port PORT           HTTP port (default: preserve existing or 8097)
  --vaapi-device PATH   VA-API render device (default: /dev/dri/renderD128)
  --software-encoder    Use libx264 instead of VA-API
  --help                Show this help

Media libraries and the optional API token are configured in the web UI.
Existing SYLC_MEDIA_ROOTS values are migrated automatically on first startup.
EOF
}

while (($#)); do
  case "$1" in
    --service-user) SERVICE_USER=${2:?missing user}; shift 2 ;;
    --bind-host) BIND_HOST=${2:?missing address}; shift 2 ;;
    --port) PORT=${2:?missing port}; shift 2 ;;
    --vaapi-device) VAAPI_DEVICE=${2:?missing path}; shift 2 ;;
    --software-encoder) ENCODER_MODE=software; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ $EUID -eq 0 ]] || { echo "Run with sudo: sudo ./scripts/install.sh" >&2; exit 1; }

read_env_value() {
  local key=$1 file=$2
  [[ -f $file ]] || return 0
  sed -n -E "s/^[[:space:]]*${key}=//p" "$file" | tail -n 1
}

existing_user=
if [[ -f $UNIT_FILE ]]; then
  existing_user=$(sed -n -E 's/^[[:space:]]*User=//p' "$UNIT_FILE" | head -n 1 || true)
fi
if [[ -z $SERVICE_USER ]]; then
  if [[ -n $existing_user ]] && id "$existing_user" >/dev/null 2>&1; then
    SERVICE_USER=$existing_user
  elif [[ -n ${SUDO_USER:-} && ${SUDO_USER:-root} != root ]] && id "$SUDO_USER" >/dev/null 2>&1; then
    SERVICE_USER=$SUDO_USER
  else
    SERVICE_USER=sylc
  fi
fi
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
  if [[ $SERVICE_USER != sylc ]]; then
    echo "Linux user does not exist: $SERVICE_USER" >&2
    exit 1
  fi
  useradd --system --home-dir "$CONFIG_DIR" --create-home --shell /usr/sbin/nologin sylc
  echo "Created dedicated service user: sylc"
fi
SERVICE_GROUP=$(id -gn "$SERVICE_USER")

old_bind=$(read_env_value SYLC_BIND_HOST "$ENV_FILE")
old_port=$(read_env_value SYLC_PORT "$ENV_FILE")
old_vaapi=$(read_env_value SYLC_VAAPI_DEVICE "$ENV_FILE")
old_encoder=$(read_env_value SYLC_ENCODER_MODE "$ENV_FILE")
BIND_HOST=${BIND_HOST:-${old_bind:-0.0.0.0}}
PORT=${PORT:-${old_port:-8097}}
VAAPI_DEVICE=${VAAPI_DEVICE:-${old_vaapi:-/dev/dri/renderD128}}
ENCODER_MODE=${ENCODER_MODE:-${old_encoder:-vaapi}}
[[ $PORT =~ ^[0-9]+$ ]] && ((PORT >= 1 && PORT <= 65535)) || { echo "Invalid port: $PORT" >&2; exit 1; }
[[ $ENCODER_MODE == vaapi || $ENCODER_MODE == software ]] || { echo "Encoder mode must be vaapi or software" >&2; exit 1; }

for command in python3 ffmpeg ffprobe cmake ninja c++ curl systemctl runuser ss getent; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "Required command is missing: $command" >&2
    echo "On Ubuntu/Debian, install: ffmpeg cmake ninja-build build-essential curl libbluray2" >&2
    exit 1
  }
done

for required in \
  "$BUNDLE_ROOT/app/server.py" \
  "$BUNDLE_ROOT/app/static/index.html" \
  "$BUNDLE_ROOT/engine/run-phase6-streaming-session.sh" \
  "$BUNDLE_ROOT/engine/phase6-streaming/CMakeLists.txt" \
  "$BUNDLE_ROOT/engine/phase6-streaming/src/sylc_hsbs_pipe.cpp" \
  "$BUNDLE_ROOT/engine/phase6-streaming/src/iso/sylc_iso_source.cpp" \
  "$BUNDLE_ROOT/engine/phase6-streaming/src/iso/sylc_m2ts_pgs_demuxer.cpp" \
  "$BUNDLE_ROOT/engine/phase6-streaming/src/iso/sylc_m2ts_pgs_demuxer.h" \
  "$BUNDLE_ROOT/engine/phase6-streaming/src/iso/sylc_truehd_framer.h" \
  "$BUNDLE_ROOT/engine/phase6-streaming/src/videolan/libudfread-1.2.0/COPYING" \
  "$BUNDLE_ROOT/systemd/sylc-mvc-stream.service.in"; do
  [[ -f $required ]] || { echo "Package file is missing: $required" >&2; exit 1; }
done

was_active=0
if systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
  was_active=1
  echo "Stopping the existing SyLC service for upgrade..."
  systemctl stop "$SERVICE_NAME"
fi

if ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q .; then
  echo "TCP $PORT is already in use by another process." >&2
  ss -ltnp "sport = :$PORT" || true
  if (( was_active )); then systemctl start "$SERVICE_NAME" || true; fi
  exit 1
fi

stamp=$(date +%Y%m%d-%H%M%S)
backup=${DEST}.bak-${stamp}
if [[ -d $DEST ]]; then
  install -d -m 0750 "$backup"
  for item in app engine docs README.md LICENSE NOTICE THIRD_PARTY_NOTICES.md RELEASE_NOTES.md VERSION; do
    [[ -e $DEST/$item ]] && cp -a "$DEST/$item" "$backup/"
  done
  [[ -f $ENV_FILE ]] && cp -a "$ENV_FILE" "$backup/sylc-mvc-stream.env"
  [[ -f $CONFIG_FILE ]] && cp -a "$CONFIG_FILE" "$backup/config.json"
  [[ -f $UNIT_FILE ]] && cp -a "$UNIT_FILE" "$backup/sylc-mvc-stream.service"
  echo "Backed up the previous installation to $backup"
fi

install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_GROUP" "$DEST" "$DEST/state" "$CONFIG_DIR"
rm -rf "$DEST/app" "$DEST/engine" "$DEST/docs"
install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_GROUP" \
  "$DEST/app" "$DEST/app/static" "$DEST/engine" "$DEST/docs"

install -m 0755 -o "$SERVICE_USER" -g "$SERVICE_GROUP" "$BUNDLE_ROOT/app/server.py" "$DEST/app/server.py"
install -m 0644 -o "$SERVICE_USER" -g "$SERVICE_GROUP" "$BUNDLE_ROOT/app/static/"* "$DEST/app/static/"
install -m 0755 -o "$SERVICE_USER" -g "$SERVICE_GROUP" "$BUNDLE_ROOT/engine/run-phase6-streaming-session.sh" "$RUNNER_DEST"
cp -a "$BUNDLE_ROOT/engine/phase6-streaming" "$ENGINE_DEST"
chown -R "$SERVICE_USER:$SERVICE_GROUP" "$ENGINE_DEST"
find "$ENGINE_DEST" -type d -exec chmod 0750 {} +
find "$ENGINE_DEST" -type f -exec chmod 0640 {} +
for file in README.md LICENSE NOTICE THIRD_PARTY_NOTICES.md RELEASE_NOTES.md VERSION; do
  [[ -f $BUNDLE_ROOT/$file ]] && install -m 0644 -o "$SERVICE_USER" -g "$SERVICE_GROUP" "$BUNDLE_ROOT/$file" "$DEST/$file"
done
install -m 0644 -o "$SERVICE_USER" -g "$SERVICE_GROUP" "$BUNDLE_ROOT/docs/"* "$DEST/docs/"

# Grant render access without assuming every distribution has both groups.
supplementary=()
for group in video render; do
  if getent group "$group" >/dev/null; then
    usermod -a -G "$group" "$SERVICE_USER"
    supplementary+=("$group")
  fi
done

if [[ $ENCODER_MODE == vaapi ]]; then
  [[ -e $VAAPI_DEVICE ]] || {
    echo "VA-API device does not exist: $VAAPI_DEVICE" >&2
    echo "Use --software-encoder or specify --vaapi-device PATH." >&2
    exit 1
  }
fi

echo "Building the native MVC decoder/compositor and Blu-ray ISO adapter..."
rm -rf "$ENGINE_DEST/build"
runuser -u "$SERVICE_USER" -- cmake \
  -S "$ENGINE_DEST" -B "$ENGINE_DEST/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  >"$DEST/docs/engine-build.log" 2>&1 || {
    tail -n 120 "$DEST/docs/engine-build.log" >&2
    echo "CMake configuration failed. Previous files remain in $backup" >&2
    exit 1
  }
runuser -u "$SERVICE_USER" -- cmake --build "$ENGINE_DEST/build" --parallel "$(nproc)" \
  >>"$DEST/docs/engine-build.log" 2>&1 || {
    tail -n 160 "$DEST/docs/engine-build.log" >&2
    echo "Native build failed. Previous files remain in $backup" >&2
    exit 1
  }
for binary in sylc_hsbs_pipe sylc_iso_source; do
  [[ -x $ENGINE_DEST/build/$binary ]] || { echo "Built binary is missing: $binary" >&2; exit 1; }
done
runuser -u "$SERVICE_USER" -- ctest --test-dir "$ENGINE_DEST/build" --output-on-failure \
  >>"$DEST/docs/engine-build.log" 2>&1 || {
    tail -n 120 "$DEST/docs/engine-build.log" >&2
    echo "Native self-tests failed." >&2
    exit 1
  }

install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_GROUP" "$CONFIG_DIR"
if [[ ! -f $ENV_FILE ]]; then
  install -m 0640 -o root -g "$SERVICE_GROUP" "$BUNDLE_ROOT/config/sylc-mvc-stream.env.example" "$ENV_FILE"
else
  cp -a "$ENV_FILE" "${ENV_FILE}.bak-${stamp}"
fi
python3 - "$ENV_FILE" "$DEST" "$CONFIG_FILE" "$BIND_HOST" "$PORT" "$ENCODER_MODE" "$VAAPI_DEVICE" <<'PY'
from pathlib import Path
import sys
path, dest, config_file, bind_host, port, encoder_mode, vaapi_device = sys.argv[1:]
p = Path(path)
updates = {
    "SYLC_BIND_HOST": bind_host,
    "SYLC_PORT": port,
    "SYLC_CONFIG_FILE": config_file,
    "SYLC_STATE_ROOT": f"{dest}/state",
    "SYLC_ENGINE_PROJECT": f"{dest}/engine/phase6-streaming",
    "SYLC_ENGINE_RUNNER": f"{dest}/engine/run-phase6-streaming-session.sh",
    "SYLC_ENGINE_WRAPPER": f"{dest}/engine/run-phase6-streaming-session.sh",
    "SYLC_ENCODER_MODE": encoder_mode,
    "SYLC_VAAPI_DEVICE": vaapi_device,
    "SYLC_BROWSE_ROOTS": "/mnt|/media|/srv|/home",
    "SYLC_STARTUP_TIMEOUT_SECONDS": "60",
    "SYLC_STALL_TIMEOUT_SECONDS": "30",
    "SYLC_CLEANUP_INTERVAL_SECONDS": "3600",
    "SYLC_MINIMUM_FREE_GIB": "10",
    "SYLC_EMERGENCY_FREE_GIB": "5",
}
obsolete = {"SYLC_PHASE5_PROJECT", "SYLC_PHASE5_RUNNER", "SYLC_PHASE5_WRAPPER"}
lines = p.read_text(encoding="utf-8").splitlines() if p.exists() else []
out = []
seen = set()
for line in lines:
    stripped = line.strip()
    if not stripped or stripped.startswith("#") or "=" not in stripped:
        out.append(line)
        continue
    key = stripped.split("=", 1)[0].strip()
    if key in obsolete:
        continue
    if key in updates:
        if key not in seen:
            out.append(f"{key}={updates[key]}")
            seen.add(key)
        continue
    out.append(line)
out.append("")
out.append("# Managed defaults updated by the SyLC 0.7 installer")
for key, value in updates.items():
    if key not in seen:
        out.append(f"{key}={value}")
p.write_text("\n".join(out).rstrip() + "\n", encoding="utf-8")
PY
chown root:"$SERVICE_GROUP" "$ENV_FILE"
chmod 0640 "$ENV_FILE"

# Preserve existing runtime settings. New installs intentionally start without a
# config.json so the browser presents the first-run Media Libraries wizard.
if [[ -f $CONFIG_FILE ]]; then
  chown "$SERVICE_USER:$SERVICE_GROUP" "$CONFIG_FILE"
  chmod 0640 "$CONFIG_FILE"
fi

supplementary_line=
if ((${#supplementary[@]})); then
  supplementary_line="SupplementaryGroups=${supplementary[*]}"
fi
python3 - "$BUNDLE_ROOT/systemd/sylc-mvc-stream.service.in" "$UNIT_FILE" \
  "$SERVICE_USER" "$SERVICE_GROUP" "$supplementary_line" "$DEST" "$ENV_FILE" "$CONFIG_DIR" <<'PY'
from pathlib import Path
import sys
source, target, user, group, supplementary, dest, env_file, config_dir = sys.argv[1:]
text = Path(source).read_text(encoding="utf-8")
for key, value in {
    "@SERVICE_USER@": user,
    "@SERVICE_GROUP@": group,
    "@SUPPLEMENTARY_GROUPS@": supplementary,
    "@DEST@": dest,
    "@ENV_FILE@": env_file,
    "@CONFIG_DIR@": config_dir,
}.items():
    text = text.replace(key, value)
Path(target).write_text(text, encoding="utf-8")
PY
chmod 0644 "$UNIT_FILE"

runuser -u "$SERVICE_USER" -- test -x "$ENGINE_DEST/build/sylc_hsbs_pipe"
runuser -u "$SERVICE_USER" -- test -x "$ENGINE_DEST/build/sylc_iso_source"
if [[ $ENCODER_MODE == vaapi ]]; then
  runuser -u "$SERVICE_USER" -- test -r "$VAAPI_DEVICE" || {
    echo "The service user cannot read $VAAPI_DEVICE after group assignment." >&2
    echo "Log out/reboot if group membership has just changed, or use --software-encoder." >&2
    exit 1
  }
fi

systemctl daemon-reload
systemctl enable --now "$SERVICE_NAME"

health_host=$BIND_HOST
[[ $BIND_HOST == 0.0.0.0 ]] && health_host=127.0.0.1
[[ $BIND_HOST == :: ]] && health_host='[::1]'
for _ in $(seq 1 60); do
  if curl -fsS "http://${health_host}:${PORT}/api/health" >/dev/null 2>&1; then break; fi
  sleep .25
done
if ! curl -fsS "http://${health_host}:${PORT}/api/health" >/dev/null; then
  systemctl --no-pager --full status "$SERVICE_NAME" || true
  journalctl -u "$SERVICE_NAME" -n 80 --no-pager || true
  exit 1
fi

lan_ip=$(hostname -I 2>/dev/null | awk '{print $1}')
[[ -n $lan_ip ]] || lan_ip=$BIND_HOST
[[ $lan_ip == 0.0.0.0 ]] && lan_ip=127.0.0.1

echo
echo "SyLC MVC Stream Server 0.7.0-alpha.3 installed successfully."
echo "Open:   http://${lan_ip}:${PORT}"
echo "Health: curl -fsS http://${health_host}:${PORT}/api/health | python3 -m json.tool"
echo "Logs:   journalctl -u sylc-mvc-stream -f"
if [[ ! -f $CONFIG_FILE ]] && [[ -z $(read_env_value SYLC_MEDIA_ROOTS "$ENV_FILE") ]]; then
  echo
echo "Complete first-run setup in the web UI by choosing one or more media folders."
fi
echo "Jellyfin and Docker services were not changed."
