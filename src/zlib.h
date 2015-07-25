#ifndef PNG_ZLIB_H
#define PNG_ZLIB_H

#include <stdint.h>
#include <sys/types.h>

ssize_t zlib_decompress(uint8_t *src, size_t src_size, uint8_t **dst);

#endif /* PNG_ZLIB_H */
