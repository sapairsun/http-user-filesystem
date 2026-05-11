/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "httpfs_server.h"

int main(int argc, char **argv) {
    httpfs_server_config config;
    int i;

    memset(&config, 0, sizeof(config));
    config.host = "127.0.0.1";

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --host\n");
                return 1;
            }
            config.host = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --port\n");
                return 1;
            }
            config.port = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--root-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --root-dir\n");
                return 1;
            }
            config.root_dir = argv[++i];
            continue;
        }

        fprintf(stderr, "Usage: %s --port <port> --root-dir <backend_root_dir> [--host <host>]\n", argv[0]);
        return 1;
    }

    if (config.port <= 0 || config.root_dir == NULL) {
        fprintf(stderr, "Usage: %s --port <port> --root-dir <backend_root_dir> [--host <host>]\n", argv[0]);
        return 1;
    }

    return httpfs_server_run(&config);
}
