#!/usr/bin/env bash
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BACKEND_DIR="$ROOT_DIR/build/backend-root"
SERVER_LOG="$ROOT_DIR/build/httpfs-server.log"
SERVER_PID_FILE="$ROOT_DIR/build/httpfs-server.pid"
PORT="${HTTPFS_TEST_PORT:-18080}"

cleanup() {
    if [[ -f "$SERVER_PID_FILE" ]]; then
        kill "$(cat "$SERVER_PID_FILE")" >/dev/null 2>&1 || true
        rm -f "$SERVER_PID_FILE"
    fi
}

trap cleanup EXIT

mkdir -p "$BACKEND_DIR"
rm -rf "$BACKEND_DIR"/*

"$ROOT_DIR/build/test_json_utils"
"$ROOT_DIR/build/httpfs_service_provider_c" --host 127.0.0.1 --port "$PORT" --root-dir "$BACKEND_DIR" >"$SERVER_LOG" 2>&1 &
echo $! >"$SERVER_PID_FILE"

for _ in $(seq 1 20); do
    if bash -c "exec 3<>/dev/tcp/127.0.0.1/$PORT" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

HTTPFS_BASE_URL="http://127.0.0.1:$PORT" "$ROOT_DIR/build/test_http_client_integration"
HTTPFS_BASE_URL="http://127.0.0.1:$PORT/v1" "$ROOT_DIR/build/test_http_client_integration"

echo "All tests passed"
