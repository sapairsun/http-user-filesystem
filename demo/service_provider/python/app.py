#!/usr/bin/env python3
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

import argparse
import binascii
import errno
import hashlib
import os
import stat
import time
from typing import Any, Dict, List

from flask import Flask, Response, jsonify, request
from werkzeug.exceptions import HTTPException

app = Flask(__name__)

ROOT_DIR = ""
UTIME_NOW = getattr(os, "UTIME_NOW", 1073741823)
UTIME_OMIT = getattr(os, "UTIME_OMIT", 1073741822)
CONTENT_MD5_HEADER = "X-HTTPFS-Content-MD5"


def error_response(errno_value: int, message: str):
    return jsonify({
        "status": "error",
        "errno": int(errno_value),
        "message": message,
    })


def ok_response(payload: Dict[str, Any] | None = None):
    data: Dict[str, Any] = {"status": "ok"}
    if payload:
        data.update(payload)
    return jsonify(data)


def init_root(root_dir: str) -> None:
    global ROOT_DIR
    ROOT_DIR = os.path.abspath(root_dir)


def resolve_path(path: str) -> str:
    if not path.startswith("/"):
        raise OSError(errno.EINVAL, "path must start with '/'")

    if path == "/":
        return ROOT_DIR

    candidate = os.path.abspath(os.path.join(ROOT_DIR, path.lstrip("/")))
    if os.path.commonpath([ROOT_DIR, candidate]) != ROOT_DIR:
        raise OSError(errno.EINVAL, "path escapes backend root")
    return candidate


def meta_from_stat(path: str, st: os.stat_result) -> Dict[str, Any]:
    return {
        "path": path,
        "type": "dir" if stat.S_ISDIR(st.st_mode) else "file",
        "mode": int(st.st_mode),
        "size": int(st.st_size),
        "uid": int(st.st_uid),
        "gid": int(st.st_gid),
        "nlink": int(st.st_nlink),
        "atime": int(st.st_atime),
        "mtime": int(st.st_mtime),
        "ctime": int(st.st_ctime),
    }


def entry_type(local_path: str) -> str:
    return "dir" if os.path.isdir(local_path) else "file"


def decode_hex(data_hex: str) -> bytes:
    try:
        return binascii.unhexlify(data_hex.encode("ascii"))
    except (binascii.Error, ValueError) as exc:
        raise OSError(errno.EINVAL, f"invalid hex payload: {exc}") from exc


def compute_sparse_md5_hex(data: bytes, base_offset: int) -> str:
    sampled = bytearray()
    for index, value in enumerate(data):
        if (base_offset + index) % 10 == 0:
            sampled.append(value)
    return hashlib.md5(sampled).hexdigest()


def validate_content_md5(value: str) -> bool:
    return len(value) == 32 and all(ch in "0123456789abcdef" for ch in value)


def normalize_utimens_value(current_ns: int, sec: int, nsec: int) -> int:
    if nsec == UTIME_NOW:
        return time.time_ns()
    if nsec == UTIME_OMIT:
        return current_ns
    if nsec < 0 or nsec >= 1_000_000_000:
        raise OSError(errno.EINVAL, "nanoseconds must be in [0, 1000000000)")
    return sec * 1_000_000_000 + nsec


@app.errorhandler(OSError)
def handle_os_error(exc: OSError):
    errno_value = exc.errno if exc.errno is not None else errno.EIO
    return error_response(errno_value, str(exc))


@app.errorhandler(KeyError)
@app.errorhandler(TypeError)
@app.errorhandler(ValueError)
def handle_input_error(exc: Exception):
    return error_response(errno.EINVAL, str(exc))


@app.errorhandler(HTTPException)
def handle_http_error(exc: HTTPException):
    return error_response(exc.code or errno.EIO, exc.description)


@app.get("/v1/meta")
def get_meta():
    path = request.args.get("path", "")
    local_path = resolve_path(path)
    st = os.lstat(local_path)
    return ok_response({"meta": meta_from_stat(path, st)})


@app.get("/v1/list")
def list_dir():
    path = request.args.get("path", "")
    local_path = resolve_path(path)
    entries: List[Dict[str, str]] = []

    entries.append({"name": ".", "type": "dir"})
    entries.append({"name": "..", "type": "dir"})
    for name in sorted(os.listdir(local_path)):
        entry_local_path = os.path.join(local_path, name)
        entries.append({"name": name, "type": entry_type(entry_local_path)})

    return ok_response({"entries": entries})


@app.get("/v1/read")
def read_file():
    path = request.args.get("path", "")
    offset = int(request.args.get("offset", "0"))
    size = int(request.args.get("size", "0"))
    local_path = resolve_path(path)

    if offset < 0 or size < 0:
        raise OSError(errno.EINVAL, "offset and size must be non-negative")

    with open(local_path, "rb") as handle:
        handle.seek(offset)
        data = handle.read(size)

    response = Response(data, mimetype="application/octet-stream")
    response.headers[CONTENT_MD5_HEADER] = compute_sparse_md5_hex(data, offset)
    return response


@app.post("/v1/write")
def write_file():
    path = request.args.get("path", "")
    offset = int(request.args.get("offset", "0"))
    data = request.get_data(cache=False, as_text=False)
    expected_md5 = request.headers.get(CONTENT_MD5_HEADER, "")
    local_path = resolve_path(path)

    if offset < 0:
        raise OSError(errno.EINVAL, "offset must be non-negative")
    if not validate_content_md5(expected_md5):
        raise OSError(errno.EINVAL, "missing or invalid content md5")
    if compute_sparse_md5_hex(data, offset) != expected_md5:
        raise OSError(errno.EIO, "write content md5 verification failed")

    with open(local_path, "r+b") as handle:
        handle.seek(offset)
        written = handle.write(data)
        handle.flush()
        handle.seek(offset)
        stored = handle.read(len(data))

    if written != len(data):
        raise OSError(errno.EIO, "short write")
    actual_md5 = compute_sparse_md5_hex(stored, offset)
    if actual_md5 != expected_md5:
        raise OSError(errno.EIO, "stored content md5 verification failed")

    return ok_response({"bytes_written": written, "content_md5": actual_md5})


@app.post("/v1/create-file")
def create_file():
    payload = request.get_json(force=True, silent=False)
    path = payload["path"]
    mode = int(payload["mode"])
    local_path = resolve_path(path)

    fd = os.open(local_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, mode)
    os.close(fd)
    return ok_response()


@app.post("/v1/create-dir")
def create_dir():
    payload = request.get_json(force=True, silent=False)
    path = payload["path"]
    mode = int(payload["mode"])
    local_path = resolve_path(path)

    os.mkdir(local_path, mode)
    return ok_response()


@app.post("/v1/remove-file")
def remove_file():
    payload = request.get_json(force=True, silent=False)
    path = payload["path"]
    local_path = resolve_path(path)

    os.unlink(local_path)
    return ok_response()


@app.post("/v1/remove-dir")
def remove_dir():
    payload = request.get_json(force=True, silent=False)
    path = payload["path"]
    local_path = resolve_path(path)

    os.rmdir(local_path)
    return ok_response()


@app.post("/v1/rename")
def rename_path():
    payload = request.get_json(force=True, silent=False)
    old_path = payload["old_path"]
    new_path = payload["new_path"]
    old_local = resolve_path(old_path)
    new_local = resolve_path(new_path)

    os.rename(old_local, new_local)
    return ok_response()


@app.post("/v1/truncate")
def truncate_file():
    payload = request.get_json(force=True, silent=False)
    path = payload["path"]
    size = int(payload["size"])
    local_path = resolve_path(path)

    os.truncate(local_path, size)
    return ok_response()


@app.post("/v1/utimens")
def update_times():
    payload = request.get_json(force=True, silent=False)
    path = payload["path"]
    atime_sec = int(payload["atime_sec"])
    atime_nsec = int(payload["atime_nsec"])
    mtime_sec = int(payload["mtime_sec"])
    mtime_nsec = int(payload["mtime_nsec"])
    local_path = resolve_path(path)
    st = os.stat(local_path, follow_symlinks=False)

    atime_ns = normalize_utimens_value(st.st_atime_ns, atime_sec, atime_nsec)
    mtime_ns = normalize_utimens_value(st.st_mtime_ns, mtime_sec, mtime_nsec)
    os.utime(local_path, ns=(atime_ns, mtime_ns), follow_symlinks=False)
    return ok_response()


def main() -> int:
    parser = argparse.ArgumentParser(description="Flask HTTP service provider for the HTTP FUSE protocol")
    parser.add_argument("--port", type=int, required=True, help="listen port")
    parser.add_argument("--root-dir", required=True, help="backend root directory")
    parser.add_argument("--host", default="127.0.0.1", help="listen host")
    args = parser.parse_args()

    init_root(args.root_dir)
    app.run(host=args.host, port=args.port, debug=False, use_reloader=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
