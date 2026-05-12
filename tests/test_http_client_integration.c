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
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>

#include "http_client.h"

static void assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", message);
        exit(1);
    }
}

typedef struct {
    char *data;
    size_t length;
} curl_response_buffer;

static size_t test_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    curl_response_buffer *buffer = (curl_response_buffer *) userp;
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

static void normalize_test_base_url(char *output, size_t output_size, const char *base_url) {
    size_t length = 0;

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

static void assert_bad_md5_rejected(const char *base_url) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    curl_response_buffer response = {0};
    char normalized_base_url[512];
    char url[768];
    long status_code = 0;
    const char payload[] = "bad-md5";

    normalize_test_base_url(normalized_base_url, sizeof(normalized_base_url), base_url);
    snprintf(url, sizeof(url), "%s/v1/write?path=%%2Fdemo%%2Fbad-md5.bin&offset=0", normalized_base_url);

    curl = curl_easy_init();
    assert_true(curl != NULL, "Failed to initialize curl for bad MD5 request");
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    headers = curl_slist_append(headers, "X-HTTPFS-Content-MD5: 00000000000000000000000000000000");
    headers = curl_slist_append(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) (sizeof(payload) - 1));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, test_curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    assert_true(curl_easy_perform(curl) == CURLE_OK, "Bad MD5 request should reach the server");
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    assert_true(status_code == 200, "Bad MD5 request should return HTTP 200 with JSON error");
    assert_true(response.data != NULL && strstr(response.data, "\"status\":\"error\"") != NULL,
                "Bad MD5 request should be rejected");

    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

typedef struct {
    uint32_t state[4];
    uint64_t bit_count;
    unsigned char buffer[64];
} md5_context;

#define MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define MD5_STEP(func, a, b, c, d, x, s, ac)                                                                          \
    do {                                                                                                               \
        (a) += func((b), (c), (d)) + (x) + (uint32_t) (ac);                                                           \
        (a) = MD5_ROTATE_LEFT((a), (s));                                                                               \
        (a) += (b);                                                                                                    \
    } while (0)

static void md5_transform(uint32_t state[4], const unsigned char block[64]) {
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t x[16];
    size_t i;

    for (i = 0; i < 16; ++i) {
        x[i] = (uint32_t) block[i * 4] | ((uint32_t) block[i * 4 + 1] << 8) | ((uint32_t) block[i * 4 + 2] << 16) |
               ((uint32_t) block[i * 4 + 3] << 24);
    }

    MD5_STEP(MD5_F, a, b, c, d, x[0], 7, 0xd76aa478);
    MD5_STEP(MD5_F, d, a, b, c, x[1], 12, 0xe8c7b756);
    MD5_STEP(MD5_F, c, d, a, b, x[2], 17, 0x242070db);
    MD5_STEP(MD5_F, b, c, d, a, x[3], 22, 0xc1bdceee);
    MD5_STEP(MD5_F, a, b, c, d, x[4], 7, 0xf57c0faf);
    MD5_STEP(MD5_F, d, a, b, c, x[5], 12, 0x4787c62a);
    MD5_STEP(MD5_F, c, d, a, b, x[6], 17, 0xa8304613);
    MD5_STEP(MD5_F, b, c, d, a, x[7], 22, 0xfd469501);
    MD5_STEP(MD5_F, a, b, c, d, x[8], 7, 0x698098d8);
    MD5_STEP(MD5_F, d, a, b, c, x[9], 12, 0x8b44f7af);
    MD5_STEP(MD5_F, c, d, a, b, x[10], 17, 0xffff5bb1);
    MD5_STEP(MD5_F, b, c, d, a, x[11], 22, 0x895cd7be);
    MD5_STEP(MD5_F, a, b, c, d, x[12], 7, 0x6b901122);
    MD5_STEP(MD5_F, d, a, b, c, x[13], 12, 0xfd987193);
    MD5_STEP(MD5_F, c, d, a, b, x[14], 17, 0xa679438e);
    MD5_STEP(MD5_F, b, c, d, a, x[15], 22, 0x49b40821);

    MD5_STEP(MD5_G, a, b, c, d, x[1], 5, 0xf61e2562);
    MD5_STEP(MD5_G, d, a, b, c, x[6], 9, 0xc040b340);
    MD5_STEP(MD5_G, c, d, a, b, x[11], 14, 0x265e5a51);
    MD5_STEP(MD5_G, b, c, d, a, x[0], 20, 0xe9b6c7aa);
    MD5_STEP(MD5_G, a, b, c, d, x[5], 5, 0xd62f105d);
    MD5_STEP(MD5_G, d, a, b, c, x[10], 9, 0x02441453);
    MD5_STEP(MD5_G, c, d, a, b, x[15], 14, 0xd8a1e681);
    MD5_STEP(MD5_G, b, c, d, a, x[4], 20, 0xe7d3fbc8);
    MD5_STEP(MD5_G, a, b, c, d, x[9], 5, 0x21e1cde6);
    MD5_STEP(MD5_G, d, a, b, c, x[14], 9, 0xc33707d6);
    MD5_STEP(MD5_G, c, d, a, b, x[3], 14, 0xf4d50d87);
    MD5_STEP(MD5_G, b, c, d, a, x[8], 20, 0x455a14ed);
    MD5_STEP(MD5_G, a, b, c, d, x[13], 5, 0xa9e3e905);
    MD5_STEP(MD5_G, d, a, b, c, x[2], 9, 0xfcefa3f8);
    MD5_STEP(MD5_G, c, d, a, b, x[7], 14, 0x676f02d9);
    MD5_STEP(MD5_G, b, c, d, a, x[12], 20, 0x8d2a4c8a);

    MD5_STEP(MD5_H, a, b, c, d, x[5], 4, 0xfffa3942);
    MD5_STEP(MD5_H, d, a, b, c, x[8], 11, 0x8771f681);
    MD5_STEP(MD5_H, c, d, a, b, x[11], 16, 0x6d9d6122);
    MD5_STEP(MD5_H, b, c, d, a, x[14], 23, 0xfde5380c);
    MD5_STEP(MD5_H, a, b, c, d, x[1], 4, 0xa4beea44);
    MD5_STEP(MD5_H, d, a, b, c, x[4], 11, 0x4bdecfa9);
    MD5_STEP(MD5_H, c, d, a, b, x[7], 16, 0xf6bb4b60);
    MD5_STEP(MD5_H, b, c, d, a, x[10], 23, 0xbebfbc70);
    MD5_STEP(MD5_H, a, b, c, d, x[13], 4, 0x289b7ec6);
    MD5_STEP(MD5_H, d, a, b, c, x[0], 11, 0xeaa127fa);
    MD5_STEP(MD5_H, c, d, a, b, x[3], 16, 0xd4ef3085);
    MD5_STEP(MD5_H, b, c, d, a, x[6], 23, 0x04881d05);
    MD5_STEP(MD5_H, a, b, c, d, x[9], 4, 0xd9d4d039);
    MD5_STEP(MD5_H, d, a, b, c, x[12], 11, 0xe6db99e5);
    MD5_STEP(MD5_H, c, d, a, b, x[15], 16, 0x1fa27cf8);
    MD5_STEP(MD5_H, b, c, d, a, x[2], 23, 0xc4ac5665);

    MD5_STEP(MD5_I, a, b, c, d, x[0], 6, 0xf4292244);
    MD5_STEP(MD5_I, d, a, b, c, x[7], 10, 0x432aff97);
    MD5_STEP(MD5_I, c, d, a, b, x[14], 15, 0xab9423a7);
    MD5_STEP(MD5_I, b, c, d, a, x[5], 21, 0xfc93a039);
    MD5_STEP(MD5_I, a, b, c, d, x[12], 6, 0x655b59c3);
    MD5_STEP(MD5_I, d, a, b, c, x[3], 10, 0x8f0ccc92);
    MD5_STEP(MD5_I, c, d, a, b, x[10], 15, 0xffeff47d);
    MD5_STEP(MD5_I, b, c, d, a, x[1], 21, 0x85845dd1);
    MD5_STEP(MD5_I, a, b, c, d, x[8], 6, 0x6fa87e4f);
    MD5_STEP(MD5_I, d, a, b, c, x[15], 10, 0xfe2ce6e0);
    MD5_STEP(MD5_I, c, d, a, b, x[6], 15, 0xa3014314);
    MD5_STEP(MD5_I, b, c, d, a, x[13], 21, 0x4e0811a1);
    MD5_STEP(MD5_I, a, b, c, d, x[4], 6, 0xf7537e82);
    MD5_STEP(MD5_I, d, a, b, c, x[11], 10, 0xbd3af235);
    MD5_STEP(MD5_I, c, d, a, b, x[2], 15, 0x2ad7d2bb);
    MD5_STEP(MD5_I, b, c, d, a, x[9], 21, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(md5_context *ctx) {
    ctx->bit_count = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(md5_context *ctx, const unsigned char *input, size_t length) {
    size_t index = (size_t) ((ctx->bit_count >> 3) & 0x3fU);
    size_t part_len = 64U - index;
    size_t i = 0;

    ctx->bit_count += (uint64_t) length << 3;
    if (length >= part_len) {
        memcpy(&ctx->buffer[index], input, part_len);
        md5_transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 63U < length; i += 64U) {
            md5_transform(ctx->state, &input[i]);
        }
        index = 0;
    }
    memcpy(&ctx->buffer[index], &input[i], length - i);
}

static void md5_final(md5_context *ctx, unsigned char digest[16]) {
    static const unsigned char padding[64] = {0x80};
    unsigned char bits[8];
    size_t index;
    size_t pad_len;
    size_t i;

    for (i = 0; i < 8; ++i) {
        bits[i] = (unsigned char) ((ctx->bit_count >> (i * 8U)) & 0xffU);
    }

    index = (size_t) ((ctx->bit_count >> 3) & 0x3fU);
    pad_len = index < 56U ? (56U - index) : (120U - index);
    md5_update(ctx, padding, pad_len);
    md5_update(ctx, bits, sizeof(bits));

    for (i = 0; i < 4; ++i) {
        digest[i * 4] = (unsigned char) (ctx->state[i] & 0xffU);
        digest[i * 4 + 1] = (unsigned char) ((ctx->state[i] >> 8) & 0xffU);
        digest[i * 4 + 2] = (unsigned char) ((ctx->state[i] >> 16) & 0xffU);
        digest[i * 4 + 3] = (unsigned char) ((ctx->state[i] >> 24) & 0xffU);
    }
}

static void compute_sparse_md5_hex(const unsigned char *data, size_t size, char output[33]) {
    static const char hex_digits[] = "0123456789abcdef";
    md5_context ctx;
    unsigned char digest[16];
    size_t i;

    md5_init(&ctx);
    for (i = 0; i < size; i += 10U) {
        md5_update(&ctx, &data[i], 1);
    }
    md5_final(&ctx, digest);

    for (i = 0; i < 16; ++i) {
        output[i * 2] = hex_digits[digest[i] >> 4];
        output[i * 2 + 1] = hex_digits[digest[i] & 0x0fU];
    }
    output[32] = '\0';
}

int main(void) {
    const char *base_url = getenv("HTTPFS_BASE_URL");
    httpfs_client client;
    httpfs_error error;
    httpfs_meta meta;
    httpfs_dir_entry entries[64];
    size_t entry_count = 0;
    unsigned char read_buffer[96];
    size_t bytes_read = 0;
    size_t bytes_written = 0;
    const unsigned char payload[] = "hello-httpfs";
    unsigned char *large_payload = NULL;
    unsigned char *large_read_buffer = NULL;
    const size_t large_payload_size = 2U * 1024U * 1024U + 123U;
    char payload_md5[33];
    char read_md5[33];
    char large_payload_md5[33];
    char large_read_md5[33];
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
    compute_sparse_md5_hex(payload, sizeof(payload) - 1, payload_md5);

    assert_true(httpfs_get_meta(&client, "/demo/hello.txt", &meta, &error) == 0, error.message);
    assert_true(meta.size == (off_t) (sizeof(payload) - 1), "File size should match");

    assert_true(httpfs_read_file(&client, "/demo/hello.txt", 0, sizeof(read_buffer), read_buffer, &bytes_read, &error) ==
                    0,
                error.message);
    assert_true(bytes_read == sizeof(payload) - 1, "Read size should match");
    assert_true(memcmp(read_buffer, payload, bytes_read) == 0, "Read content should match");
    compute_sparse_md5_hex(read_buffer, bytes_read, read_md5);
    assert_true(strcmp(read_md5, payload_md5) == 0, "Sampled MD5 should match for hello.txt");

    large_payload = malloc(large_payload_size);
    assert_true(large_payload != NULL, "Failed to allocate large payload");
    large_read_buffer = malloc(large_payload_size);
    assert_true(large_read_buffer != NULL, "Failed to allocate large read buffer");
    for (i = 0; i < large_payload_size; ++i) {
        large_payload[i] = (unsigned char) (i % 251);
    }
    compute_sparse_md5_hex(large_payload, large_payload_size, large_payload_md5);
    assert_true(httpfs_create_file(&client, "/demo/large.bin", 0644, &error) == 0, error.message);
    assert_true(httpfs_create_file(&client, "/demo/bad-md5.bin", 0644, &error) == 0, error.message);
    assert_bad_md5_rejected(base_url);
    assert_true(httpfs_get_meta(&client, "/demo/bad-md5.bin", &meta, &error) == 0, error.message);
    assert_true(meta.size == 0, "Rejected bad MD5 write should not change file size");
    assert_true(httpfs_remove_file(&client, "/demo/bad-md5.bin", &error) == 0, error.message);
    assert_true(httpfs_write_file(&client, "/demo/large.bin", 0, large_payload, large_payload_size, &bytes_written, &error) ==
                    0,
                error.message);
    assert_true(bytes_written == large_payload_size, "Large write size should match");
    assert_true(httpfs_get_meta(&client, "/demo/large.bin", &meta, &error) == 0, error.message);
    assert_true(meta.size == (off_t) large_payload_size, "Large file size should match");
    assert_true(httpfs_read_file(&client, "/demo/large.bin", 0, large_payload_size, large_read_buffer, &bytes_read, &error) ==
                    0,
                error.message);
    assert_true(bytes_read == large_payload_size, "Large read size should match");
    assert_true(memcmp(large_read_buffer, large_payload, large_payload_size) == 0, "Large read content should match");
    compute_sparse_md5_hex(large_read_buffer, bytes_read, large_read_md5);
    assert_true(strcmp(large_read_md5, large_payload_md5) == 0, "Sampled MD5 should match for large.bin");
    assert_true(httpfs_remove_file(&client, "/demo/large.bin", &error) == 0, error.message);
    free(large_payload);
    large_payload = NULL;
    free(large_read_buffer);
    large_read_buffer = NULL;

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
    free(large_payload);
    free(large_read_buffer);

    puts("test_http_client_integration: PASS");
    return 0;
}
