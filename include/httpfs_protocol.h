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

#ifndef HTTPFS_PROTOCOL_H
#define HTTPFS_PROTOCOL_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#define HTTPFS_MAX_PATH 4096
#define HTTPFS_MAX_MESSAGE 512

typedef struct {
    char name[256];
    char type[8];
} httpfs_dir_entry;

typedef struct {
    char path[HTTPFS_MAX_PATH];
    char type[8];
    mode_t mode;
    off_t size;
    uid_t uid;
    gid_t gid;
    nlink_t nlink;
    time_t atime;
    time_t mtime;
    time_t ctime;
} httpfs_meta;

typedef struct {
    long errno_value;
    char message[HTTPFS_MAX_MESSAGE];
} httpfs_error;

#endif
