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

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_utils.h"
#include "../third_party/jsmn.h"

typedef struct {
    char *data;
    size_t length;
} response_buffer;

static void normalize_base_url(char *output, size_t output_size, const char *base_url) {
    size_t length = 0;

    if (output_size == 0) {
        return;
    }

    if (base_url == NULL) {
        output[0] = '\0';
        return;
    }

    snprintf(output, output_size, "%s", base_url);
    length = strlen(output);

    while (length > 0 && output[length - 1] == '/') {
        output[--length] = '\0';
    }

    if (length >= 3 && strcmp(output + length - 3, "/v1") == 0) {
        output[length - 3] = '\0';
        length -= 3;
    }

    while (length > 0 && output[length - 1] == '/') {
        output[--length] = '\0';
    }
}

static void set_error(httpfs_error *error, long errno_value, const char *message) {
    if (error == NULL) {
        return;
    }

    error->errno_value = errno_value;
    if (message == NULL) {
        error->message[0] = '\0';
        return;
    }

    snprintf(error->message, sizeof(error->message), "%s", message);
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    response_buffer *buffer = (response_buffer *) userp;
    char *new_data = realloc(buffer->data, buffer->length + total_size + 1);

    if (new_data == NULL) {
        return 0;
    }

    buffer->data = new_data;
    memcpy(buffer->data + buffer->length, contents, total_size);
    buffer->length += total_size;
    buffer->data[buffer->length] = '\0';
    return total_size;
}

static int token_equals(const char *json, const jsmntok_t *token, const char *value) {
    size_t length = strlen(value);
    return token->start >= 0 && token->end >= token->start &&
           (size_t) (token->end - token->start) == length &&
           strncmp(json + token->start, value, length) == 0;
}

static int token_to_string(const char *json, const jsmntok_t *token, char *output, size_t output_size) {
    size_t length;

    if (token->start < 0 || token->end < token->start) {
        return -1;
    }

    length = (size_t) (token->end - token->start);
    if (length + 1 > output_size) {
        return -1;
    }

    memcpy(output, json + token->start, length);
    output[length] = '\0';
    return 0;
}

static int token_to_long(const char *json, const jsmntok_t *token, long *value) {
    char buffer[64];
    char *endptr = NULL;

    if (token_to_string(json, token, buffer, sizeof(buffer)) != 0) {
        return -1;
    }

    *value = strtol(buffer, &endptr, 10);
    if (endptr == buffer || *endptr != '\0') {
        return -1;
    }
    return 0;
}

static int next_token_index(jsmntok_t *tokens, int count, int index) {
    int end = tokens[index].end;
    int i = index + 1;
    while (i < count && tokens[i].start < end) {
        i++;
    }
    return i;
}

static int find_object_value(const char *json, jsmntok_t *tokens, int count, int object_index, const char *key) {
    int i = object_index + 1;
    int end = tokens[object_index].end;

    while (i < count && tokens[i].start < end) {
        if (tokens[i].parent == object_index && tokens[i].type == JSMN_STRING && token_equals(json, &tokens[i], key)) {
            int value_index = i + 1;
            if (value_index < count) {
                return value_index;
            }
            return -1;
        }

        if (tokens[i].parent == object_index && tokens[i].type == JSMN_STRING) {
            i = next_token_index(tokens, count, i + 1);
        } else {
            i++;
        }
    }

    return -1;
}

static int parse_response_tokens(const char *json, jsmntok_t *tokens, int max_tokens, httpfs_error *error) {
    jsmn_parser parser;
    int token_count;

    jsmn_init(&parser);
    token_count = jsmn_parse(&parser, json, strlen(json), tokens, (unsigned int) max_tokens);
    if (token_count < 0) {
        set_error(error, EIO, "Server returned invalid JSON");
        return -1;
    }

    if (token_count < 1 || tokens[0].type != JSMN_OBJECT) {
        set_error(error, EIO, "Server returned an invalid response format");
        return -1;
    }

    return token_count;
}

static int parse_status_or_error(const char *json, jsmntok_t *tokens, int token_count, httpfs_error *error) {
    int status_index = find_object_value(json, tokens, token_count, 0, "status");
    if (status_index < 0) {
        set_error(error, EIO, "Server response is missing the status field");
        return -1;
    }

    if (token_equals(json, &tokens[status_index], "ok")) {
        return 0;
    }

    if (token_equals(json, &tokens[status_index], "error")) {
        int errno_index = find_object_value(json, tokens, token_count, 0, "errno");
        int message_index = find_object_value(json, tokens, token_count, 0, "message");
        long errno_value = EIO;
        char message[HTTPFS_MAX_MESSAGE] = "Server returned an error";

        if (errno_index >= 0) {
            (void) token_to_long(json, &tokens[errno_index], &errno_value);
        }
        if (message_index >= 0) {
            (void) token_to_string(json, &tokens[message_index], message, sizeof(message));
        }

        set_error(error, errno_value, message);
        return -1;
    }

    set_error(error, EIO, "Server returned an unknown status");
    return -1;
}

static int http_request(httpfs_client *client, const char *method, const char *endpoint, const char *body,
                        long *status_code, char **response_out, httpfs_error *error) {
    CURL *curl = NULL;
    CURLcode curl_result;
    struct curl_slist *headers = NULL;
    response_buffer response = {0};
    char url[1024];

    if (snprintf(url, sizeof(url), "%s%s", client->base_url, endpoint) >= (int) sizeof(url)) {
        set_error(error, ENAMETOOLONG, "Request URL is too long");
        return -1;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        set_error(error, EIO, "Failed to initialize curl");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, client->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, client->verbose ? 1L : 0L);

    if (strcmp(method, "POST") == 0) {
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body != NULL ? body : "");
    }

    curl_result = curl_easy_perform(curl);
    if (curl_result != CURLE_OK) {
        set_error(error, EIO, curl_easy_strerror(curl_result));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(response.data);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (response.data == NULL) {
        response.data = malloc(1);
        if (response.data == NULL) {
            set_error(error, ENOMEM, "Failed to allocate response buffer");
            return -1;
        }
        response.data[0] = '\0';
    }

    *response_out = response.data;
    return 0;
}

static int build_read_endpoint(const char *path, off_t offset, size_t size, char *endpoint, size_t endpoint_size) {
    CURL *curl = curl_easy_init();
    char *escaped_path;
    int written;

    if (curl == NULL) {
        return -1;
    }

    escaped_path = curl_easy_escape(curl, path, 0);
    if (escaped_path == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }

    written = snprintf(endpoint, endpoint_size, "/v1/read?path=%s&offset=%lld&size=%zu", escaped_path,
                       (long long) offset, size);

    curl_free(escaped_path);
    curl_easy_cleanup(curl);
    return (written >= 0 && (size_t) written < endpoint_size) ? 0 : -1;
}

static int build_simple_get_endpoint(const char *prefix, const char *path, char *endpoint, size_t endpoint_size) {
    CURL *curl = curl_easy_init();
    char *escaped_path;
    int written;

    if (curl == NULL) {
        return -1;
    }

    escaped_path = curl_easy_escape(curl, path, 0);
    if (escaped_path == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }

    written = snprintf(endpoint, endpoint_size, "%s?path=%s", prefix, escaped_path);
    curl_free(escaped_path);
    curl_easy_cleanup(curl);

    return (written >= 0 && (size_t) written < endpoint_size) ? 0 : -1;
}

static int parse_meta_object(const char *json, jsmntok_t *tokens, int token_count, int meta_index, httpfs_meta *meta) {
    int path_index = find_object_value(json, tokens, token_count, meta_index, "path");
    int type_index = find_object_value(json, tokens, token_count, meta_index, "type");
    int mode_index = find_object_value(json, tokens, token_count, meta_index, "mode");
    int size_index = find_object_value(json, tokens, token_count, meta_index, "size");
    int uid_index = find_object_value(json, tokens, token_count, meta_index, "uid");
    int gid_index = find_object_value(json, tokens, token_count, meta_index, "gid");
    int nlink_index = find_object_value(json, tokens, token_count, meta_index, "nlink");
    int atime_index = find_object_value(json, tokens, token_count, meta_index, "atime");
    int mtime_index = find_object_value(json, tokens, token_count, meta_index, "mtime");
    int ctime_index = find_object_value(json, tokens, token_count, meta_index, "ctime");
    long number;

    if (path_index < 0 || type_index < 0 || mode_index < 0 || size_index < 0 || uid_index < 0 || gid_index < 0 ||
        nlink_index < 0 || atime_index < 0 || mtime_index < 0 || ctime_index < 0) {
        return -1;
    }

    if (token_to_string(json, &tokens[path_index], meta->path, sizeof(meta->path)) != 0 ||
        token_to_string(json, &tokens[type_index], meta->type, sizeof(meta->type)) != 0) {
        return -1;
    }

    if (token_to_long(json, &tokens[mode_index], &number) != 0) {
        return -1;
    }
    meta->mode = (mode_t) number;

    if (token_to_long(json, &tokens[size_index], &number) != 0) {
        return -1;
    }
    meta->size = (off_t) number;

    if (token_to_long(json, &tokens[uid_index], &number) != 0) {
        return -1;
    }
    meta->uid = (uid_t) number;

    if (token_to_long(json, &tokens[gid_index], &number) != 0) {
        return -1;
    }
    meta->gid = (gid_t) number;

    if (token_to_long(json, &tokens[nlink_index], &number) != 0) {
        return -1;
    }
    meta->nlink = (nlink_t) number;

    if (token_to_long(json, &tokens[atime_index], &number) != 0) {
        return -1;
    }
    meta->atime = (time_t) number;

    if (token_to_long(json, &tokens[mtime_index], &number) != 0) {
        return -1;
    }
    meta->mtime = (time_t) number;

    if (token_to_long(json, &tokens[ctime_index], &number) != 0) {
        return -1;
    }
    meta->ctime = (time_t) number;

    return 0;
}

static int parse_ok_response(const char *json, httpfs_error *error, jsmntok_t *tokens, int *token_count_out) {
    int token_count = parse_response_tokens(json, tokens, 512, error);
    if (token_count < 0) {
        return -1;
    }

    if (parse_status_or_error(json, tokens, token_count, error) != 0) {
        return -1;
    }

    *token_count_out = token_count;
    return 0;
}

void httpfs_client_init(httpfs_client *client, const char *base_url, long timeout_ms) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    memset(client, 0, sizeof(*client));
    normalize_base_url(client->base_url, sizeof(client->base_url), base_url);
    client->timeout_ms = timeout_ms;
    client->verbose = 0;
}

void httpfs_client_set_verbose(httpfs_client *client, int verbose) {
    client->verbose = verbose;
}

int httpfs_get_meta(httpfs_client *client, const char *path, httpfs_meta *meta, httpfs_error *error) {
    char endpoint[1024];
    char *response = NULL;
    long status_code = 0;
    jsmntok_t tokens[512];
    int token_count;
    int meta_index;
    int result = -1;

    if (build_simple_get_endpoint("/v1/meta", path, endpoint, sizeof(endpoint)) != 0) {
        set_error(error, ENAMETOOLONG, "Failed to build meta request");
        return -1;
    }

    if (http_request(client, "GET", endpoint, NULL, &status_code, &response, error) != 0) {
        return -1;
    }

    if (status_code != 200) {
        set_error(error, EIO, "Meta request returned a non-200 status");
        goto out;
    }

    if (parse_ok_response(response, error, tokens, &token_count) != 0) {
        goto out;
    }

    meta_index = find_object_value(response, tokens, token_count, 0, "meta");
    if (meta_index < 0 || parse_meta_object(response, tokens, token_count, meta_index, meta) != 0) {
        set_error(error, EIO, "Meta response is missing valid metadata");
        goto out;
    }

    result = 0;

out:
    free(response);
    return result;
}

int httpfs_list_dir(httpfs_client *client, const char *path, httpfs_dir_entry *entries, size_t max_entries,
                    size_t *entry_count, httpfs_error *error) {
    char endpoint[1024];
    char *response = NULL;
    long status_code = 0;
    jsmntok_t tokens[1024];
    int token_count;
    int entries_index;
    int array_end;
    int i;
    size_t used = 0;
    int result = -1;

    if (build_simple_get_endpoint("/v1/list", path, endpoint, sizeof(endpoint)) != 0) {
        set_error(error, ENAMETOOLONG, "Failed to build list request");
        return -1;
    }

    if (http_request(client, "GET", endpoint, NULL, &status_code, &response, error) != 0) {
        return -1;
    }

    if (status_code != 200) {
        set_error(error, EIO, "List request returned a non-200 status");
        goto out;
    }

    if (parse_ok_response(response, error, tokens, &token_count) != 0) {
        goto out;
    }

    entries_index = find_object_value(response, tokens, token_count, 0, "entries");
    if (entries_index < 0 || tokens[entries_index].type != JSMN_ARRAY) {
        set_error(error, EIO, "List response is missing the entries array");
        goto out;
    }

    array_end = tokens[entries_index].end;
    i = entries_index + 1;
    while (i < token_count && tokens[i].start < array_end) {
        int name_index;
        int type_index;

        if (tokens[i].parent != entries_index || tokens[i].type != JSMN_OBJECT) {
            i++;
            continue;
        }

        if (used >= max_entries) {
            set_error(error, ENOBUFS, "Directory entry buffer is too small");
            goto out;
        }

        name_index = find_object_value(response, tokens, token_count, i, "name");
        type_index = find_object_value(response, tokens, token_count, i, "type");
        if (name_index < 0 || type_index < 0 ||
            token_to_string(response, &tokens[name_index], entries[used].name, sizeof(entries[used].name)) != 0 ||
            token_to_string(response, &tokens[type_index], entries[used].type, sizeof(entries[used].type)) != 0) {
            set_error(error, EIO, "Failed to parse directory entry");
            goto out;
        }

        used++;
        i = next_token_index(tokens, token_count, i);
    }

    *entry_count = used;
    result = 0;

out:
    free(response);
    return result;
}

int httpfs_read_file(httpfs_client *client, const char *path, off_t offset, size_t size, unsigned char *buffer,
                     size_t *bytes_read, httpfs_error *error) {
    char endpoint[1024];
    char *response = NULL;
    long status_code = 0;
    jsmntok_t tokens[512];
    int token_count;
    int data_index;
    int bytes_read_index;
    char data_hex[131072];
    long server_bytes_read = 0;
    size_t decoded_len = 0;
    int result = -1;

    if (size * 2 + 1 > sizeof(data_hex)) {
        set_error(error, ENOBUFS, "Read size exceeds the current implementation limit");
        return -1;
    }

    if (build_read_endpoint(path, offset, size, endpoint, sizeof(endpoint)) != 0) {
        set_error(error, ENAMETOOLONG, "Failed to build read request");
        return -1;
    }

    if (http_request(client, "GET", endpoint, NULL, &status_code, &response, error) != 0) {
        return -1;
    }

    if (status_code != 200) {
        set_error(error, EIO, "Read request returned a non-200 status");
        goto out;
    }

    if (parse_ok_response(response, error, tokens, &token_count) != 0) {
        goto out;
    }

    data_index = find_object_value(response, tokens, token_count, 0, "data_hex");
    bytes_read_index = find_object_value(response, tokens, token_count, 0, "bytes_read");
    if (data_index < 0 || bytes_read_index < 0) {
        set_error(error, EIO, "Read response is missing required fields");
        goto out;
    }

    if (token_to_string(response, &tokens[data_index], data_hex, sizeof(data_hex)) != 0 ||
        token_to_long(response, &tokens[bytes_read_index], &server_bytes_read) != 0) {
        set_error(error, EIO, "Failed to parse read response");
        goto out;
    }

    if (hex_decode(data_hex, buffer, size, &decoded_len) != 0 || decoded_len != (size_t) server_bytes_read) {
        set_error(error, EIO, "Failed to decode read data from hex");
        goto out;
    }

    *bytes_read = decoded_len;
    result = 0;

out:
    free(response);
    return result;
}

int httpfs_write_file(httpfs_client *client, const char *path, off_t offset, const unsigned char *buffer, size_t size,
                      size_t *bytes_written, httpfs_error *error) {
    char escaped_path[HTTPFS_MAX_PATH * 2];
    char *data_hex = NULL;
    char *body = NULL;
    char *response = NULL;
    long status_code = 0;
    jsmntok_t tokens[256];
    int token_count;
    int bytes_written_index;
    long server_bytes_written = 0;
    size_t body_size;
    int result = -1;

    data_hex = malloc(size * 2 + 1);
    if (data_hex == NULL) {
        set_error(error, ENOMEM, "Failed to allocate write buffer");
        return -1;
    }

    if (json_escape_string(path, escaped_path, sizeof(escaped_path)) != 0 ||
        hex_encode(buffer, size, data_hex, size * 2 + 1) != 0) {
        set_error(error, EINVAL, "Failed to build write request");
        goto out;
    }

    body_size = strlen(escaped_path) + strlen(data_hex) + 128;
    body = malloc(body_size);
    if (body == NULL) {
        set_error(error, ENOMEM, "Failed to allocate request body");
        goto out;
    }

    snprintf(body, body_size, "{\"path\":\"%s\",\"offset\":%lld,\"data_hex\":\"%s\"}", escaped_path, (long long) offset,
             data_hex);

    if (http_request(client, "POST", "/v1/write", body, &status_code, &response, error) != 0) {
        goto out;
    }

    if (status_code != 200) {
        set_error(error, EIO, "Write request returned a non-200 status");
        goto out;
    }

    if (parse_ok_response(response, error, tokens, &token_count) != 0) {
        goto out;
    }

    bytes_written_index = find_object_value(response, tokens, token_count, 0, "bytes_written");
    if (bytes_written_index < 0 || token_to_long(response, &tokens[bytes_written_index], &server_bytes_written) != 0) {
        set_error(error, EIO, "Write response is missing bytes_written");
        goto out;
    }

    *bytes_written = (size_t) server_bytes_written;
    result = 0;

out:
    free(response);
    free(body);
    free(data_hex);
    return result;
}

static int post_path_only(httpfs_client *client, const char *endpoint, const char *path, long mode, int with_mode,
                          httpfs_error *error) {
    char escaped_path[HTTPFS_MAX_PATH * 2];
    char body[HTTPFS_MAX_PATH * 2 + 64];
    char *response = NULL;
    long status_code = 0;
    jsmntok_t tokens[128];
    int token_count;
    int result = -1;

    if (json_escape_string(path, escaped_path, sizeof(escaped_path)) != 0) {
        set_error(error, EINVAL, "Failed to escape request path");
        return -1;
    }

    if (with_mode) {
        snprintf(body, sizeof(body), "{\"path\":\"%s\",\"mode\":%ld}", escaped_path, mode);
    } else {
        snprintf(body, sizeof(body), "{\"path\":\"%s\"}", escaped_path);
    }

    if (http_request(client, "POST", endpoint, body, &status_code, &response, error) != 0) {
        return -1;
    }

    if (status_code != 200) {
        set_error(error, EIO, "Request returned a non-200 status");
        goto out;
    }

    if (parse_ok_response(response, error, tokens, &token_count) != 0) {
        goto out;
    }

    result = 0;

out:
    free(response);
    return result;
}

int httpfs_create_file(httpfs_client *client, const char *path, mode_t mode, httpfs_error *error) {
    return post_path_only(client, "/v1/create-file", path, (long) mode, 1, error);
}

int httpfs_create_dir(httpfs_client *client, const char *path, mode_t mode, httpfs_error *error) {
    return post_path_only(client, "/v1/create-dir", path, (long) mode, 1, error);
}

int httpfs_remove_file(httpfs_client *client, const char *path, httpfs_error *error) {
    return post_path_only(client, "/v1/remove-file", path, 0, 0, error);
}

int httpfs_remove_dir(httpfs_client *client, const char *path, httpfs_error *error) {
    return post_path_only(client, "/v1/remove-dir", path, 0, 0, error);
}

int httpfs_rename_path(httpfs_client *client, const char *old_path, const char *new_path, httpfs_error *error) {
    char escaped_old[HTTPFS_MAX_PATH * 2];
    char escaped_new[HTTPFS_MAX_PATH * 2];
    char body[HTTPFS_MAX_PATH * 4 + 64];
    char *response = NULL;
    long status_code = 0;
    jsmntok_t tokens[128];
    int token_count;
    int result = -1;

    if (json_escape_string(old_path, escaped_old, sizeof(escaped_old)) != 0 ||
        json_escape_string(new_path, escaped_new, sizeof(escaped_new)) != 0) {
        set_error(error, EINVAL, "Failed to escape rename request path");
        return -1;
    }

    snprintf(body, sizeof(body), "{\"old_path\":\"%s\",\"new_path\":\"%s\"}", escaped_old, escaped_new);

    if (http_request(client, "POST", "/v1/rename", body, &status_code, &response, error) != 0) {
        return -1;
    }

    if (status_code != 200) {
        set_error(error, EIO, "Rename request returned a non-200 status");
        goto out;
    }

    if (parse_ok_response(response, error, tokens, &token_count) != 0) {
        goto out;
    }

    result = 0;

out:
    free(response);
    return result;
}

int httpfs_truncate_file(httpfs_client *client, const char *path, off_t size, httpfs_error *error) {
    char escaped_path[HTTPFS_MAX_PATH * 2];
    char body[HTTPFS_MAX_PATH * 2 + 64];
    char *response = NULL;
    long status_code = 0;
    jsmntok_t tokens[128];
    int token_count;
    int result = -1;

    if (json_escape_string(path, escaped_path, sizeof(escaped_path)) != 0) {
        set_error(error, EINVAL, "Failed to escape truncate request path");
        return -1;
    }

    snprintf(body, sizeof(body), "{\"path\":\"%s\",\"size\":%lld}", escaped_path, (long long) size);

    if (http_request(client, "POST", "/v1/truncate", body, &status_code, &response, error) != 0) {
        return -1;
    }

    if (status_code != 200) {
        set_error(error, EIO, "Truncate request returned a non-200 status");
        goto out;
    }

    if (parse_ok_response(response, error, tokens, &token_count) != 0) {
        goto out;
    }

    result = 0;

out:
    free(response);
    return result;
}

int httpfs_update_times(httpfs_client *client, const char *path, const struct timespec times[2], httpfs_error *error) {
    char escaped_path[HTTPFS_MAX_PATH * 2];
    char body[HTTPFS_MAX_PATH * 2 + 256];
    char *response = NULL;
    long status_code = 0;
    jsmntok_t tokens[128];
    int token_count;
    int result = -1;

    if (times == NULL) {
        set_error(error, EINVAL, "Missing timespec array");
        return -1;
    }

    if (json_escape_string(path, escaped_path, sizeof(escaped_path)) != 0) {
        set_error(error, EINVAL, "Failed to escape utimens request path");
        return -1;
    }

    snprintf(body, sizeof(body),
             "{\"path\":\"%s\",\"atime_sec\":%lld,\"atime_nsec\":%ld,\"mtime_sec\":%lld,\"mtime_nsec\":%ld}",
             escaped_path, (long long) times[0].tv_sec, times[0].tv_nsec, (long long) times[1].tv_sec, times[1].tv_nsec);

    if (http_request(client, "POST", "/v1/utimens", body, &status_code, &response, error) != 0) {
        return -1;
    }

    if (status_code != 200) {
        set_error(error, EIO, "Utimens request returned a non-200 status");
        goto out;
    }

    if (parse_ok_response(response, error, tokens, &token_count) != 0) {
        goto out;
    }

    result = 0;

out:
    free(response);
    return result;
}
