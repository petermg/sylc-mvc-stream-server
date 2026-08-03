#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

python3 -m py_compile app/server.py
python3 -m unittest -v tests.test_service tests.test_public_alpha
bash tests/http_integration_test.sh
bash tests/setup_auth_integration_test.sh
python3 tests/iso_logic_smoke.py
bash -n scripts/install.sh scripts/uninstall.sh engine/run-phase6-streaming-session.sh
if command -v node >/dev/null 2>&1; then
  node --check app/static/app.js
fi

BUILD=$(mktemp -d -t sylc-native-tests-XXXXXX)
trap 'rm -rf "$BUILD"' EXIT
cmake -S engine/phase6-streaming -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel "$(nproc)"
ctest --test-dir "$BUILD" --output-on-failure

echo "All SyLC public-alpha tests: PASS"
