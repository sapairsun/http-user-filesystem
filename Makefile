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

CC ?= cc
CSTD ?= -std=c11
WARN ?= -Wall -Wextra -Wpedantic
OPT ?= -O2
THREAD_FLAGS := -pthread
CPPFLAGS += -Iinclude -Ithird_party
CFLAGS += $(CSTD) $(WARN) $(OPT) $(THREAD_FLAGS)
LDFLAGS += $(THREAD_FLAGS)

LIBCURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
LIBCURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
FUSE_PKG := $(shell if pkg-config --exists fuse3; then echo fuse3; elif pkg-config --exists fuse; then echo fuse; fi)
FUSE_CFLAGS := $(shell if [ -n "$(FUSE_PKG)" ]; then pkg-config --cflags $(FUSE_PKG) 2>/dev/null; fi)
FUSE_LIBS := $(shell if [ -n "$(FUSE_PKG)" ]; then pkg-config --libs $(FUSE_PKG) 2>/dev/null; fi)
HAS_FUSE := $(shell if [ -n "$(FUSE_PKG)" ]; then echo 1; else echo 0; fi)

CPPFLAGS += $(LIBCURL_CFLAGS)

COMMON_SRCS := src/json_utils.c src/hash_utils.c src/http_client.c
C_SERVICE_PROVIDER_SRCS := src/json_utils.c src/hash_utils.c demo/service_provider/c/httpfs_server.c
PYTHON_PROVIDER_VENV := build/python-service_provider-venv
ALL_TARGETS := build/test_json_utils build/httpfs_service_provider_c build/httpfs_service_provider_go build/test_http_client_integration
ifeq ($(HAS_FUSE),1)
ALL_TARGETS += build/httpfs_mount
endif

.PHONY: all clean test docker-image docker-test httpfs_mount test-service_provider-c test-service_provider-python test-service_provider-go service_providers-test

all: $(ALL_TARGETS)
ifeq ($(HAS_FUSE),0)
	@echo "fuse3 development package was not found. Please install fuse3 (for example: libfuse3-dev) and then rebuild if you need httpfs_mount. Other available targets have been built."
endif

build:
	mkdir -p build

build/test_json_utils: tests/test_json_utils.c src/json_utils.c include/json_utils.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_json_utils.c src/json_utils.c -o $@ $(LDFLAGS)

build/httpfs_service_provider_c: demo/service_provider/c/main.c $(C_SERVICE_PROVIDER_SRCS) include/httpfs_server.h include/json_utils.h include/hash_utils.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) demo/service_provider/c/main.c $(C_SERVICE_PROVIDER_SRCS) -o $@ $(LDFLAGS)

build/httpfs_service_provider_go: demo/service_provider/go/main.go demo/service_provider/go/go.mod | build
	cd demo/service_provider/go && go build -o ../../../build/httpfs_service_provider_go .

build/test_http_client_integration: tests/test_http_client_integration.c $(COMMON_SRCS) include/http_client.h include/httpfs_protocol.h include/json_utils.h include/hash_utils.h third_party/jsmn.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_http_client_integration.c $(COMMON_SRCS) -o $@ $(LDFLAGS) $(LIBCURL_LIBS)

$(PYTHON_PROVIDER_VENV)/bin/python: demo/service_provider/python/requirements.txt | build
	python3 -m venv $(PYTHON_PROVIDER_VENV)
	$(PYTHON_PROVIDER_VENV)/bin/pip install --upgrade pip
	$(PYTHON_PROVIDER_VENV)/bin/pip install -r demo/service_provider/python/requirements.txt

httpfs_mount: build/httpfs_mount

build/httpfs_mount: src/httpfs_mount_main.c src/httpfs_fuse.c $(COMMON_SRCS) include/httpfs_fuse.h include/http_client.h include/httpfs_protocol.h include/json_utils.h include/hash_utils.h third_party/jsmn.h | build
ifeq ($(HAS_FUSE),1)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) src/httpfs_mount_main.c src/httpfs_fuse.c $(COMMON_SRCS) -o $@ $(LDFLAGS) $(LIBCURL_LIBS) $(FUSE_LIBS)
else
	@echo "fuse3 development package was not found. Please install fuse3 (for example: libfuse3-dev) before building build/httpfs_mount."
	@exit 1
endif

test: all
	chmod +x scripts/run_tests.sh
	./scripts/run_tests.sh

test-service_provider-c: build/httpfs_service_provider_c build/test_http_client_integration
	chmod +x scripts/run_provider_integration.sh
	./scripts/run_provider_integration.sh c-service_provider auto ./build/httpfs_service_provider_c --host 127.0.0.1 --port __PORT__ --root-dir __BACKEND_DIR__

test-service_provider-python: $(PYTHON_PROVIDER_VENV)/bin/python build/test_http_client_integration
	chmod +x scripts/run_provider_integration.sh
	./scripts/run_provider_integration.sh python-service_provider auto $(PYTHON_PROVIDER_VENV)/bin/python demo/service_provider/python/app.py --host 127.0.0.1 --port __PORT__ --root-dir __BACKEND_DIR__

test-service_provider-go: build/httpfs_service_provider_go build/test_http_client_integration
	chmod +x scripts/run_provider_integration.sh
	./scripts/run_provider_integration.sh go-service_provider auto ./build/httpfs_service_provider_go --host 127.0.0.1 --port __PORT__ --root-dir __BACKEND_DIR__

service_providers-test: test-service_provider-c test-service_provider-python test-service_provider-go

docker-image:
	docker build -t httpfs-dev:latest .

docker-test: docker-image
	docker run --rm -v "$(CURDIR):/workspace" -w /workspace httpfs-dev:latest bash -lc "make clean && make all && make test && make service_providers-test && make clean"

clean:
	rm -rf build
