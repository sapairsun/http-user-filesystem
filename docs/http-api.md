# HTTP User FileSystem Protocol Definition

[中文版本](./http-api-cn.md)

## General Rules

- Base prefix: `/v1`
- Encoding: both requests and responses use `application/json; charset=utf-8`
- Paths: all paths are absolute and must start with `/`
- Time: all time fields use Unix timestamps in seconds
- Binary data: file contents are transferred as hexadecimal strings in `data_hex`
- Success response:

```json
{
  "status": "ok"
}
```

- Error response:

```json
{
  "status": "error",
  "errno": 2,
  "message": "No such file or directory"
}
```

The `errno` field maps directly to POSIX `errno` values. The client returns the negative value back to FUSE.

## Metadata Object

`meta` describes a file or directory:

```json
{
  "path": "/hello.txt",
  "type": "file",
  "mode": 33188,
  "size": 12,
  "uid": 1000,
  "gid": 1000,
  "nlink": 1,
  "atime": 1710000000,
  "mtime": 1710000001,
  "ctime": 1710000002
}
```

Notes:

- `type` only supports `file` and `dir`
- `mode` is the full `st_mode` value including file type bits

## Directory Entry Object

```json
{
  "name": "hello.txt",
  "type": "file"
}
```

## API List

### 1. Query Metadata

- Method: `GET`
- Path: `/v1/meta?path=/target`

Success response:

```json
{
  "status": "ok",
  "meta": {
    "path": "/target",
    "type": "dir",
    "mode": 16877,
    "size": 0,
    "uid": 1000,
    "gid": 1000,
    "nlink": 2,
    "atime": 1710000000,
    "mtime": 1710000001,
    "ctime": 1710000002
  }
}
```

### 2. List Directory

- Method: `GET`
- Path: `/v1/list?path=/dir`

Success response:

```json
{
  "status": "ok",
  "entries": [
    {
      "name": ".",
      "type": "dir"
    },
    {
      "name": "..",
      "type": "dir"
    },
    {
      "name": "hello.txt",
      "type": "file"
    }
  ]
}
```

### 3. Read File

- Method: `GET`
- Path: `/v1/read?path=/file.txt&offset=0&size=4096`
- Success Content-Type: `application/octet-stream`
- Success Header: `X-HTTPFS-Content-MD5: <md5>`

Success response:

- The response body is the raw binary data read from the requested range

Notes:

- The client automatically splits large reads into multiple requests
- The server computes sampled MD5 from bytes whose absolute file offsets are multiples of `10` (`0, 10, 20, 30, ...`) and returns it in `X-HTTPFS-Content-MD5`
- The client must recompute the same sampled MD5 locally and fail the read if it does not match
- Error responses remain JSON payloads with the existing `status/errno/message` structure

### 4. Write File

- Method: `POST`
- Path: `/v1/write?path=/file.txt&offset=0`
- Content-Type: `application/octet-stream`
- Required Header: `X-HTTPFS-Content-MD5: <md5>`

Request body:

- Raw binary file content for the requested write range

Success response:

```json
{
  "status": "ok",
  "bytes_written": 6,
  "content_md5": "d41d8cd98f00b204e9800998ecf8427e"
}
```

Notes:

- The client automatically splits large writes into multiple requests
- The request body is raw bytes and is no longer wrapped in JSON or encoded as hex
- The sampled MD5 is computed from bytes whose absolute file offsets are multiples of `10` (`0, 10, 20, 30, ...`)
- The server must verify the request header MD5 against the uploaded chunk and verify the stored bytes again after writing; any mismatch must return an error

### 5. Create File

- Method: `POST`
- Path: `/v1/create-file`

Request body:

```json
{
  "path": "/new.txt",
  "mode": 420
}
```

### 6. Create Directory

- Method: `POST`
- Path: `/v1/create-dir`

Request body:

```json
{
  "path": "/data",
  "mode": 493
}
```

### 7. Remove File

- Method: `POST`
- Path: `/v1/remove-file`

Request body:

```json
{
  "path": "/old.txt"
}
```

### 8. Remove Directory

- Method: `POST`
- Path: `/v1/remove-dir`

Request body:

```json
{
  "path": "/old-dir"
}
```

### 9. Rename

- Method: `POST`
- Path: `/v1/rename`

Request body:

```json
{
  "old_path": "/old.txt",
  "new_path": "/new.txt"
}
```

### 10. Truncate File

- Method: `POST`
- Path: `/v1/truncate`

Request body:

```json
{
  "path": "/file.txt",
  "size": 2
}
```

### 11. Update Timestamps

- Method: `POST`
- Path: `/v1/utimens`

Request body:

```json
{
  "path": "/file.txt",
  "atime_sec": 1710000100,
  "atime_nsec": 123456789,
  "mtime_sec": 1710000200,
  "mtime_nsec": 987654321
}
```

Notes:

- `atime_*` represents access time
- `mtime_*` represents modification time
- `*_nsec` supports both normal nanosecond values and the special values `UTIME_NOW` and `UTIME_OMIT`

## FUSE Operation Mapping

- `getattr` -> `GET /v1/meta`
- `readdir` -> `GET /v1/list`
- `read` -> `GET /v1/read`
- `write` -> `POST /v1/write`
- `create` -> `POST /v1/create-file`
- `mkdir` -> `POST /v1/create-dir`
- `unlink` -> `POST /v1/remove-file`
- `rmdir` -> `POST /v1/remove-dir`
- `rename` -> `POST /v1/rename`
- `truncate` -> `POST /v1/truncate`
- `utimens` -> `POST /v1/utimens`

## Example Flows

### Create And Write A File

1. `POST /v1/create-file` creates `/hello.txt`
2. `POST /v1/write` writes `hello\n`
3. `GET /v1/meta?path=/hello.txt` reads the file size
4. `GET /v1/read?path=/hello.txt&offset=0&size=4096` reads the file content

### Remove A Directory

1. `GET /v1/list?path=/demo` confirms the directory is empty
2. `POST /v1/remove-dir` removes the directory
