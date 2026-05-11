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

#include "json_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int json_escape_string(const char *input, char *output, size_t output_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;

    if (output_size == 0) {
        return -1;
    }

    while (input[in_pos] != '\0') {
        unsigned char ch = (unsigned char) input[in_pos];

        if (out_pos + 2 >= output_size) {
            return -1;
        }

        switch (ch) {
            case '\\':
            case '"':
                output[out_pos++] = '\\';
                output[out_pos++] = (char) ch;
                break;
            case '\b':
                output[out_pos++] = '\\';
                output[out_pos++] = 'b';
                break;
            case '\f':
                output[out_pos++] = '\\';
                output[out_pos++] = 'f';
                break;
            case '\n':
                output[out_pos++] = '\\';
                output[out_pos++] = 'n';
                break;
            case '\r':
                output[out_pos++] = '\\';
                output[out_pos++] = 'r';
                break;
            case '\t':
                output[out_pos++] = '\\';
                output[out_pos++] = 't';
                break;
            default:
                if (ch < 0x20) {
                    if (out_pos + 6 >= output_size) {
                        return -1;
                    }
                    snprintf(output + out_pos, output_size - out_pos, "\\u%04x", ch);
                    out_pos += 6;
                } else {
                    output[out_pos++] = (char) ch;
                }
                break;
        }

        in_pos++;
    }

    output[out_pos] = '\0';
    return 0;
}

static int hex_nibble(char ch) {
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

int hex_encode(const unsigned char *input, size_t input_len, char *output, size_t output_size) {
    static const char hex_digits[] = "0123456789abcdef";
    size_t i;

    if (output_size < input_len * 2 + 1) {
        return -1;
    }

    for (i = 0; i < input_len; ++i) {
        output[i * 2] = hex_digits[(input[i] >> 4) & 0x0f];
        output[i * 2 + 1] = hex_digits[input[i] & 0x0f];
    }

    output[input_len * 2] = '\0';
    return 0;
}

int hex_decode(const char *input, unsigned char *output, size_t output_size, size_t *decoded_len) {
    size_t input_len = strlen(input);
    size_t i;

    if ((input_len % 2) != 0) {
        return -1;
    }

    if (output_size < input_len / 2) {
        return -1;
    }

    for (i = 0; i < input_len; i += 2) {
        int high = hex_nibble(input[i]);
        int low = hex_nibble(input[i + 1]);

        if (high < 0 || low < 0) {
            return -1;
        }

        output[i / 2] = (unsigned char) ((high << 4) | low);
    }

    if (decoded_len != NULL) {
        *decoded_len = input_len / 2;
    }

    return 0;
}
