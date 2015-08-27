#ifndef PNG_ZLIB_H
#define PNG_ZLIB_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct zlib_stream {
        /* public fields */
        const uint8_t *z_src;
        size_t z_src_idx;
        char z_src_bidx; /* 0-7: index of the first bit we have not read */
        size_t z_src_end;

        uint8_t *z_dst;
        size_t z_dst_idx;
        size_t z_dst_end;

        /* internal fields */
        size_t wsize;

        /* length/litteral tree */
        struct huff_tree *z_lltree;

        /* distance tree */
        struct huff_tree *z_dtree;
};

int zlib_decompress(struct zlib_stream *stream);

#endif /* PNG_ZLIB_H */
