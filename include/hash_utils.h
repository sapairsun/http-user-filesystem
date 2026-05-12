#ifndef HASH_UTILS_H
#define HASH_UTILS_H

#include <stddef.h>
#include <sys/types.h>

int httpfs_sparse_md5_hex(const unsigned char *data, size_t size, off_t base_offset, char output[33]);
int httpfs_is_valid_md5_hex(const char *value);

#endif
