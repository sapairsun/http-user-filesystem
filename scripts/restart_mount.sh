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
HTTPFS_MOUNT_BIN="${HTTPFS_MOUNT_BIN:-$ROOT_DIR/build/httpfs_mount}"

usage() {
    echo "Usage: $0 <base_url> <mountpoint> [FUSE options...]" >&2
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

is_mountpoint() {
    local target="$1"

    if command_exists mountpoint; then
        mountpoint -q "$target"
        return $?
    fi

    python3 - "$target" <<'PY'
import os
import sys

target = os.path.realpath(sys.argv[1])

with open("/proc/self/mountinfo", "r", encoding="utf-8") as f:
    for line in f:
        parts = line.split()
        if len(parts) >= 5:
            mount_point = parts[4].replace("\\040", " ")
            if os.path.realpath(mount_point) == target:
                sys.exit(0)

sys.exit(1)
PY
}

cleanup_mountpoint() {
    local target="$1"

    if command_exists fusermount3; then
        fusermount3 -u "$target" >/dev/null 2>&1 || true
        fusermount3 -uz "$target" >/dev/null 2>&1 || true
    fi

    if is_mountpoint "$target" && command_exists fusermount; then
        fusermount -u "$target" >/dev/null 2>&1 || true
        fusermount -uz "$target" >/dev/null 2>&1 || true
    fi

    if is_mountpoint "$target" && command_exists umount; then
        umount "$target" >/dev/null 2>&1 || true
        umount -l "$target" >/dev/null 2>&1 || true
    fi

    if is_mountpoint "$target"; then
        echo "Failed to clean mountpoint: $target" >&2
        return 1
    fi

    return 0
}

if [[ $# -lt 2 ]]; then
    usage
    exit 1
fi

BASE_URL="$1"
MOUNTPOINT="$2"
shift 2

if [[ ! -x "$HTTPFS_MOUNT_BIN" ]]; then
    echo "httpfs_mount binary is not executable: $HTTPFS_MOUNT_BIN" >&2
    echo "Please build it first with: make httpfs_mount" >&2
    exit 1
fi

if [[ -e "$MOUNTPOINT" && ! -d "$MOUNTPOINT" ]]; then
    echo "Mountpoint exists but is not a directory: $MOUNTPOINT" >&2
    exit 1
fi

mkdir -p "$MOUNTPOINT"

if is_mountpoint "$MOUNTPOINT"; then
    if stat "$MOUNTPOINT" >/dev/null 2>&1; then
        echo "Mountpoint is already active and healthy: $MOUNTPOINT" >&2
        echo "Please unmount it first if you want to remount." >&2
        exit 1
    fi

    echo "Detected stale FUSE mountpoint, cleaning it up: $MOUNTPOINT"
    cleanup_mountpoint "$MOUNTPOINT"
fi

echo "Starting httpfs_mount on $MOUNTPOINT"
exec "$HTTPFS_MOUNT_BIN" "$BASE_URL" "$MOUNTPOINT" "$@"
