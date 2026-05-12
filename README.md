# HTTP User-Space File System

[中文版本](./README-CN.md)

This project is an HTTP user-space file system example implemented in C. The user-space file system side talks to backend service providers over HTTP using JSON metadata APIs plus raw binary upload for file writes, and maps common file operations to a remote directory. This is very useful when developing AI software.

## Latest Features

- Supports `getattr`, `readdir`, `read`, `write`, `create`, `mkdir`, `unlink`, `rmdir`, `rename`, and `truncate`
- Adds `utimens` support so `touch` and timestamp updates no longer fail with `Function not implemented`
- Switches `write` to raw binary upload and `read` to raw binary download, with automatic client-side chunking for large files
- Enforces runtime integrity verification for every upload and download chunk using sampled MD5 over bytes at absolute file offsets `0, 10, 20, 30, ...`
- Provides three service provider implementations:
  - `demo/service_provider/c/`
  - `demo/service_provider/python/`
  - `demo/service_provider/go/`
- Unifies C service provider startup arguments as `--host --port --root-dir`
- Supports both of these `httpfs_mount` base URL forms:
  - `http://127.0.0.1:18080`
  - `http://127.0.0.1:18080/v1`
- Adds `scripts/restart_mount.sh` to automatically clean up stale FUSE mountpoints before remounting
- Includes local builds and full verification in `Ubuntu 24.04 Docker`

## Directory Layout

- `docs/http-api.md`: HTTP API and protocol definition
- `include/`: shared header files
- `src/http_client.c`: HTTP client and JSON parsing
- `src/httpfs_fuse.c`: FUSE operation implementation
- `src/httpfs_mount_main.c`: FUSE mount entry
- `demo/service_provider/c/`: C service provider
- `demo/service_provider/python/`: Flask service provider
- `demo/service_provider/go/`: Go service provider
- `tests/`: unit and integration tests
- `scripts/run_tests.sh`: main test script
- `scripts/run_provider_integration.sh`: service provider validation script
- `scripts/restart_mount.sh`: cleans stale mountpoints and restarts the mount

## Build

### Dependencies

- `make`
- `pkg-config`
- `libcurl` development package
- Optional: `fuse3` or `fuse` development package

### Local Build

```bash
make all
make test
make service_providers-test
```

Notes:

- If `fuse3` or `fuse` development headers are installed, `make all` also builds `build/httpfs_mount`
- If no FUSE development package is installed, `make all` still builds the other targets and then prints a message asking you to install `fuse3`
- To build the mount binary only:

```bash
make httpfs_mount
```

### Debian 10

Debian 10 commonly ships the `fuse2` development package. The project automatically prefers `fuse3` and falls back to `fuse` if `fuse3` is not available.

Using `fuse2`:

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libcurl4-openssl-dev libfuse-dev
make httpfs_mount
```

Using `fuse3`:

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libcurl4-openssl-dev libfuse3-dev
make httpfs_mount
```

### Docker Build And Test

```bash
make docker-test
```

This target runs the following steps inside an `Ubuntu 24.04` container:

- `make all`
- `make test`
- `make service_providers-test`

## Service Providers

### Start The C Service Provider

```bash
mkdir -p /tmp/httpfs-backend
./build/httpfs_service_provider_c --host 127.0.0.1 --port 18080 --root-dir /tmp/httpfs-backend
```

### Start The Python Service Provider

```bash
python3 -m venv build/python-service_provider-venv
build/python-service_provider-venv/bin/pip install -r demo/service_provider/python/requirements.txt
build/python-service_provider-venv/bin/python demo/service_provider/python/app.py --host 127.0.0.1 --port 18081 --root-dir /tmp/httpfs-backend
```

### Start The Go Service Provider

```bash
make build/httpfs_service_provider_go
./build/httpfs_service_provider_go --host 127.0.0.1 --port 18083 --root-dir /tmp/httpfs-backend
```

### Notes

- Python and Go service providers can run in more environments directly
- The C service provider uses a Linux-only `epoll + multithreading` network architecture
- On macOS, Docker is the recommended way to verify the full workflow

## Mount The FUSE File System

Build first:

```bash
make httpfs_mount
```

Start directly:

```bash
mkdir -p /tmp/httpfs-mnt
./build/httpfs_mount http://127.0.0.1:18080 /tmp/httpfs-mnt -f
```

Recommended remount script:

```bash
mkdir -p /tmp/httpfs-mnt
./scripts/restart_mount.sh http://127.0.0.1:18080 /tmp/httpfs-mnt -f
```

Notes:

- The first argument is the HTTP service URL
- Both `http://127.0.0.1:18080` and `http://127.0.0.1:18080/v1` are supported
- The second argument is the mountpoint
- Remaining arguments are passed through to FUSE unchanged
- If the mountpoint still holds a broken old FUSE session, `scripts/restart_mount.sh` cleans it up before starting again

## Usage Example

After mounting successfully, you can use normal file commands:

```bash
echo "hello" > /tmp/httpfs-mnt/demo.txt
cat /tmp/httpfs-mnt/demo.txt
mkdir /tmp/httpfs-mnt/dir1
mv /tmp/httpfs-mnt/demo.txt /tmp/httpfs-mnt/dir1/demo.txt
truncate -s 2 /tmp/httpfs-mnt/dir1/demo.txt
touch /tmp/httpfs-mnt/dir1/demo.txt
rm /tmp/httpfs-mnt/dir1/demo.txt
rmdir /tmp/httpfs-mnt/dir1
```

## Automated Tests

The project currently includes:

- `tests/test_json_utils.c`: validates JSON escaping and hex encoding/decoding
- `tests/test_http_client_integration.c`: validates create, read, write, rename, truncate, timestamp update, and delete flows
- `make test`: starts the C service provider and validates both `http://127.0.0.1:$PORT` and `http://127.0.0.1:$PORT/v1`
- `make service_providers-test`: validates the C, Python, and Go service providers in sequence

## Common Issues And Fixes

### 1. `fuse3 development package was not found`

Cause:

- No FUSE development package is installed on the machine

Fix:

```bash
sudo apt-get update
sudo apt-get install -y libfuse3-dev
make all
```

On Debian 10 you can also install:

```bash
sudo apt-get install -y libfuse-dev
```

### 2. `fatal error: 'curl/curl.h' file not found`

Cause:

- Only the runtime library is installed, but the `libcurl` development headers are missing

Fix:

```bash
sudo apt-get update
sudo apt-get install -y libcurl4-openssl-dev
```

### 3. `fatal error: 'sys/epoll.h' file not found`

Cause:

- You are trying to build the C service provider on a non-Linux system

Fix:

- Build the C service provider on Linux
- Or use Docker directly:

```bash
make docker-test
```

### 4. `touch: cannot touch ...: Function not implemented`

Cause:

- An old `httpfs_mount` process is still running and its FUSE operation table does not include the newer `utimens` support

Fix:

```bash
make all
./scripts/restart_mount.sh http://127.0.0.1:18080/v1 /tmp/httpfs-mnt -f
```

Note:

- Restarting only the backend service is not enough; you also need to restart `httpfs_mount`

### 5. `unknown endpoint`

Cause:

- The request path is wrong, or one side is still using an older binary

Current compatibility behavior:

- `httpfs_mount` supports:
  - `http://127.0.0.1:18080`
  - `http://127.0.0.1:18080/v1`

Recommendation:

- Prefer the latest build output
- Make sure the service provider and `httpfs_mount` come from the same build

### 6. `Transport endpoint is not connected`

Cause:

- The old FUSE mountpoint is broken but still mounted

Recommended fix:

```bash
./scripts/restart_mount.sh http://127.0.0.1:18080/v1 /tmp/httpfs-mnt -f
```

If you want to handle it manually:

```bash
fusermount3 -u /tmp/httpfs-mnt
fusermount3 -uz /tmp/httpfs-mnt
umount -l /tmp/httpfs-mnt
```

### 7. The remount script refuses to continue even though the mountpoint exists

Symptom:

- `Mountpoint is already active and healthy`

Cause:

- The mountpoint is still healthy and mounted. The script exits intentionally to avoid unmounting a live instance by mistake.

Fix:

- Unmount the healthy mountpoint manually first, then start again

## Protocol

For the full protocol definition, see:

- `docs/http-api.md`
