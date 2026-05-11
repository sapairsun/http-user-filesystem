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

if [[ $# -lt 4 ]]; then
    echo "Usage: $0 <label> <port> <command> [args...]" >&2
    exit 1
fi

LABEL="$1"
PORT="$2"
shift 2
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BACKEND_DIR="$ROOT_DIR/build/${LABEL}-backend"
SERVER_LOG="$ROOT_DIR/build/${LABEL}.log"
SERVER_PID_FILE="$ROOT_DIR/build/${LABEL}.pid"
COMMAND=()

if [[ "$PORT" == "0" || "$PORT" == "auto" ]]; then
    PORT="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"
fi

cleanup() {
    if [[ -f "$SERVER_PID_FILE" ]]; then
        kill "$(cat "$SERVER_PID_FILE")" >/dev/null 2>&1 || true
        wait "$(cat "$SERVER_PID_FILE")" 2>/dev/null || true
        rm -f "$SERVER_PID_FILE"
    fi
}

trap cleanup EXIT

mkdir -p "$BACKEND_DIR"
rm -rf "$BACKEND_DIR"/*

export PORT
export BACKEND_DIR
export ROOT_DIR

for arg in "$@"; do
    case "$arg" in
        __PORT__)
            COMMAND+=("$PORT")
            ;;
        __BACKEND_DIR__)
            COMMAND+=("$BACKEND_DIR")
            ;;
        __ROOT_DIR__)
            COMMAND+=("$ROOT_DIR")
            ;;
        *)
            COMMAND+=("$arg")
            ;;
    esac
done

"${COMMAND[@]}" >"$SERVER_LOG" 2>&1 &
echo $! >"$SERVER_PID_FILE"

server_ready=0
for _ in $(seq 1 50); do
    if bash -lc "exec 3<>/dev/tcp/127.0.0.1/$PORT" >/dev/null 2>&1; then
        server_ready=1
        break
    fi
    sleep 0.2
done

if [[ "$server_ready" -ne 1 ]]; then
    echo "Provider failed to start: $LABEL" >&2
    if [[ -f "$SERVER_LOG" ]]; then
        cat "$SERVER_LOG" >&2
    fi
    exit 1
fi

HTTPFS_BASE_URL="http://127.0.0.1:$PORT" "$ROOT_DIR/build/test_http_client_integration"
echo "Provider validation passed: $LABEL"
