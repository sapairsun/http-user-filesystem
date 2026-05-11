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

#include "httpfs_fuse.h"

int main(int argc, char **argv) {
    char **fuse_argv;
    const char *base_url;
    int i;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <base_url> <mountpoint> [FUSE options...]\n", argv[0]);
        return 1;
    }

    base_url = argv[1];
    fuse_argv = malloc((size_t) argc * sizeof(char *));
    if (fuse_argv == NULL) {
        fprintf(stderr, "Failed to allocate FUSE arguments\n");
        return 1;
    }

    fuse_argv[0] = argv[0];
    for (i = 2; i < argc; ++i) {
        fuse_argv[i - 1] = argv[i];
    }

    i = httpfs_fuse_main(argc - 1, fuse_argv, base_url);
    free(fuse_argv);
    return i;
}
