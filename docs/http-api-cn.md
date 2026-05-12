# HTTP User FileSystem 协议定义

[English Version](./http-api.md)

## 总体约定

- 基础前缀：`/v1`
- 编码：请求和响应统一使用 `application/json; charset=utf-8`
- 路径：所有路径均为以 `/` 开头的绝对路径
- 时间：时间字段统一使用 Unix 秒时间戳
- 二进制：文件读写内容统一使用十六进制字符串 `data_hex`
- 成功响应：

```json
{
  "status": "ok"
}
```

- 失败响应：

```json
{
  "status": "error",
  "errno": 2,
  "message": "No such file or directory"
}
```

其中 `errno` 直接对应 POSIX `errno` 编号，客户端拿到后取负值返回给 FUSE。

## 元数据对象

`meta` 用于表达文件或目录属性：

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

说明：

- `type` 仅支持 `file` 与 `dir`
- `mode` 为完整 `st_mode` 数值，已经包含文件类型位

## 目录项对象

```json
{
  "name": "hello.txt",
  "type": "file"
}
```

## 接口清单

### 1. 查询属性

- 方法：`GET`
- 路径：`/v1/meta?path=/target`

成功响应：

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

### 2. 列出目录

- 方法：`GET`
- 路径：`/v1/list?path=/dir`

成功响应：

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

### 3. 读取文件

- 方法：`GET`
- 路径：`/v1/read?path=/file.txt&offset=0&size=4096`
- 成功响应 Content-Type：`application/octet-stream`
- 成功响应 Header：`X-HTTPFS-Content-MD5: <md5>`

成功响应：

- 响应体直接返回请求区间内读取到的原始二进制数据

说明：

- 客户端会自动把大读取拆成多个分片请求
- 服务端会按“绝对文件偏移为 `10` 的倍数”的字节位置（`0、10、20、30 ...`）采样并计算 MD5，通过 `X-HTTPFS-Content-MD5` 返回
- 客户端必须用同样的采样规则重算 MD5；如果不一致，读取直接报错
- 出错时仍然沿用现有 JSON 错误结构 `status/errno/message`

### 4. 写入文件

- 方法：`POST`
- 路径：`/v1/write?path=/file.txt&offset=0`
- Content-Type：`application/octet-stream`
- 必需 Header：`X-HTTPFS-Content-MD5: <md5>`

请求体：

- 直接上传目标写入范围对应的原始二进制数据

成功响应：

```json
{
  "status": "ok",
  "bytes_written": 6,
  "content_md5": "d41d8cd98f00b204e9800998ecf8427e"
}
```

说明：

- 客户端会自动把大写入拆成多个分片请求
- 请求体现在是原始字节流，不再使用 JSON 包装和十六进制编码
- 采样 MD5 的算法是：只取绝对文件偏移为 `10` 的倍数的位置字节（`0、10、20、30 ...`）拼接后计算 MD5
- 服务端必须先校验请求头里的 MD5 与上传数据是否一致，写入后还要再次从文件中读回并校验；任一步不一致都要报错

### 5. 创建文件

- 方法：`POST`
- 路径：`/v1/create-file`

请求体：

```json
{
  "path": "/new.txt",
  "mode": 420
}
```

### 6. 创建目录

- 方法：`POST`
- 路径：`/v1/create-dir`

请求体：

```json
{
  "path": "/data",
  "mode": 493
}
```

### 7. 删除文件

- 方法：`POST`
- 路径：`/v1/remove-file`

请求体：

```json
{
  "path": "/old.txt"
}
```

### 8. 删除目录

- 方法：`POST`
- 路径：`/v1/remove-dir`

请求体：

```json
{
  "path": "/old-dir"
}
```

### 9. 重命名

- 方法：`POST`
- 路径：`/v1/rename`

请求体：

```json
{
  "old_path": "/old.txt",
  "new_path": "/new.txt"
}
```

### 10. 截断文件

- 方法：`POST`
- 路径：`/v1/truncate`

请求体：

```json
{
  "path": "/file.txt",
  "size": 2
}
```

### 11. 更新时间

- 方法：`POST`
- 路径：`/v1/utimens`

请求体：

```json
{
  "path": "/file.txt",
  "atime_sec": 1710000100,
  "atime_nsec": 123456789,
  "mtime_sec": 1710000200,
  "mtime_nsec": 987654321
}
```

说明：

- `atime_*` 表示访问时间
- `mtime_*` 表示修改时间
- `*_nsec` 支持普通纳秒值，也支持 `UTIME_NOW` 与 `UTIME_OMIT`

## FUSE 操作映射

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

## 示例流程

### 创建并写入文件

1. `POST /v1/create-file` 创建 `/hello.txt`
2. `POST /v1/write` 写入 `hello\n`
3. `GET /v1/meta?path=/hello.txt` 获取大小
4. `GET /v1/read?path=/hello.txt&offset=0&size=4096` 读取内容

### 删除目录

1. `GET /v1/list?path=/demo` 确认为空
2. `POST /v1/remove-dir` 删除目录
