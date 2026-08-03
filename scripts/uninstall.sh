#!/usr/bin/env bash
set -Eeuo pipefail
DEST=${DEST:-/srv/sylc-mvc-stream}
ENV_FILE=${ENV_FILE:-/etc/sylc-mvc-stream.env}
CONFIG_DIR=${CONFIG_DIR:-/var/lib/sylc-mvc-stream}
UNIT_FILE=${UNIT_FILE:-/etc/systemd/system/sylc-mvc-stream.service}
[[ $EUID -eq 0 ]] || { echo "Run with sudo." >&2; exit 1; }
systemctl disable --now sylc-mvc-stream.service 2>/dev/null || true
rm -f "$UNIT_FILE"
systemctl daemon-reload
cat <<EOF
SyLC service stopped and its systemd unit removed.

The following were deliberately retained:
  Application and session reports: $DEST
  Machine-level environment:        $ENV_FILE
  Media Libraries/API settings:     $CONFIG_DIR

Remove those manually only after preserving any reports or settings you need.
Source movie files were not changed.
EOF
