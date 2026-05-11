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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http_client.h"

static void assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", message);
        exit(1);
    }
}

int main(void) {
    const char *base_url = getenv("HTTPFS_BASE_URL");
    httpfs_client client;
    httpfs_error error;
    httpfs_meta meta;
    httpfs_dir_entry entries[64];
    size_t entry_count = 0;
    unsigned char read_buffer[64];
    size_t bytes_read = 0;
    size_t bytes_written = 0;
    const unsigned char payload[] = "hello-httpfs";
    struct timespec times[2];
    int has_renamed = 0;
    size_t i;

    if (base_url == NULL) {
        fprintf(stderr, "Missing HTTPFS_BASE_URL environment variable\n");
        return 1;
    }

    httpfs_client_init(&client, base_url, 5000);

    assert_true(httpfs_get_meta(&client, "/", &meta, &error) == 0, error.message);
    assert_true(strcmp(meta.type, "dir") == 0, "Root path should be a directory");

    assert_true(httpfs_create_dir(&client, "/demo", 0755, &error) == 0, error.message);
    assert_true(httpfs_create_file(&client, "/demo/hello.txt", 0644, &error) == 0, error.message);
    assert_true(httpfs_write_file(&client, "/demo/hello.txt", 0, payload, sizeof(payload) - 1, &bytes_written, &error) ==
                    0,
                error.message);
    assert_true(bytes_written == sizeof(payload) - 1, "Written size should match");

    assert_true(httpfs_get_meta(&client, "/demo/hello.txt", &meta, &error) == 0, error.message);
    assert_true(meta.size == (off_t) (sizeof(payload) - 1), "File size should match");

    assert_true(httpfs_read_file(&client, "/demo/hello.txt", 0, sizeof(read_buffer), read_buffer, &bytes_read, &error) ==
                    0,
                error.message);
    assert_true(bytes_read == sizeof(payload) - 1, "Read size should match");
    assert_true(memcmp(read_buffer, payload, bytes_read) == 0, "Read content should match");

    assert_true(httpfs_rename_path(&client, "/demo/hello.txt", "/demo/renamed.txt", &error) == 0, error.message);
    has_renamed = 1;

    assert_true(httpfs_list_dir(&client, "/demo", entries, 64, &entry_count, &error) == 0, error.message);
    for (i = 0; i < entry_count; ++i) {
        if (strcmp(entries[i].name, "renamed.txt") == 0) {
            has_renamed = 2;
        }
    }
    assert_true(has_renamed == 2, "Directory listing should contain renamed.txt");

    assert_true(httpfs_truncate_file(&client, "/demo/renamed.txt", 5, &error) == 0, error.message);
    assert_true(httpfs_get_meta(&client, "/demo/renamed.txt", &meta, &error) == 0, error.message);
    assert_true(meta.size == 5, "File size after truncate should be 5");

    times[0].tv_sec = 1710000100;
    times[0].tv_nsec = 123456789;
    times[1].tv_sec = 1710000200;
    times[1].tv_nsec = 987654321;
    assert_true(httpfs_update_times(&client, "/demo/renamed.txt", times, &error) == 0, error.message);
    assert_true(httpfs_get_meta(&client, "/demo/renamed.txt", &meta, &error) == 0, error.message);
    assert_true(meta.atime == times[0].tv_sec, "File atime should match updated seconds");
    assert_true(meta.mtime == times[1].tv_sec, "File mtime should match updated seconds");

    assert_true(httpfs_remove_file(&client, "/demo/renamed.txt", &error) == 0, error.message);
    assert_true(httpfs_remove_dir(&client, "/demo", &error) == 0, error.message);

    puts("test_http_client_integration: PASS");
    return 0;
}
