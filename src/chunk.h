#ifndef PNG_CHUNK_H
#define PNG_CHUNK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * limits in bytes for chunk size. this is the size of the whole chunk --
 * *including* the length, type, and crc fields. This is in contrast to
 * the actual length field within the chunk.
 * 
 * at a minnimum, a chunk has length, type, and crc, each of which are 4
 * bytes. the length is a png integer (max valid size 2^31 - 1), so the
 * maximum valid size is 2^31 - 1 + 12
 */
#define MIN_CHUNK_SIZE ((size_t)12)
#define MAX_CHUNK_SIZE ((size_t)((1 << 31) + 11))

/* simple ints so we can have arrays of chunks */
enum chunk_enum {
        CHUNK_IHDR = 0,
        CHUNK_PLTE,
        CHUNK_IDAT,
        CHUNK_IEND,
        CHUNK_SRGB,
        CHUNK_BKGD,
        CHUNK_PHYS,
        CHUNK_TIME,
        CHUNK_TEXT,
        CHUNK_UNKNOWN
};

struct chunk;
struct png_image;

/* virtual functions for a chunk */
struct chunk_ops {
        /*
         * read a chunk from the memory pointed to by buf, write to out.
         * return nr bytes read on sucess, or 0 for error. If this method is
         * null, the chunk data field is expected to be empty.
         */
        ssize_t (*read)(struct chunk *chunk, const uint8_t *buf, size_t size);

        /* print info about the chunk. If null, nothing will be printed */
        void (*print_info)(FILE *stream, const struct chunk *chunk);

        /* free the chunk. If null, free is called on the chunk itself */
        void (*free)(struct chunk *chunk);
        
        /* alocate a chunk. if null, a generic chunk is allocated */
        struct chunk *(*alloc)();
};

struct chunk *chunk_lookup(struct png_image *img, enum chunk_enum type);

/* generic data for every chunk in a png image */
struct chunk_template {
        /*
         * chunk type. lsb is last byte of type, msb is first byte of type
         * (simulates interpreting the on-disk format of chunk type as if it
         * were a png (big endian) integer)
         */
        int32_t ct_type;

        /* human readable name of the chunk */
        const char *ct_name;

        /* enumerated type of the chunk. useful for indexing */
        enum chunk_enum ct_type_idx;

        /* virtual functions to operate on this chunk */
        struct chunk_ops ct_ops;
};

struct chunk {
        struct chunk_template *c_tmpl;
        struct png_image *c_img;
        size_t length;

        /* XXX: replace this with a real list */
        struct chunk *next;
};

struct png_image {
        /* XXX: replace this with a real list */
        struct chunk *first;
};

/* read a chunk from a buffer and return a chunk of the correct type */
ssize_t parse_next_chunk(const uint8_t *buf, size_t size, struct png_image *img);

#endif /* PNG_CHUNK_H */
