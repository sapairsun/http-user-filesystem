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

#define _POSIX_C_SOURCE 200809L

#include "httpfs_server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "hash_utils.h"
#include "json_utils.h"

#define SERVER_LIST_PAYLOAD_SIZE 262144
#define SERVER_INITIAL_REQUEST_BUFFER_SIZE 8192
#define SERVER_MAX_REQUEST_SIZE (8U * 1024U * 1024U)
#define SERVER_MAX_ENTRIES 4096
#define SERVER_ACCEPT_BACKLOG 128
#define SERVER_EPOLL_MAX_EVENTS 64
#define SERVER_DEFAULT_WORKERS 4
#define SERVER_MAX_WORKERS 32

static volatile sig_atomic_t g_stop_server = 0;

typedef struct {
    int fd;
    char method[8];
    char path[2048];
    char query[2048];
    char content_md5[33];
    char *body;
    size_t body_length;
    const char *root_dir;
} request_context;

typedef struct pending_fd_node {
    int fd;
    struct pending_fd_node *next;
} pending_fd_node;

struct worker_context;

typedef struct {
    int event_type;
    struct worker_context *worker;
} worker_notifier;

typedef struct connection_state {
    int event_type;
    int fd;
    size_t received;
    long content_length;
    size_t header_bytes;
    struct worker_context *worker;
    char *buffer;
    size_t capacity;
} connection_state;

typedef struct worker_context {
    int index;
    int epoll_fd;
    int notify_pipe[2];
    pthread_t thread;
    pthread_mutex_t queue_mutex;
    pending_fd_node *pending_head;
    pending_fd_node *pending_tail;
    const char *root_dir;
    worker_notifier notifier;
} worker_context;

static void handle_signal(int signum) {
    (void) signum;
    g_stop_server = 1;
}

static void send_all(int fd, const void *data, size_t length) {
    size_t sent = 0;
    const char *bytes = (const char *) data;
    while (sent < length) {
        ssize_t written = send(fd, bytes + sent, length - sent, 0);
        if (written <= 0) {
            return;
        }
        sent += (size_t) written;
    }
}

static void send_json(int fd, int status_code, const char *body) {
    char header[256];
    size_t body_length = strlen(body);
    int header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 %d OK\r\n"
                                 "Content-Type: application/json; charset=utf-8\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n\r\n",
                                 status_code, body_length);
    if (header_length > 0) {
        send_all(fd, header, (size_t) header_length);
    }
    send_all(fd, body, body_length);
}

static void send_ok(int fd, const char *payload) {
    send_json(fd, 200, payload);
}

static void send_binary(int fd, const void *body, size_t body_length, const char *content_md5) {
    char header[256];
    int header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/octet-stream\r\n"
                                 "X-HTTPFS-Content-MD5: %s\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n\r\n",
                                 content_md5 != NULL ? content_md5 : "", body_length);
    if (header_length > 0) {
        send_all(fd, header, (size_t) header_length);
    }
    if (body_length > 0) {
        send_all(fd, body, body_length);
    }
}

static int extract_header_value(const char *headers_start, const char *headers_end, const char *header_name, char *output,
                                size_t output_size) {
    size_t header_name_length = strlen(header_name);
    const char *cursor = headers_start;

    if (output == NULL || output_size == 0U) {
        return -1;
    }
    output[0] = '\0';

    while (cursor < headers_end) {
        const char *line_end = strstr(cursor, "\r\n");
        const char *value_start;
        size_t value_length;
        if (line_end == NULL || line_end > headers_end) {
            break;
        }
        if ((size_t) (line_end - cursor) > header_name_length &&
            strncasecmp(cursor, header_name, header_name_length) == 0 && cursor[header_name_length] == ':') {
            value_start = cursor + header_name_length + 1;
            while (value_start < line_end && (*value_start == ' ' || *value_start == '\t')) {
                value_start++;
            }
            value_length = (size_t) (line_end - value_start);
            if (value_length + 1U > output_size) {
                return -1;
            }
            memcpy(output, value_start, value_length);
            output[value_length] = '\0';
            return 0;
        }
        cursor = line_end + 2;
    }
    return -1;
}

static ssize_t pwrite_full(int fd, const unsigned char *buffer, size_t length, off_t offset) {
    size_t total_written = 0;

    while (total_written < length) {
        ssize_t written = pwrite(fd, buffer + total_written, length - total_written, offset + (off_t) total_written);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            break;
        }
        total_written += (size_t) written;
    }

    return (ssize_t) total_written;
}

static ssize_t pread_full(int fd, unsigned char *buffer, size_t length, off_t offset) {
    size_t total_read = 0;

    while (total_read < length) {
        ssize_t bytes_read = pread(fd, buffer + total_read, length - total_read, offset + (off_t) total_read);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        total_read += (size_t) bytes_read;
    }

    return (ssize_t) total_read;
}

static int ensure_connection_capacity(connection_state *conn, size_t required_size) {
    char *new_buffer;
    size_t new_capacity;

    if (required_size <= conn->capacity) {
        return 0;
    }
    if (required_size > SERVER_MAX_REQUEST_SIZE) {
        return -1;
    }

    new_capacity = conn->capacity > 0 ? conn->capacity : SERVER_INITIAL_REQUEST_BUFFER_SIZE;
    while (new_capacity < required_size) {
        if (new_capacity >= SERVER_MAX_REQUEST_SIZE / 2U) {
            new_capacity = SERVER_MAX_REQUEST_SIZE;
            break;
        }
        new_capacity *= 2U;
    }
    if (new_capacity < required_size || new_capacity > SERVER_MAX_REQUEST_SIZE) {
        return -1;
    }

    new_buffer = realloc(conn->buffer, new_capacity);
    if (new_buffer == NULL) {
        return -1;
    }

    conn->buffer = new_buffer;
    conn->capacity = new_capacity;
    return 0;
}

static void send_error_json(int fd, long errno_value, const char *message) {
    char escaped[1024];
    char payload[1400];

    if (json_escape_string(message, escaped, sizeof(escaped)) != 0) {
        snprintf(escaped, sizeof(escaped), "internal error");
    }

    snprintf(payload, sizeof(payload), "{\"status\":\"error\",\"errno\":%ld,\"message\":\"%s\"}", errno_value, escaped);
    send_json(fd, 200, payload);
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int url_decode(const char *input, char *output, size_t output_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (input[in_pos] != '\0') {
        if (out_pos + 1 >= output_size) {
            return -1;
        }

        if (input[in_pos] == '%') {
            int high;
            int low;
            if (input[in_pos + 1] == '\0' || input[in_pos + 2] == '\0') {
                return -1;
            }
            high = hex_value(input[in_pos + 1]);
            low = hex_value(input[in_pos + 2]);
            if (high < 0 || low < 0) {
                return -1;
            }
            output[out_pos++] = (char) ((high << 4) | low);
            in_pos += 3;
        } else if (input[in_pos] == '+') {
            output[out_pos++] = ' ';
            in_pos++;
        } else {
            output[out_pos++] = input[in_pos++];
        }
    }

    output[out_pos] = '\0';
    return 0;
}

static const char *query_value(const char *query, const char *key, char *output, size_t output_size) {
    size_t key_length = strlen(key);
    const char *cursor = query;

    while (*cursor != '\0') {
        const char *amp = strchr(cursor, '&');
        const char *segment_end = amp != NULL ? amp : cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t) (segment_end - cursor));

        if (equals != NULL && (size_t) (equals - cursor) == key_length && strncmp(cursor, key, key_length) == 0) {
            size_t encoded_len = (size_t) (segment_end - equals - 1);
            char encoded[2048];

            if (encoded_len + 1 > sizeof(encoded)) {
                return NULL;
            }

            memcpy(encoded, equals + 1, encoded_len);
            encoded[encoded_len] = '\0';
            if (url_decode(encoded, output, output_size) != 0) {
                return NULL;
            }
            return output;
        }

        cursor = (*segment_end == '&') ? segment_end + 1 : segment_end;
    }

    return NULL;
}

static int json_get_string(const char *json, const char *key, char *output, size_t output_size) {
    char pattern[128];
    char *start;
    size_t out_pos = 0;

    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    start = strstr((char *) json, pattern);
    if (start == NULL) {
        return -1;
    }

    start += strlen(pattern);
    while (*start != '\0' && *start != '"') {
        if (*start == '\\') {
            start++;
            if (*start == '\0') {
                return -1;
            }
            switch (*start) {
                case 'n':
                    output[out_pos++] = '\n';
                    break;
                case 'r':
                    output[out_pos++] = '\r';
                    break;
                case 't':
                    output[out_pos++] = '\t';
                    break;
                case '\\':
                case '"':
                case '/':
                    output[out_pos++] = *start;
                    break;
                default:
                    return -1;
            }
        } else {
            output[out_pos++] = *start;
        }

        if (out_pos + 1 >= output_size) {
            return -1;
        }
        start++;
    }

    if (*start != '"') {
        return -1;
    }

    output[out_pos] = '\0';
    return 0;
}

static int json_get_long(const char *json, const char *key, long long *value) {
    char pattern[128];
    char *start;
    char *endptr;

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    start = strstr((char *) json, pattern);
    if (start == NULL) {
        return -1;
    }

    start += strlen(pattern);
    *value = strtoll(start, &endptr, 10);
    if (start == endptr) {
        return -1;
    }
    return 0;
}

static int resolve_path(const char *root_dir, const char *path, char *resolved, size_t resolved_size) {
    if (path[0] != '/') {
        return -1;
    }

    if (strcmp(path, "/") == 0) {
        return snprintf(resolved, resolved_size, "%s", root_dir) < (int) resolved_size ? 0 : -1;
    }

    if (strstr(path, "/../") != NULL || strstr(path, "/./") != NULL || strstr(path, "//") != NULL ||
        strcmp(path, "/..") == 0 || strcmp(path, "/.") == 0) {
        return -1;
    }

    return snprintf(resolved, resolved_size, "%s%s", root_dir, path) < (int) resolved_size ? 0 : -1;
}

static void stat_to_json(const char *path, const struct stat *st, char *output, size_t output_size) {
    const char *type = S_ISDIR(st->st_mode) ? "dir" : "file";
    snprintf(output, output_size,
             "{\"status\":\"ok\",\"meta\":{\"path\":\"%s\",\"type\":\"%s\",\"mode\":%u,\"size\":%lld,\"uid\":%u,"
             "\"gid\":%u,\"nlink\":%lu,\"atime\":%lld,\"mtime\":%lld,\"ctime\":%lld}}",
             path, type, (unsigned int) st->st_mode, (long long) st->st_size, (unsigned int) st->st_uid,
             (unsigned int) st->st_gid, (unsigned long) st->st_nlink, (long long) st->st_atime, (long long) st->st_mtime,
             (long long) st->st_ctime);
}

static void handle_meta(request_context *ctx) {
    char path[2048];
    char local_path[4096];
    struct stat st;
    size_t payload_size;
    char *payload;

    if (query_value(ctx->query, "path", path, sizeof(path)) == NULL || resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid path");
        return;
    }

    if (lstat(local_path, &st) != 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    payload_size = strlen(path) + 256;
    payload = malloc(payload_size);
    if (payload == NULL) {
        send_error_json(ctx->fd, ENOMEM, "memory allocation failed");
        return;
    }

    stat_to_json(path, &st, payload, payload_size);
    send_ok(ctx->fd, payload);
    free(payload);
}

static void handle_list(request_context *ctx) {
    char path[2048];
    char local_path[4096];
    char entry_path[4096];
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    char payload[SERVER_LIST_PAYLOAD_SIZE];
    size_t pos = 0;
    int first = 1;

    if (query_value(ctx->query, "path", path, sizeof(path)) == NULL || resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid path");
        return;
    }

    dir = opendir(local_path);
    if (dir == NULL) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    pos += (size_t) snprintf(payload + pos, sizeof(payload) - pos, "{\"status\":\"ok\",\"entries\":[");
    while ((entry = readdir(dir)) != NULL) {
        const char *type = "file";
        char escaped[512];

        if (snprintf(entry_path, sizeof(entry_path), "%s/%s", local_path, entry->d_name) >= (int) sizeof(entry_path)) {
            closedir(dir);
            send_error_json(ctx->fd, ENAMETOOLONG, "entry path too long");
            return;
        }

        if (lstat(entry_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            type = "dir";
        }

        if (json_escape_string(entry->d_name, escaped, sizeof(escaped)) != 0) {
            closedir(dir);
            send_error_json(ctx->fd, EIO, "entry name escape failed");
            return;
        }

        pos += (size_t) snprintf(payload + pos, sizeof(payload) - pos, "%s{\"name\":\"%s\",\"type\":\"%s\"}",
                                 first ? "" : ",", escaped, type);
        first = 0;
        if (pos + 64 >= sizeof(payload)) {
            closedir(dir);
            send_error_json(ctx->fd, ENOBUFS, "too many entries");
            return;
        }
    }

    closedir(dir);
    snprintf(payload + pos, sizeof(payload) - pos, "]}");
    send_ok(ctx->fd, payload);
}

static void handle_read(request_context *ctx) {
    char path[2048];
    char local_path[4096];
    char offset_text[64];
    char size_text[64];
    long long offset = 0;
    long long size = 0;
    int fd;
    unsigned char *buffer;
    ssize_t bytes_read;
    char content_md5[33];

    if (query_value(ctx->query, "path", path, sizeof(path)) == NULL ||
        query_value(ctx->query, "offset", offset_text, sizeof(offset_text)) == NULL ||
        query_value(ctx->query, "size", size_text, sizeof(size_text)) == NULL ||
        resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid read query");
        return;
    }

    offset = strtoll(offset_text, NULL, 10);
    size = strtoll(size_text, NULL, 10);
    if (offset < 0 || size < 0) {
        send_error_json(ctx->fd, EINVAL, "invalid offset or size");
        return;
    }

    fd = open(local_path, O_RDONLY);
    if (fd < 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    buffer = malloc((size_t) size + 1);
    if (buffer == NULL) {
        close(fd);
        free(buffer);
        send_error_json(ctx->fd, ENOMEM, "memory allocation failed");
        return;
    }

    bytes_read = pread(fd, buffer, (size_t) size, (off_t) offset);
    close(fd);
    if (bytes_read < 0) {
        free(buffer);
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    if (httpfs_sparse_md5_hex(buffer, (size_t) bytes_read, (off_t) offset, content_md5) != 0) {
        free(buffer);
        send_error_json(ctx->fd, EIO, "failed to compute read content md5");
        return;
    }

    send_binary(ctx->fd, buffer, (size_t) bytes_read, content_md5);
    free(buffer);
}

static void handle_write(request_context *ctx) {
    char path[2048];
    char local_path[4096];
    char offset_text[64];
    long long offset = 0;
    int fd;
    unsigned char *verify_buffer = NULL;
    ssize_t written;
    ssize_t verified;
    char expected_md5[33];
    char actual_md5[33];
    char payload[256];

    if (query_value(ctx->query, "path", path, sizeof(path)) == NULL ||
        query_value(ctx->query, "offset", offset_text, sizeof(offset_text)) == NULL ||
        resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid write body");
        return;
    }
    offset = strtoll(offset_text, NULL, 10);
    if (offset < 0) {
        send_error_json(ctx->fd, EINVAL, "invalid write offset");
        return;
    }
    if (!httpfs_is_valid_md5_hex(ctx->content_md5)) {
        send_error_json(ctx->fd, EINVAL, "missing or invalid content md5");
        return;
    }
    if (httpfs_sparse_md5_hex((const unsigned char *) ctx->body, ctx->body_length, (off_t) offset, expected_md5) != 0) {
        send_error_json(ctx->fd, EIO, "failed to compute write content md5");
        return;
    }
    if (strcmp(expected_md5, ctx->content_md5) != 0) {
        send_error_json(ctx->fd, EIO, "write content md5 verification failed");
        return;
    }

    fd = open(local_path, O_RDWR);
    if (fd < 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    written = pwrite_full(fd, (const unsigned char *) ctx->body, ctx->body_length, (off_t) offset);
    if (written < 0) {
        close(fd);
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }
    if ((size_t) written != ctx->body_length) {
        close(fd);
        send_error_json(ctx->fd, EIO, "short write");
        return;
    }

    verify_buffer = malloc(ctx->body_length > 0 ? ctx->body_length : 1U);
    if (verify_buffer == NULL) {
        close(fd);
        send_error_json(ctx->fd, ENOMEM, "memory allocation failed");
        return;
    }
    verified = pread_full(fd, verify_buffer, ctx->body_length, (off_t) offset);
    close(fd);
    if (verified < 0) {
        free(verify_buffer);
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }
    if ((size_t) verified != ctx->body_length ||
        httpfs_sparse_md5_hex(verify_buffer, (size_t) verified, (off_t) offset, actual_md5) != 0 ||
        strcmp(actual_md5, expected_md5) != 0) {
        free(verify_buffer);
        send_error_json(ctx->fd, EIO, "stored content md5 verification failed");
        return;
    }
    free(verify_buffer);

    snprintf(payload, sizeof(payload), "{\"status\":\"ok\",\"bytes_written\":%lld,\"content_md5\":\"%s\"}", (long long) written,
             actual_md5);
    send_ok(ctx->fd, payload);
}

static void handle_create_file(request_context *ctx) {
    char path[2048];
    char local_path[4096];
    long long mode = 0;
    int fd;

    if (json_get_string(ctx->body, "path", path, sizeof(path)) != 0 || json_get_long(ctx->body, "mode", &mode) != 0 ||
        resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid create-file body");
        return;
    }

    fd = open(local_path, O_CREAT | O_EXCL | O_WRONLY, (mode_t) mode);
    if (fd < 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }
    close(fd);
    send_ok(ctx->fd, "{\"status\":\"ok\"}");
}

static void handle_create_dir(request_context *ctx) {
    char path[2048];
    char local_path[4096];
    long long mode = 0;

    if (json_get_string(ctx->body, "path", path, sizeof(path)) != 0 || json_get_long(ctx->body, "mode", &mode) != 0 ||
        resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid create-dir body");
        return;
    }

    if (mkdir(local_path, (mode_t) mode) != 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    send_ok(ctx->fd, "{\"status\":\"ok\"}");
}

static void handle_remove_file(request_context *ctx) {
    char path[2048];
    char local_path[4096];

    if (json_get_string(ctx->body, "path", path, sizeof(path)) != 0 || resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid remove-file body");
        return;
    }

    if (unlink(local_path) != 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    send_ok(ctx->fd, "{\"status\":\"ok\"}");
}

static void handle_remove_dir(request_context *ctx) {
    char path[2048];
    char local_path[4096];

    if (json_get_string(ctx->body, "path", path, sizeof(path)) != 0 || resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid remove-dir body");
        return;
    }

    if (rmdir(local_path) != 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    send_ok(ctx->fd, "{\"status\":\"ok\"}");
}

static void handle_rename(request_context *ctx) {
    char old_path[2048];
    char new_path[2048];
    char local_old[4096];
    char local_new[4096];

    if (json_get_string(ctx->body, "old_path", old_path, sizeof(old_path)) != 0 ||
        json_get_string(ctx->body, "new_path", new_path, sizeof(new_path)) != 0 ||
        resolve_path(ctx->root_dir, old_path, local_old, sizeof(local_old)) != 0 ||
        resolve_path(ctx->root_dir, new_path, local_new, sizeof(local_new)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid rename body");
        return;
    }

    if (rename(local_old, local_new) != 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    send_ok(ctx->fd, "{\"status\":\"ok\"}");
}

static void handle_truncate(request_context *ctx) {
    char path[2048];
    char local_path[4096];
    long long size = 0;

    if (json_get_string(ctx->body, "path", path, sizeof(path)) != 0 || json_get_long(ctx->body, "size", &size) != 0 ||
        resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid truncate body");
        return;
    }

    if (truncate(local_path, (off_t) size) != 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    send_ok(ctx->fd, "{\"status\":\"ok\"}");
}

static void handle_utimens(request_context *ctx) {
    char path[2048];
    char local_path[4096];
    long long atime_sec = 0;
    long long atime_nsec = 0;
    long long mtime_sec = 0;
    long long mtime_nsec = 0;
    struct timespec times[2];

    if (json_get_string(ctx->body, "path", path, sizeof(path)) != 0 ||
        json_get_long(ctx->body, "atime_sec", &atime_sec) != 0 ||
        json_get_long(ctx->body, "atime_nsec", &atime_nsec) != 0 ||
        json_get_long(ctx->body, "mtime_sec", &mtime_sec) != 0 ||
        json_get_long(ctx->body, "mtime_nsec", &mtime_nsec) != 0 ||
        resolve_path(ctx->root_dir, path, local_path, sizeof(local_path)) != 0) {
        send_error_json(ctx->fd, EINVAL, "invalid utimens body");
        return;
    }

    times[0].tv_sec = (time_t) atime_sec;
    times[0].tv_nsec = (long) atime_nsec;
    times[1].tv_sec = (time_t) mtime_sec;
    times[1].tv_nsec = (long) mtime_nsec;

    if (utimensat(AT_FDCWD, local_path, times, 0) != 0) {
        send_error_json(ctx->fd, errno, strerror(errno));
        return;
    }

    send_ok(ctx->fd, "{\"status\":\"ok\"}");
}

static void dispatch_request(request_context *ctx) {
    if (strcmp(ctx->method, "GET") == 0 && strcmp(ctx->path, "/v1/meta") == 0) {
        handle_meta(ctx);
        return;
    }
    if (strcmp(ctx->method, "GET") == 0 && strcmp(ctx->path, "/v1/list") == 0) {
        handle_list(ctx);
        return;
    }
    if (strcmp(ctx->method, "GET") == 0 && strcmp(ctx->path, "/v1/read") == 0) {
        handle_read(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/v1/write") == 0) {
        handle_write(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/v1/create-file") == 0) {
        handle_create_file(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/v1/create-dir") == 0) {
        handle_create_dir(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/v1/remove-file") == 0) {
        handle_remove_file(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/v1/remove-dir") == 0) {
        handle_remove_dir(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/v1/rename") == 0) {
        handle_rename(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/v1/truncate") == 0) {
        handle_truncate(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/v1/utimens") == 0) {
        handle_utimens(ctx);
        return;
    }

    send_error_json(ctx->fd, ENOSYS, "unknown endpoint");
}

static int parse_request_line(char *line, request_context *ctx) {
    char raw_uri[2048];
    char *query_start;

    if (sscanf(line, "%7s %2047s", ctx->method, raw_uri) != 2) {
        return -1;
    }

    query_start = strchr(raw_uri, '?');
    if (query_start != NULL) {
        *query_start = '\0';
        query_start++;
        snprintf(ctx->query, sizeof(ctx->query), "%s", query_start);
    } else {
        ctx->query[0] = '\0';
    }

    snprintf(ctx->path, sizeof(ctx->path), "%s", raw_uri);
    return 0;
}

static void process_request_buffer(int client_fd, const char *root_dir, char *buffer, size_t received) {
    char *header_end;
    char *line_end;
    char *content_length_header;
    long content_length = 0;
    request_context ctx;
    const char *headers_start;

    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = client_fd;
    ctx.root_dir = root_dir;

    header_end = strstr(buffer, "\r\n\r\n");
    if (header_end == NULL) {
        send_error_json(client_fd, EINVAL, "invalid http request");
        return;
    }

    line_end = strstr(buffer, "\r\n");
    if (line_end == NULL) {
        send_error_json(client_fd, EINVAL, "invalid request line");
        return;
    }
    *line_end = '\0';

    if (parse_request_line(buffer, &ctx) != 0) {
        send_error_json(client_fd, EINVAL, "invalid request line");
        return;
    }

    headers_start = line_end + 2;
    content_length_header = strstr(line_end + 2, "Content-Length:");
    if (content_length_header != NULL) {
        content_length = strtol(content_length_header + strlen("Content-Length:"), NULL, 10);
    }
    if (extract_header_value(headers_start, header_end, "X-HTTPFS-Content-MD5", ctx.content_md5, sizeof(ctx.content_md5)) != 0) {
        ctx.content_md5[0] = '\0';
    }

    ctx.body = header_end + 4;
    ctx.body_length = (size_t) ((buffer + received) - ctx.body);
    if ((long) ctx.body_length < content_length) {
        send_error_json(client_fd, EINVAL, "request body truncated");
        return;
    }

    dispatch_request(&ctx);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
    return 0;
}

static size_t choose_worker_count(void) {
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0) {
        return SERVER_DEFAULT_WORKERS;
    }
    if (cpu_count > SERVER_MAX_WORKERS) {
        return SERVER_MAX_WORKERS;
    }
    return (size_t) cpu_count;
}

static int enqueue_pending_fd(worker_context *worker, int fd) {
    pending_fd_node *node = malloc(sizeof(*node));

    if (node == NULL) {
        return -1;
    }
    node->fd = fd;
    node->next = NULL;

    pthread_mutex_lock(&worker->queue_mutex);
    if (worker->pending_tail != NULL) {
        worker->pending_tail->next = node;
    } else {
        worker->pending_head = node;
    }
    worker->pending_tail = node;
    pthread_mutex_unlock(&worker->queue_mutex);
    return 0;
}

static int dequeue_pending_fd(worker_context *worker) {
    pending_fd_node *node;
    int fd;

    pthread_mutex_lock(&worker->queue_mutex);
    node = worker->pending_head;
    if (node == NULL) {
        pthread_mutex_unlock(&worker->queue_mutex);
        return -1;
    }
    worker->pending_head = node->next;
    if (worker->pending_head == NULL) {
        worker->pending_tail = NULL;
    }
    pthread_mutex_unlock(&worker->queue_mutex);

    fd = node->fd;
    free(node);
    return fd;
}

static void close_connection(connection_state *conn) {
    if (conn == NULL) {
        return;
    }
    epoll_ctl(conn->worker->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    free(conn->buffer);
    free(conn);
}

static void drain_notify_pipe(worker_context *worker) {
    char buffer[64];
    ssize_t bytes_read;

    do {
        bytes_read = read(worker->notify_pipe[0], buffer, sizeof(buffer));
    } while (bytes_read > 0 || (bytes_read < 0 && errno == EINTR));
}

static int add_client_to_worker(worker_context *worker, int client_fd) {
    connection_state *conn;
    struct epoll_event event;

    conn = calloc(1, sizeof(*conn));
    if (conn == NULL) {
        return -1;
    }

    conn->event_type = 2;
    conn->fd = client_fd;
    conn->content_length = -1;
    conn->worker = worker;
    if (ensure_connection_capacity(conn, SERVER_INITIAL_REQUEST_BUFFER_SIZE) != 0) {
        free(conn);
        return -1;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.ptr = conn;
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &event) != 0) {
        free(conn);
        return -1;
    }

    return 0;
}

static void add_pending_clients(worker_context *worker) {
    for (;;) {
        int client_fd = dequeue_pending_fd(worker);
        if (client_fd < 0) {
            return;
        }
        if (add_client_to_worker(worker, client_fd) != 0) {
            close(client_fd);
        }
    }
}

static int connection_has_complete_request(connection_state *conn) {
    char *header_end;
    char *content_length_header;
    size_t total_required;

    conn->buffer[conn->received] = '\0';
    if (conn->header_bytes == 0U) {
        header_end = strstr(conn->buffer, "\r\n\r\n");
        if (header_end == NULL) {
            return 0;
        }
        conn->header_bytes = (size_t) ((header_end + 4) - conn->buffer);
        conn->content_length = 0;
        content_length_header = strstr(conn->buffer, "Content-Length:");
        if (content_length_header != NULL && content_length_header < header_end) {
            conn->content_length = strtol(content_length_header + strlen("Content-Length:"), NULL, 10);
            if (conn->content_length < 0) {
                return -1;
            }
        }
    }

    total_required = conn->header_bytes + (size_t) conn->content_length;
    if (total_required + 1U > SERVER_MAX_REQUEST_SIZE) {
        return -2;
    }
    if (ensure_connection_capacity(conn, total_required + 1U) != 0) {
        return -2;
    }
    return conn->received >= total_required ? 1 : 0;
}

static void process_connection_request(connection_state *conn) {
    process_request_buffer(conn->fd, conn->worker->root_dir, conn->buffer, conn->received);
}

static void handle_client_event(connection_state *conn, uint32_t events) {
    for (;;) {
        ssize_t bytes_read;
        int state;

        if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
            close_connection(conn);
            return;
        }

        if (conn->capacity - conn->received <= 1U) {
            size_t next_capacity = conn->capacity > 0 ? conn->capacity * 2U : SERVER_INITIAL_REQUEST_BUFFER_SIZE;
            if (next_capacity > SERVER_MAX_REQUEST_SIZE) {
                next_capacity = SERVER_MAX_REQUEST_SIZE;
            }
            if (ensure_connection_capacity(conn, next_capacity) != 0) {
                send_error_json(conn->fd, ENOBUFS, "request too large");
                close_connection(conn);
                return;
            }
        }

        bytes_read = recv(conn->fd, conn->buffer + conn->received, conn->capacity - conn->received - 1U, 0);
        if (bytes_read == 0) {
            close_connection(conn);
            return;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            close_connection(conn);
            return;
        }

        conn->received += (size_t) bytes_read;
        state = connection_has_complete_request(conn);
        if (state < 0) {
            if (state == -2) {
                send_error_json(conn->fd, ENOBUFS, "request too large");
            } else {
                send_error_json(conn->fd, EINVAL, "invalid http request");
            }
            close_connection(conn);
            return;
        }
        if (state > 0) {
            if (fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL, 0) & ~O_NONBLOCK) != 0) {
                close_connection(conn);
                return;
            }
            process_connection_request(conn);
            close_connection(conn);
            return;
        }
    }
}

static void *worker_thread_main(void *arg) {
    worker_context *worker = arg;
    struct epoll_event events[SERVER_EPOLL_MAX_EVENTS];

    while (!g_stop_server) {
        int ready = epoll_wait(worker->epoll_fd, events, SERVER_EPOLL_MAX_EVENTS, 1000);
        int i;

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return NULL;
        }

        for (i = 0; i < ready; ++i) {
            if (events[i].data.ptr == &worker->notifier) {
                drain_notify_pipe(worker);
                add_pending_clients(worker);
                continue;
            }
            handle_client_event((connection_state *) events[i].data.ptr, events[i].events);
        }
    }

    drain_notify_pipe(worker);
    add_pending_clients(worker);
    return NULL;
}

static void destroy_worker(worker_context *worker) {
    pending_fd_node *node;

    if (worker->epoll_fd >= 0) {
        close(worker->epoll_fd);
    }
    if (worker->notify_pipe[0] >= 0) {
        close(worker->notify_pipe[0]);
    }
    if (worker->notify_pipe[1] >= 0) {
        close(worker->notify_pipe[1]);
    }

    node = worker->pending_head;
    while (node != NULL) {
        pending_fd_node *next = node->next;
        close(node->fd);
        free(node);
        node = next;
    }
    pthread_mutex_destroy(&worker->queue_mutex);
}

static int initialize_worker(worker_context *worker, int index, const char *root_dir) {
    struct epoll_event event;

    memset(worker, 0, sizeof(*worker));
    worker->index = index;
    worker->root_dir = root_dir;
    worker->epoll_fd = -1;
    worker->notify_pipe[0] = -1;
    worker->notify_pipe[1] = -1;

    if (pthread_mutex_init(&worker->queue_mutex, NULL) != 0) {
        return -1;
    }

    worker->epoll_fd = epoll_create1(0);
    if (worker->epoll_fd < 0) {
        destroy_worker(worker);
        return -1;
    }

    if (pipe(worker->notify_pipe) != 0) {
        destroy_worker(worker);
        return -1;
    }
    if (set_nonblocking(worker->notify_pipe[0]) != 0 || set_nonblocking(worker->notify_pipe[1]) != 0) {
        destroy_worker(worker);
        return -1;
    }

    worker->notifier.event_type = 1;
    worker->notifier.worker = worker;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = &worker->notifier;
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, worker->notify_pipe[0], &event) != 0) {
        destroy_worker(worker);
        return -1;
    }

    if (pthread_create(&worker->thread, NULL, worker_thread_main, worker) != 0) {
        destroy_worker(worker);
        return -1;
    }

    return 0;
}

static void wake_worker(worker_context *worker) {
    char byte = 'w';
    ssize_t written;

    do {
        written = write(worker->notify_pipe[1], &byte, 1);
    } while (written < 0 && errno == EINTR);
}

static void stop_workers(worker_context *workers, size_t started_count) {
    size_t i;

    for (i = 0; i < started_count; ++i) {
        wake_worker(&workers[i]);
    }
    for (i = 0; i < started_count; ++i) {
        pthread_join(workers[i].thread, NULL);
        destroy_worker(&workers[i]);
    }
}

int httpfs_server_run(const httpfs_server_config *config) {
    int server_fd;
    int opt_value = 1;
    worker_context *workers = NULL;
    size_t worker_count = choose_worker_count();
    size_t started_workers = 0;
    size_t next_worker = 0;
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *entry;
    char port_text[16];

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    workers = calloc(worker_count, sizeof(*workers));
    if (workers == NULL) {
        perror("calloc");
        return 1;
    }

    for (started_workers = 0; started_workers < worker_count; ++started_workers) {
        if (initialize_worker(&workers[started_workers], (int) started_workers, config->root_dir) != 0) {
            perror("initialize_worker");
            g_stop_server = 1;
            stop_workers(workers, started_workers);
            free(workers);
            return 1;
        }
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        g_stop_server = 1;
        stop_workers(workers, worker_count);
        free(workers);
        return 1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_value, sizeof(opt_value)) != 0) {
        perror("setsockopt");
        close(server_fd);
        g_stop_server = 1;
        stop_workers(workers, worker_count);
        free(workers);
        return 1;
    }

    snprintf(port_text, sizeof(port_text), "%d", config->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;

    if (getaddrinfo(config->host, port_text, &hints, &result) != 0) {
        fprintf(stderr, "invalid listen host: %s\n", config->host);
        close(server_fd);
        g_stop_server = 1;
        stop_workers(workers, worker_count);
        free(workers);
        return 1;
    }

    for (entry = result; entry != NULL; entry = entry->ai_next) {
        if (bind(server_fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            break;
        }
    }
    freeaddrinfo(result);

    if (entry == NULL) {
        perror("bind");
        close(server_fd);
        g_stop_server = 1;
        stop_workers(workers, worker_count);
        free(workers);
        return 1;
    }

    if (listen(server_fd, SERVER_ACCEPT_BACKLOG) != 0) {
        perror("listen");
        close(server_fd);
        g_stop_server = 1;
        stop_workers(workers, worker_count);
        free(workers);
        return 1;
    }

    while (!g_stop_server) {
        int client_fd = accept(server_fd, NULL, NULL);
        worker_context *worker;

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            close(server_fd);
            g_stop_server = 1;
            stop_workers(workers, worker_count);
            free(workers);
            return 1;
        }

        if (set_nonblocking(client_fd) != 0) {
            close(client_fd);
            continue;
        }

        worker = &workers[next_worker++ % worker_count];
        if (enqueue_pending_fd(worker, client_fd) != 0) {
            close(client_fd);
            continue;
        }
        wake_worker(worker);
    }

    close(server_fd);
    stop_workers(workers, worker_count);
    free(workers);
    return 0;
}
