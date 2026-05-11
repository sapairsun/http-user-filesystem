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

#include "http_client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<fuse3/fuse.h>)
#define HTTPFS_USE_FUSE3 1
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#elif __has_include(<fuse/fuse.h>)
#define HTTPFS_USE_FUSE3 0
#define FUSE_USE_VERSION 26
#include <fuse/fuse.h>
#else
#error "FUSE headers not found. Please install libfuse3-dev or libfuse-dev"
#endif
#else
#define HTTPFS_USE_FUSE3 0
#define FUSE_USE_VERSION 26
#include <fuse/fuse.h>
#endif

typedef struct {
    httpfs_client client;
} httpfs_runtime;

static httpfs_runtime *runtime_from_context(void) {
    struct fuse_context *ctx = fuse_get_context();
    return (httpfs_runtime *) ctx->private_data;
}

static int httpfs_getattr_impl(const char *path, struct stat *stbuf) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_meta meta;
    httpfs_error error;

    memset(stbuf, 0, sizeof(*stbuf));

    if (httpfs_get_meta(&runtime->client, path, &meta, &error) != 0) {
        return -(int) error.errno_value;
    }

    stbuf->st_mode = meta.mode;
    stbuf->st_nlink = meta.nlink;
    stbuf->st_size = meta.size;
    stbuf->st_uid = meta.uid;
    stbuf->st_gid = meta.gid;
    stbuf->st_atime = meta.atime;
    stbuf->st_mtime = meta.mtime;
    stbuf->st_ctime = meta.ctime;
    return 0;
}

static int httpfs_readdir_impl(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                               struct fuse_file_info *fi) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_dir_entry entries[512];
    httpfs_error error;
    size_t entry_count = 0;
    size_t i;

    (void) offset;
    (void) fi;

    if (httpfs_list_dir(&runtime->client, path, entries, 512, &entry_count, &error) != 0) {
        return -(int) error.errno_value;
    }

    for (i = 0; i < entry_count; ++i) {
#if HTTPFS_USE_FUSE3
        if (filler(buf, entries[i].name, NULL, 0, 0) != 0) {
#else
        if (filler(buf, entries[i].name, NULL, 0) != 0) {
#endif
            return -ENOMEM;
        }
    }

    return 0;
}

static int httpfs_open(const char *path, struct fuse_file_info *fi) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_meta meta;
    httpfs_error error;

    (void) fi;
    if (httpfs_get_meta(&runtime->client, path, &meta, &error) != 0) {
        return -(int) error.errno_value;
    }
    return 0;
}

static int httpfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;
    size_t bytes_read = 0;

    (void) fi;
    if (httpfs_read_file(&runtime->client, path, offset, size, (unsigned char *) buf, &bytes_read, &error) != 0) {
        return -(int) error.errno_value;
    }
    return (int) bytes_read;
}

static int httpfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;
    size_t bytes_written = 0;

    (void) fi;
    if (httpfs_write_file(&runtime->client, path, offset, (const unsigned char *) buf, size, &bytes_written, &error) !=
        0) {
        return -(int) error.errno_value;
    }
    return (int) bytes_written;
}

static int httpfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;

    (void) fi;
    if (httpfs_create_file(&runtime->client, path, mode, &error) != 0) {
        return -(int) error.errno_value;
    }
    return 0;
}

static int httpfs_mkdir(const char *path, mode_t mode) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;

    if (httpfs_create_dir(&runtime->client, path, mode, &error) != 0) {
        return -(int) error.errno_value;
    }
    return 0;
}

static int httpfs_unlink(const char *path) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;

    if (httpfs_remove_file(&runtime->client, path, &error) != 0) {
        return -(int) error.errno_value;
    }
    return 0;
}

static int httpfs_rmdir(const char *path) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;

    if (httpfs_remove_dir(&runtime->client, path, &error) != 0) {
        return -(int) error.errno_value;
    }
    return 0;
}

static int httpfs_rename_impl(const char *from, const char *to) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;

    if (httpfs_rename_path(&runtime->client, from, to, &error) != 0) {
        return -(int) error.errno_value;
    }
    return 0;
}

static int httpfs_truncate_impl(const char *path, off_t size) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;

    if (httpfs_truncate_file(&runtime->client, path, size, &error) != 0) {
        return -(int) error.errno_value;
    }
    return 0;
}

static int httpfs_utimens_impl(const char *path, const struct timespec tv[2]) {
    httpfs_runtime *runtime = runtime_from_context();
    httpfs_error error;

    if (httpfs_update_times(&runtime->client, path, tv, &error) != 0) {
        return -(int) error.errno_value;
    }
    return 0;
}

#if HTTPFS_USE_FUSE3
static int httpfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;
    return httpfs_getattr_impl(path, stbuf);
}

static int httpfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi,
                          enum fuse_readdir_flags flags) {
    (void) flags;
    return httpfs_readdir_impl(path, buf, filler, offset, fi);
}

static int httpfs_rename(const char *from, const char *to, unsigned int flags) {
    if (flags != 0) {
        return -EINVAL;
    }
    return httpfs_rename_impl(from, to);
}

static int httpfs_truncate_op(const char *path, off_t size, struct fuse_file_info *fi) {
    (void) fi;
    return httpfs_truncate_impl(path, size);
}

static int httpfs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void) fi;
    return httpfs_utimens_impl(path, tv);
}
#else
static int httpfs_getattr(const char *path, struct stat *stbuf) {
    return httpfs_getattr_impl(path, stbuf);
}

static int httpfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    return httpfs_readdir_impl(path, buf, filler, offset, fi);
}

static int httpfs_rename(const char *from, const char *to) {
    return httpfs_rename_impl(from, to);
}

static int httpfs_truncate_op(const char *path, off_t size) {
    return httpfs_truncate_impl(path, size);
}

static int httpfs_utimens(const char *path, const struct timespec tv[2]) {
    return httpfs_utimens_impl(path, tv);
}
#endif

static struct fuse_operations httpfs_ops = {
    .getattr = httpfs_getattr,
    .readdir = httpfs_readdir,
    .open = httpfs_open,
    .read = httpfs_read,
    .write = httpfs_write,
    .create = httpfs_create,
    .mkdir = httpfs_mkdir,
    .unlink = httpfs_unlink,
    .rmdir = httpfs_rmdir,
    .rename = httpfs_rename,
    .truncate = httpfs_truncate_op,
    .utimens = httpfs_utimens,
};

int httpfs_fuse_main(int argc, char **argv, const char *base_url) {
    httpfs_runtime runtime;
    httpfs_client_init(&runtime.client, base_url, 5000);
    return fuse_main(argc, argv, &httpfs_ops, &runtime);
}
