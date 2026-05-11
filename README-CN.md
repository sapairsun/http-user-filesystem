# HTTP 用户态 文件系统

[English Version](./README.md)

这是一个基于 C 语言实现的 HTTP 用户态 文件系统示例项目。用户态文件系统 侧通过 HTTP/JSON 调用后端服务提供者，把常见文件操作映射到远端目录。这个在开发AI软件的时候非常有用。

## 最新功能

- 支持 `getattr`、`readdir`、`read`、`write`、`create`、`mkdir`、`unlink`、`rmdir`、`rename`、`truncate`
- 新增 `utimens` 支持，`touch`、更新时间戳等操作不再报 `Function not implemented`
- 支持三套服务提供者实现：
  - `demo/service_provider/c/`
  - `demo/service_provider/python/`
  - `demo/service_provider/go/`
- C 服务提供者启动参数已统一为 `--host --port --root-dir`
- `httpfs_mount` 同时支持：
  - `http://127.0.0.1:18080`
  - `http://127.0.0.1:18080/v1`
- 新增 `scripts/restart_mount.sh`，可以在重启挂载前自动清理坏掉的旧 FUSE 挂载点
- 提供本地构建和 `Ubuntu 24.04 Docker` 全量验证

## 目录说明

- `docs/http-api.md`：HTTP 接口与 JSON 协议定义
- `include/`：公共头文件
- `src/http_client.c`：HTTP 客户端与 JSON 解析
- `src/httpfs_fuse.c`：FUSE 操作实现
- `src/httpfs_mount_main.c`：FUSE 挂载入口
- `demo/service_provider/c/`：C 版服务提供者
- `demo/service_provider/python/`：Flask 版服务提供者
- `demo/service_provider/go/`：Go 版服务提供者
- `tests/`：单元测试与集成测试
- `scripts/run_tests.sh`：主测试脚本
- `scripts/run_provider_integration.sh`：服务提供者验证脚本
- `scripts/restart_mount.sh`：自动清理坏挂载并重启挂载

## 构建说明

### 依赖

- `make`
- `pkg-config`
- `libcurl` 开发包
- 可选：`fuse3` 或 `fuse` 开发包

### 本地构建

```bash
make all
make test
make service_providers-test
```

说明：

- 如果本地安装了 `fuse3` 或 `fuse` 开发包，`make all` 会自动构建 `build/httpfs_mount`
- 如果没有安装 FUSE 开发包，`make all` 会先构建其他目标，然后提示安装 `fuse3`
- 单独构建挂载程序可执行：

```bash
make httpfs_mount
```

### Debian 10

Debian 10 常见的是 `fuse2` 开发包，项目会自动优先选择 `fuse3`，找不到时回退到 `fuse`。

使用 `fuse2`：

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libcurl4-openssl-dev libfuse-dev
make httpfs_mount
```

使用 `fuse3`：

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libcurl4-openssl-dev libfuse3-dev
make httpfs_mount
```

### Docker 构建与测试

```bash
make docker-test
```

该目标会在 `Ubuntu 24.04` 容器中执行：

- `make all`
- `make test`
- `make service_providers-test`

## 服务提供者

### 启动 C 服务提供者

```bash
mkdir -p /tmp/httpfs-backend
./build/httpfs_service_provider_c --host 127.0.0.1 --port 18080 --root-dir /tmp/httpfs-backend
```

### 启动 Python 服务提供者

```bash
python3 -m venv build/python-service_provider-venv
build/python-service_provider-venv/bin/pip install -r demo/service_provider/python/requirements.txt
build/python-service_provider-venv/bin/python demo/service_provider/python/app.py --host 127.0.0.1 --port 18081 --root-dir /tmp/httpfs-backend
```

### 启动 Go 服务提供者

```bash
make build/httpfs_service_provider_go
./build/httpfs_service_provider_go --host 127.0.0.1 --port 18083 --root-dir /tmp/httpfs-backend
```

### 说明

- Python 和 Go 服务提供者可以在更多环境中直接运行
- C 服务提供者的网络层已经改成 Linux 下的 `epoll + 多线程` 架构，因此只支持 Linux
- 在 macOS 上如果要验证完整链路，推荐直接使用 Docker

## 挂载 FUSE 文件系统

先构建：

```bash
make httpfs_mount
```

直接启动：

```bash
mkdir -p /tmp/httpfs-mnt
./build/httpfs_mount http://127.0.0.1:18080 /tmp/httpfs-mnt -f
```

推荐使用自动恢复脚本：

```bash
mkdir -p /tmp/httpfs-mnt
./scripts/restart_mount.sh http://127.0.0.1:18080 /tmp/httpfs-mnt -f
```

说明：

- 第一个参数是 HTTP 服务端地址
- 支持 `http://127.0.0.1:18080` 和 `http://127.0.0.1:18080/v1` 两种写法
- 第二个参数是挂载点
- 后续参数会原样传递给 FUSE
- 如果挂载点残留了断开的旧 FUSE 会话，`scripts/restart_mount.sh` 会自动清理后再启动

## 使用示例

挂载成功后可以直接使用普通文件命令：

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

## 自动测试说明

当前包含以下验证：

- `tests/test_json_utils.c`：校验 JSON 转义和十六进制编解码
- `tests/test_http_client_integration.c`：校验创建、读取、写入、重命名、截断、更新时间、删除流程
- `make test`：启动 C 服务提供者并验证 `http://127.0.0.1:$PORT` 与 `http://127.0.0.1:$PORT/v1` 两种 base URL
- `make service_providers-test`：依次验证 C、Python、Go 三套服务提供者

## 常见异常与解决办法

### 1. `fuse3 development package was not found`

原因：

- 当前机器没有安装 FUSE 开发包

解决办法：

```bash
sudo apt-get update
sudo apt-get install -y libfuse3-dev
make all
```

Debian 10 也可以安装：

```bash
sudo apt-get install -y libfuse-dev
```

### 2. `fatal error: 'curl/curl.h' file not found`

原因：

- 只安装了运行库，没有安装 `libcurl` 开发头文件

解决办法：

```bash
sudo apt-get update
sudo apt-get install -y libcurl4-openssl-dev
```

### 3. `fatal error: 'sys/epoll.h' file not found`

原因：

- 你正在非 Linux 环境编译 C 服务提供者

解决办法：

- 在 Linux 上构建 C 服务提供者
- 或直接使用 Docker：

```bash
make docker-test
```

### 4. `touch: cannot touch ...: Function not implemented`

原因：

- 通常是旧版本的 `httpfs_mount` 还在运行，FUSE 操作表里还没有新的 `utimens` 支持

解决办法：

```bash
make all
./scripts/restart_mount.sh http://127.0.0.1:18080/v1 /tmp/httpfs-mnt -f
```

说明：

- 只重启后端服务不够，还需要重启 `httpfs_mount`

### 5. `unknown endpoint`

原因：

- 常见于请求路径拼错，或使用了旧版本程序

当前兼容行为：

- `httpfs_mount` 现在兼容：
  - `http://127.0.0.1:18080`
  - `http://127.0.0.1:18080/v1`

建议：

- 优先使用最新编译结果
- 确认服务提供者和 `httpfs_mount` 来自同一份构建

### 6. `Transport endpoint is not connected`

原因：

- 旧的 FUSE 挂载点已经坏掉，但还残留在挂载点上

推荐解决办法：

```bash
./scripts/restart_mount.sh http://127.0.0.1:18080/v1 /tmp/httpfs-mnt -f
```

如果你要手动处理：

```bash
fusermount3 -u /tmp/httpfs-mnt
fusermount3 -uz /tmp/httpfs-mnt
umount -l /tmp/httpfs-mnt
```

### 7. 挂载点已经正常工作，但重启脚本拒绝继续

现象：

- `Mountpoint is already active and healthy`

原因：

- 当前挂载点仍然是正常挂载，脚本为了避免误卸载正在工作的实例，会直接退出

解决办法：

- 先手动卸载当前正常挂载，再重新启动

## 协议说明

完整协议定义见：

- `docs/http-api.md`

