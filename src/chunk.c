#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "chunk.h"
#include "int.h"
#include "util.h"

extern struct chunk_template header_chunk_tmpl;
extern struct chunk_template palette_chunk_tmpl;
extern struct chunk_template data_chunk_tmpl;
extern struct chunk_template end_chunk_tmpl;
extern struct chunk_template unknown_chunk_tmpl;

static struct chunk_template* c_tmpl_mapping[] = {
        [CHUNK_IHDR] = &header_chunk_tmpl,
        [CHUNK_PLTE] = &palette_chunk_tmpl,
        [CHUNK_IDAT] = &data_chunk_tmpl,
        [CHUNK_IEND] = &end_chunk_tmpl,
        [CHUNK_UNKNOWN] = &unknown_chunk_tmpl
};

static inline enum chunk_enum type_to_idx(int32_t type)
{
        enum chunk_enum type_idx;
        
        for (type_idx = 0; type_idx < CHUNK_UNKNOWN; type_idx++)
                if (c_tmpl_mapping[type_idx]->ct_type == type)
                        break;
        
        return type_idx;
}

#define BYTES_TO_TYPE(b0, b1, b2, b3)           \
        ((b0) << 24 | (b1) << 16 | (b2) << 8 | b3)

/* allocate and initialize a chunk given its type, length, and parent image */
static struct chunk *alloc_chunk(int32_t type, size_t length,
                                 struct png_image *img)
{
        enum chunk_enum type_idx;
        struct chunk *chunk, *tmp;
        struct chunk_template *tmpl;

        type_idx = type_to_idx(type);
        tmpl = c_tmpl_mapping[type_idx];
        if (tmpl->ct_ops.alloc) {
                if (!tmpl->ct_ops.alloc(img, length, &chunk))
                        return NULL;
        } else {
                chunk = malloc(sizeof *chunk);
                if (!chunk)
                        return NULL;
        }
        
        chunk->c_tmpl = tmpl;
        chunk->c_img = img;
        chunk->length = length;
        chunk->next = NULL;

        /* put the chunk at the end of the image's singly linked list of chunks */
        if (!img->first) {
                img->first = chunk;
        } else {
                tmp = img->first;
                while (tmp->next)
                        tmp = tmp->next;
                tmp->next = chunk;
        }

        return chunk;
}

/* placeholder to crc */
static uint32_t do_crc(const char *buf, size_t size)
{
        (void)buf;
        (void)size;

        return 0;
}

/* read the next chunk out of a buffer. return nr of bytes read */
size_t parse_next_chunk(const char *buf, size_t size, struct png_image *img)
{
        uint32_t length, crc;
        int32_t type;
        size_t count, ret;
        struct chunk *chunk;

        printf("entering parse chunk. buf is %p\n", buf);
        
        if (size < MIN_CHUNK_SIZE) {
                printf("didn't have enough bytes left: %zu", size);
                return 0;
        }

        /* first comes the length field */
        count = 0;
        if (!read_png_uint(buf, &length)) {
                printf("failed to parse length: %"PRIu32"\n", length);
                return 0;
        }
        count += 4;

        printf("read length %" PRIu32 "\n", length);

        /*
         * chunk length only counts the size of the data field, not the 4
         * bytes type or crc fields on either end. bail if there aren't
         * enough bytes left for this to make sense.
         */
        if (size - count < length + 8)
                return 0;

        /* next comes the type field */
        type = __read_png_int_raw(buf + count);
        /* XXX: validate type here */
        count += 4;

        printf("read type %"PRIu32" aka %d %d %d %d\n", type,
               (type >> 24) & 0xff, (type >> 16) & 0xff,
               (type >> 8) & 0xff, type & 0xff);

        /* now we have enough information to read the chunk data */
        chunk = alloc_chunk(type, length, img);
        if (!chunk)
                return 0;

        ret = length;
        if (chunk->c_tmpl->ct_ops.read) {
                ret = chunk->c_tmpl->ct_ops.read(chunk, buf + count,
                                                 size - count);
                if (!ret) {
                        printf("read failed\n");
                        return 0;
                }
        } else {
                printf("skipped read for %s chunk with type %d %d %d %d\n",
                       chunk->c_tmpl->ct_name,
                       (type >> 24) & 0xff,
                       (type >> 16) & 0xff,
                       (type >> 8) & 0xff,
                       type & 0xff);
        }
        count += ret;

        /*
         * finally read and validate the crc. the crc is for the type and
         * data fields of the chunk, but not the length field, so start 4
         * bytes in and add 4 to length to include the size of the type
         * field.
         */
        crc = __read_png_int_raw(buf + count);
        if (crc != do_crc(buf + 4, length + 4))
                crc = 0; /* placeholder b/c no crc: return 0; */
        count += 4;

        return count;
}


/* definitions for header chunk. see section 11.2.2 */

/* bit values for various header fields */
#define __COLOR_GREYSCALE  0    
#define __COLOR_INDEXED    1
#define __COLOR_TRUE       2
#define __COLOR_ALPHA      4
#define COLOR_GREYSCALE    __COLOR_GREYSCALE
#define COLOR_TRUE         __COLOR_TRUE
#define COLOR_INDEXED      (__COLOR_INDEXED | __COLOR_TRUE)
#define COLOR_GREY_ALPHA   (__COLOR_GREYSCALE | __COLOR_ALPHA)
#define COLOR_TRUE_ALPHA   (__COLOR_TRUE | __COLOR_ALPHA)

#define ZTYPE_DEFLATE      0
#define FILTER_ADAPTIVE    0
#define INTERLACE_NONE     0
#define INTERLACE_ADAM7    1

#define HEADER_DISK_SIZE   13

/* each image has exactly one header chunk. basic metadata about the image */
struct header_chunk {
        /* base chunk */
        struct chunk chunk;

        /* width of the image in pixels */
        uint32_t width;

        /* height of the image in pixels */
        uint32_t height;

        /* pixel depth i.e. bits per pixel */
        char depth;

        /* type of color in the image. one of COLOR_* */
        char color;

        /* compression type. must be ZTYPE_DEFLATE */
        char ztype;

        /* filtering type. must be FILTER_ADAPTIVE */
        char filter;

        /* interlace type. one of INTERLACE_* */
        char interlace;
};

static inline struct header_chunk *header_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct header_chunk, chunk);
}

static size_t header_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct header_chunk *hc;
        uint32_t width, height;
        char depth, color, ztype, filter, interlace;

        if (size < HEADER_DISK_SIZE)
                return 0;

        hc = header_chunk(chunk);

        if (!read_png_uint(buf, &width))
                return 0;
        buf += 4;

        if (!read_png_uint(buf, &height))
                return 0;
        buf += 4;

        depth = *buf++;
        /* power of 2 \in [1,16] */
        if (depth < 0 || depth > 16 || __builtin_popcount(depth) != 1)
                return 0;

        /*
         * validate that that the color is a valid value, and at the same
         * time validate that the bit depth is valid for that color.
         * see the restrictions in Table 11.1 of the standard
         */
        color = *buf++;
        switch (color) {
        case COLOR_GREYSCALE:
                /* all depths valid */
                break;

        case COLOR_INDEXED:
                if (depth > 8)
                        return 0;
                break;
                
        case COLOR_TRUE:
        case COLOR_GREY_ALPHA:
        case COLOR_TRUE_ALPHA:
                if (depth < 8 || depth > 16)
                        return 0;
                break;

        default:
                return 0;
        }

        ztype = *buf++;
        if (ztype != ZTYPE_DEFLATE)
                return 0;

        filter = *buf++;
        if (filter != FILTER_ADAPTIVE)
                return 0;

        interlace = *buf++;
        if (interlace != INTERLACE_NONE && interlace != INTERLACE_ADAM7)
                return 0;

        hc->width = width;
        hc->height = height;
        hc->depth = depth;
        hc->color = color;
        hc->ztype = ztype;
        hc->filter = filter;
        hc->interlace = interlace;
        
        return HEADER_DISK_SIZE;
}

static void header_print_info(FILE *stream, const struct chunk *chunk)
{
        struct header_chunk *hc;

        hc = header_chunk(chunk);

        fprintf(stream, "width: %" PRIu32 "\n", hc->width);
        fprintf(stream, "height: %" PRIu32 "\n", hc->height);
        fprintf(stream, "depth: %d\n", hc->depth);
        fprintf(stream, "color: %d\n", hc->color);
        fprintf(stream, "ztype: %d\n", hc->ztype);
        fprintf(stream, "filter: %d\n", hc->filter);
        fprintf(stream, "interlace: %d\n", hc->interlace);
}

static void header_free(struct chunk *chunk)
{
        free(header_chunk(chunk));
}

static bool header_alloc(struct png_image *img, size_t length,
                        struct chunk **out)
{
        struct header_chunk *hc;
        (void)img;

        if (length != HEADER_DISK_SIZE)
                return false;

        hc = malloc(sizeof *hc);
        if (!hc)
                return false;

        *out = &hc->chunk;
        return true;
}

struct chunk_template header_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(73, 72, 68, 82),
        .ct_name = "header",
        .ct_type_idx = CHUNK_IHDR,
        .ct_ops = {
                .read = header_read,
                .print_info = header_print_info,
                .free = header_free,
                .alloc = header_alloc
        }
};


/* definitions for palette chunk. see section 11.2.3 */

#define MAX_PALETTE_ENTRIES  256U
#define PALETE_ENTRY_SIZE    3U

/* a single entry in a static color palette */
struct palette_entry {
        char red;
        char green;
        char blue;
};

/* each image has exactly one palette chunk */
struct palette_chunk {
        /* base chunk */
        struct chunk chunk;

        /* number of entries in the palette */
        unsigned entries;

        /* palette itself */
        struct palette_entry palette[];
};                 

static inline struct palette_chunk *palette_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct palette_chunk, chunk);
}

static size_t palette_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct palette_chunk *pc;
        unsigned i;

        pc = palette_chunk(chunk);

        if (size > pc->entries * PALETE_ENTRY_SIZE) {
                printf("palette read failed\n");
                return 0;
        }

        for (i = 0; i < pc->entries; i++) {
                pc->palette[i].red = *buf++;
                pc->palette[i].green = *buf++;
                pc->palette[i].blue = *buf++;
        }
        
        return pc->entries * PALETE_ENTRY_SIZE;
}

static void palette_print_info(FILE *stream, const struct chunk *chunk)
{
        struct palette_chunk *pc;
        struct palette_entry *entry;
        unsigned i;

        pc = palette_chunk(chunk);

        fprintf(stream, "palette has %d entries\n", pc->entries);
        for (i = 0; i < pc->entries; i++) {
                entry = &pc->palette[i];
                fprintf(stream, "palette entry %d: (r: %d, g: %d, b: %d)\n",
                        i, entry->red, entry->green, entry->blue);
        }
}

static void palette_free(struct chunk *chunk)
{
        free(palette_chunk(chunk));
}

static bool palette_alloc(struct png_image *img, size_t length,
                         struct chunk **out)
{
        struct palette_chunk *pc;
        int entries;
        (void)img;

        printf("allocating palette\n");

        if (length % PALETE_ENTRY_SIZE)
                return false;

        if (length > MAX_PALETTE_ENTRIES * PALETE_ENTRY_SIZE)
                return false;

        /* XXX: validate length against bit depth as per 11.2.3 para 5 */

        /*
         * we have to do a bit of a dance here because the size of a
         * struct palette_entry may not be the size of an on disk palette
         * entry due to padding
         */
        entries = length/PALETE_ENTRY_SIZE;
        pc = malloc(sizeof *pc + entries * sizeof pc->palette[0]);
        if (!pc)
                return false;

        pc->entries = length/PALETE_ENTRY_SIZE;
        *out = &pc->chunk;
        printf("success\n");        
        return true;
}

struct chunk_template palette_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(80, 76, 84, 69),
        .ct_name = "palette",
        .ct_type_idx = CHUNK_PLTE,
        .ct_ops = {
                .read = palette_read,
                .print_info = palette_print_info,
                .free = palette_free,
                .alloc = palette_alloc
        }
};


/* definitions for data chunk. section 11.2.4 */

struct data_chunk {
        /* base chunk */
        struct chunk chunk;

        /*
         * pointer to chunk data. read only (must me later concatenated
         * to be beaningful)
         */
        const char *buf;
};

static inline struct data_chunk *data_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct data_chunk, chunk);
}

static size_t data_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct data_chunk *dc;
        (void)size;
        
        dc = data_chunk(chunk);
        dc->buf = buf;
        
        return dc->chunk.length;
}

static void data_print_info(FILE *stream, const struct chunk *chunk)
{
        struct data_chunk *dc;

        dc = data_chunk(chunk);
        fprintf(stream, "data chunk: %zu bytes long with base %p",
                dc->chunk.length, (void*)dc->buf);
}

static void data_free(struct chunk *chunk)
{
        free(data_chunk(chunk));
}

static bool data_alloc(struct png_image *img, size_t length,
                      struct chunk **out)
{
        struct data_chunk *dc;
        (void)img;
        (void)length;

        dc = malloc(sizeof *dc);
        if (!dc)
                return false;

        *out = &dc->chunk;
        return true;
}

struct chunk_template data_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(73, 68, 65, 84),
        .ct_name = "data",
        .ct_type_idx = CHUNK_IDAT,
        .ct_ops = {
                .read = data_read,
                .print_info = data_print_info,
                .free = data_free,
                .alloc = data_alloc
        }
};


/* definitions for end chunk 11.2.5 */

struct chunk_template end_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(73, 69, 78, 68),
        .ct_name = "end",
        .ct_type_idx = CHUNK_IEND
        /* ops are all null, generic stuff is fine */
};


/* handle unknown chunks somewhat nicely this way */

struct chunk_template unknown_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(0, 0, 0, 0),
        .ct_name = "unknown",
        .ct_type_idx = CHUNK_UNKNOWN
};
