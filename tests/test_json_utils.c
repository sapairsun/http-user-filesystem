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

#include "json_utils.h"

static void assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", message);
        exit(1);
    }
}

int main(void) {
    char escaped[128];
    char encoded[128];
    unsigned char decoded[128];
    size_t decoded_len = 0;
    const unsigned char sample[] = {'h', 'e', 'l', 'l', 'o', '\n'};

    assert_true(json_escape_string("a\"b\\c\n", escaped, sizeof(escaped)) == 0, "JSON escaping should succeed");
    assert_true(strcmp(escaped, "a\\\"b\\\\c\\n") == 0, "JSON escaping result should match");

    assert_true(hex_encode(sample, sizeof(sample), encoded, sizeof(encoded)) == 0, "Hex encoding should succeed");
    assert_true(strcmp(encoded, "68656c6c6f0a") == 0, "Hex encoding result should match");

    assert_true(hex_decode(encoded, decoded, sizeof(decoded), &decoded_len) == 0, "Hex decoding should succeed");
    assert_true(decoded_len == sizeof(sample), "Hex decoded length should match");
    assert_true(memcmp(decoded, sample, sizeof(sample)) == 0, "Hex decoded content should match");

    puts("test_json_utils: PASS");
    return 0;
}
