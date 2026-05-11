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

#ifndef JSON_UTILS_H
#define JSON_UTILS_H

#include <stddef.h>

int json_escape_string(const char *input, char *output, size_t output_size);
int hex_encode(const unsigned char *input, size_t input_len, char *output, size_t output_size);
int hex_decode(const char *input, unsigned char *output, size_t output_size, size_t *decoded_len);

#endif
