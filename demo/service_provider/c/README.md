<!--
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# C Service Provider

This directory contains the C entry point for the HTTP protocol service provider.
It also contains the C implementation of the service provider protocol handlers.

## Build

From the project root:

```bash
make build/httpfs_service_provider_c
```

## Run

```bash
./build/httpfs_service_provider_c --host 127.0.0.1 --port 18080 --root-dir /tmp/httpfs-backend
```

The protocol implementation lives in `demo/service_provider/c/httpfs_server.c`.
