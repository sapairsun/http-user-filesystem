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

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>
#include <time.h>

#include "httpfs_protocol.h"

typedef struct {
    char base_url[512];
    long timeout_ms;
    int verbose;
} httpfs_client;

void httpfs_client_init(httpfs_client *client, const char *base_url, long timeout_ms);
void httpfs_client_set_verbose(httpfs_client *client, int verbose);

int httpfs_get_meta(httpfs_client *client, const char *path, httpfs_meta *meta, httpfs_error *error);
int httpfs_list_dir(httpfs_client *client, const char *path, httpfs_dir_entry *entries, size_t max_entries,
                    size_t *entry_count, httpfs_error *error);
int httpfs_read_file(httpfs_client *client, const char *path, off_t offset, size_t size, unsigned char *buffer,
                     size_t *bytes_read, httpfs_error *error);
int httpfs_write_file(httpfs_client *client, const char *path, off_t offset, const unsigned char *buffer,
                      size_t size, size_t *bytes_written, httpfs_error *error);
int httpfs_create_file(httpfs_client *client, const char *path, mode_t mode, httpfs_error *error);
int httpfs_create_dir(httpfs_client *client, const char *path, mode_t mode, httpfs_error *error);
int httpfs_remove_file(httpfs_client *client, const char *path, httpfs_error *error);
int httpfs_remove_dir(httpfs_client *client, const char *path, httpfs_error *error);
int httpfs_rename_path(httpfs_client *client, const char *old_path, const char *new_path, httpfs_error *error);
int httpfs_truncate_file(httpfs_client *client, const char *path, off_t size, httpfs_error *error);
int httpfs_update_times(httpfs_client *client, const char *path, const struct timespec times[2], httpfs_error *error);

#endif
