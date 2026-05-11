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

# Python Service Provider

This directory contains a Flask-based implementation of the HTTP protocol defined by this project.

## Run

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
python app.py --host 127.0.0.1 --port 18081 --root-dir /tmp/httpfs-backend
```

## Endpoints

The Flask service implements:

- `GET /v1/meta`
- `GET /v1/list`
- `GET /v1/read`
- `POST /v1/write`
- `POST /v1/create-file`
- `POST /v1/create-dir`
- `POST /v1/remove-file`
- `POST /v1/remove-dir`
- `POST /v1/rename`
- `POST /v1/truncate`
