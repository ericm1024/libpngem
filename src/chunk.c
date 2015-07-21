#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "chunk.h"
#include "error.h"
#include "int.h"
#include "util.h"

extern struct chunk_template header_chunk_tmpl;
extern struct chunk_template palette_chunk_tmpl;
extern struct chunk_template data_chunk_tmpl;
extern struct chunk_template end_chunk_tmpl;
extern struct chunk_template srgb_chunk_tmpl;
extern struct chunk_template background_chunk_tmpl;
extern struct chunk_template unknown_chunk_tmpl;

static struct chunk_template* c_tmpl_mapping[] = {
        [CHUNK_IHDR] = &header_chunk_tmpl,
        [CHUNK_PLTE] = &palette_chunk_tmpl,
        [CHUNK_IDAT] = &data_chunk_tmpl,
        [CHUNK_IEND] = &end_chunk_tmpl,
        [CHUNK_SRGB] = &srgb_chunk_tmpl,
        [CHUNK_BKGD] = &background_chunk_tmpl,
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

struct chunk *lookup_chunk(struct png_image *img, enum chunk_enum type)
{
        struct chunk *chunk;

        for (chunk = img->first; chunk; chunk = chunk->next)
                if (chunk->c_tmpl->ct_type_idx == type)
                        break;

        return chunk;
}

/* allocate and initialize a chunk given its type, length, and parent image */
static struct chunk *alloc_chunk(int32_t type, size_t length,
                                 struct png_image *img)
{
        enum chunk_enum type_idx;
        struct chunk *chunk, *tmp;
        struct chunk_template *tmpl;

        type_idx = type_to_idx(type);
        tmpl = c_tmpl_mapping[type_idx];
        if (tmpl->ct_ops.alloc)
                chunk = tmpl->ct_ops.alloc(img, length);
        else
                chunk = malloc(sizeof *chunk);

        if (!chunk)
                return NULL;

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
ssize_t parse_next_chunk(const char *buf, size_t size, struct png_image *img)
{
        uint32_t length, crc;
        int32_t type;
        size_t count;
        ssize_t ret;
        struct chunk *chunk;

        if (size < MIN_CHUNK_SIZE)
                return -P_E2SMALL;

        /* first comes the length field */
        count = 0;
        if (!read_png_uint(buf, &length))
                return -P_ERANGE;
        count += 4;

        /*
         * chunk length only counts the size of the data field, not the 4
         * bytes type or crc fields on either end. bail if there aren't
         * enough bytes left for this to make sense.
         */
        if (size - count < length + 8)
                return -P_E2SMALL;

        /* next comes the type field */
        type = __read_png_int_raw(buf + count);
        /* XXX: validate type here */
        count += 4;

        /* now we have enough information to read the chunk data */
        chunk = alloc_chunk(type, length, img);
        if (!chunk)
                return -P_ENOMEM;

        ret = length;
        if (chunk->c_tmpl->ct_ops.read) {
                ret = chunk->c_tmpl->ct_ops.read(chunk, buf + count,
                                                 size - count);
                if (ret < 0)
                        return ret;
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

static ssize_t header_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct header_chunk *hc;
        uint32_t width, height;
        char depth, color, ztype, filter, interlace;

        if (size < HEADER_DISK_SIZE)
                return -P_E2SMALL;
        if (chunk->length != HEADER_DISK_SIZE)
                return -P_EINVAL;

        hc = header_chunk(chunk);

        if (!read_png_uint(buf, &width))
                return -P_ERANGE;
        buf += 4;

        if (!read_png_uint(buf, &height))
                return -P_ERANGE;
        buf += 4;

        depth = *buf++;
        /* power of 2 \in [1,16] */
        if (depth < 0 || depth > 16 || __builtin_popcount(depth) != 1)
                return -P_EINVAL;

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
                        return -P_EINVAL;
                break;
                
        case COLOR_TRUE:
        case COLOR_GREY_ALPHA:
        case COLOR_TRUE_ALPHA:
                if (depth < 8 || depth > 16)
                        return -P_EINVAL;
                break;

        default:
                return -P_EINVAL;
        }

        ztype = *buf++;
        if (ztype != ZTYPE_DEFLATE)
                return -P_EINVAL;

        filter = *buf++;
        if (filter != FILTER_ADAPTIVE)
                return -P_EINVAL;

        interlace = *buf++;
        if (interlace != INTERLACE_NONE && interlace != INTERLACE_ADAM7)
                return -P_EINVAL;

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

static struct chunk *header_alloc(struct png_image *img, size_t length)
{
        struct header_chunk *hc;
        (void)img;
        (void)length;

        hc = malloc(sizeof *hc);
        if (!hc)
                return NULL;

        return &hc->chunk;
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

        /* palette itself. we porentailly waste some memory here */
        struct palette_entry palette[MAX_PALETTE_ENTRIES];
};                 

static inline struct palette_chunk *palette_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct palette_chunk, chunk);
}

static ssize_t palette_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct palette_chunk *pc;
        uint32_t length;
        unsigned i;

        pc = palette_chunk(chunk);

        length = pc->chunk.length;
        if (length % PALETE_ENTRY_SIZE)
                return -P_EINVAL;

        if (length > MAX_PALETTE_ENTRIES * PALETE_ENTRY_SIZE)
                return -P_EINVAL;

        /* XXX: validate length against bit depth as per 11.2.3 para 5 */

        pc->entries = length/PALETE_ENTRY_SIZE;

        if (size > pc->entries * PALETE_ENTRY_SIZE)
                return -P_E2SMALL;

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

static struct chunk *palette_alloc(struct png_image *img, size_t length)
{
        struct palette_chunk *pc;
        int entries;
        (void)img;

        printf("allocating palette\n");

        entries = length/PALETE_ENTRY_SIZE;
        pc = malloc(sizeof *pc);
        if (!pc)
                return NULL;

        printf("success\n");
        return &pc->chunk;
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

static ssize_t data_read(struct chunk *chunk, const char *buf, size_t size)
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
        fprintf(stream, "data chunk: %zu bytes long with base %p\n",
                dc->chunk.length, (void*)dc->buf);
}

static void data_free(struct chunk *chunk)
{
        free(data_chunk(chunk));
}

static struct chunk *data_alloc(struct png_image *img, size_t length)
{
        struct data_chunk *dc;
        (void)img;
        (void)length;

        dc = malloc(sizeof *dc);
        if (!dc)
                return NULL;

        return &dc->chunk;
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


/* definitions for srgb chunk 11.3.3.5 */

/* constants for rendering_intent field of struct srgb_chunk */
#define SRGB_RI_PERCEPTUAL 0
#define SRGB_RI_REL_COLORIMETRIC 1
#define SRGB_RI_SATURATION 2
#define SRGB_RI_ABS_COLORIMETRIC 3

struct srgb_chunk  {
        /* base chunk */
        struct chunk chunk;

        /* rndering intent */
        char rendering_intent;
};

static inline struct srgb_chunk *srgb_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct srgb_chunk, chunk);
}

static ssize_t srgb_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct srgb_chunk *sc;
        char ri;

        sc = srgb_chunk(chunk);

        if (size < 1)
                return -P_E2SMALL;

        ri = buf[0];
        if (ri != SRGB_RI_PERCEPTUAL
            && ri != SRGB_RI_REL_COLORIMETRIC
            && ri != SRGB_RI_SATURATION
            && ri != SRGB_RI_ABS_COLORIMETRIC)
                return -P_EINVAL;

        sc->rendering_intent = ri;
        return 1;
}

static void srgb_print_info(FILE *stream, const struct chunk *chunk)
{
        struct srgb_chunk *sc;

        sc = srgb_chunk(chunk);

        fprintf(stream, "srgb rendering intent is %d\n", sc->rendering_intent);
}

static void srgb_free(struct chunk *chunk)
{
        free(srgb_chunk(chunk));
}

static struct chunk *srgb_alloc(struct png_image *img, size_t length)
{
        struct srgb_chunk *sc;
        (void)img;
        (void)length;

        sc = malloc(sizeof *sc);
        if (!sc)
                return NULL;

        return &sc->chunk;
}

struct chunk_template srgb_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(115, 82, 71, 66),
        .ct_name = "srgb color space",
        .ct_type_idx = CHUNK_SRGB,
        .ct_ops = {
                .read = srgb_read,
                .print_info = srgb_print_info,
                .free = srgb_free,
                .alloc = srgb_alloc
        }
};


/* definitions for background chunk 11.3.5.1 */

struct background_chunk {
        /* base chunk */
        struct chunk chunk;
        union {
                uint16_t grey;
                struct {
                        uint16_t red;
                        uint16_t green;
                        uint16_t blue;
                };
                uint8_t palette_idx;
        };
};

static inline struct background_chunk *
background_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct background_chunk, chunk);
}

static ssize_t background_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct background_chunk *bc;
        struct header_chunk *hc;
        struct palette_chunk *pc;
        struct chunk *tmp;
        struct png_image *img;
        size_t count = 0;
        char depth;
        uint16_t color_max, grey, red, green, blue;
        uint8_t palette_idx;

        bc = background_chunk(chunk);
        img = chunk->c_img;

        /* chunk ordering rules (section 5.6) guarentee us a header */
        tmp = lookup_chunk(img, CHUNK_IHDR);
        if (!tmp)
                return -P_ENOCHUNK;

        hc = header_chunk(tmp);
        depth = hc->depth;
        color_max = (1 << depth) - 1;

        /*
         * depending on the color type of the image, the background chunk has
         * a different structure.
         *
         *     - if the image is greyscale, it has a single 2-byte field for
         *       the background color
         *     - if the image is colored, then it has 3 2-bytes fields for
         *       red green and blue
         *     - otherwise (palette color), it has a 1-bytes field that is
         *       a palette index
         *
         * we also have to carefully bounds check these values based on the
         * bit depth of the image or the number of entries in the palette.
         */
        switch (hc->color) {
        case COLOR_GREYSCALE:
        case COLOR_GREY_ALPHA:
                count = 2;
                if (size < count)
                        return -P_E2SMALL;

                grey = read_png_uint16(buf);
                if (grey > color_max)
                        return -P_EINVAL;

                bc->grey = grey;
                break;

        case COLOR_TRUE:
        case COLOR_TRUE_ALPHA:
                count = 6;
                if (size < count)
                        return -P_E2SMALL;

                red = read_png_uint16(buf);
                green = read_png_uint16(buf + 2);
                blue = read_png_uint16(buf + 4);
                if (red > color_max || green > color_max || blue > color_max)
                        return -P_EINVAL;

                bc->red = red;
                bc->green = green;
                bc->blue = blue;
                break;

        case COLOR_INDEXED:
                count = 1;
                if (size < count)
                        return -P_E2SMALL;

                /* ordering rules guarentee us a palette chunk by now */
                tmp = lookup_chunk(img, CHUNK_PLTE);
                if (!tmp)
                        return -P_ENOCHUNK;
                pc = palette_chunk(tmp);

                palette_idx = *buf;
                if (palette_idx >= pc->entries)
                        return -P_EINVAL;

                bc->palette_idx = palette_idx;
                break;

        default:
                BUG()
        }

        return count;
}

static void background_print_info(FILE *stream, const struct chunk *chunk)
{
        struct png_image *img;
        struct background_chunk *bc;
        struct header_chunk *hc;
        struct palette_chunk *pc;
        struct chunk *tmp;
        struct palette_entry *pentry;

        bc = background_chunk(chunk);
        img = chunk->c_img;

        tmp = lookup_chunk(img, CHUNK_IHDR);
        if (!tmp)
                BUG();

        hc = header_chunk(tmp);
        switch (hc->color) {
        case COLOR_GREYSCALE:
        case COLOR_GREY_ALPHA:
                fprintf(stream, "background color (grey): %d\n", bc->grey);
                break;

        case COLOR_TRUE:
        case COLOR_TRUE_ALPHA:
                fprintf(stream, "background color (rgb): %d %d %d\n",
                        bc->red, bc->blue, bc->green);
                break;

        case COLOR_INDEXED:
                tmp = lookup_chunk(img, CHUNK_PLTE);
                if (!tmp)
                        BUG();

                pc = palette_chunk(tmp);
                pentry = &pc->palette[bc->palette_idx];
                fprintf(stream, "background color (palette, rgb): %d %d %d\n",
                        pentry->red, pentry->green, pentry->blue);
        }
}

static void background_free(struct chunk *chunk)
{
        free(background_chunk(chunk));
}

static struct chunk *background_alloc(struct png_image *img, size_t length)
{
        struct background_chunk *bc;
        (void)img;
        (void)length;

        bc = malloc(sizeof *bc);
        if (!bc)
                return NULL;

        return &bc->chunk;
}

struct chunk_template background_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(98, 75, 71, 68),
        .ct_name = "background color",
        .ct_type_idx = CHUNK_BKGD,
        .ct_ops = {
                .read = background_read,
                .print_info = background_print_info,
                .free = background_free,
                .alloc = background_alloc
        }
};


/* handle unknown chunks somewhat nicely this way */

struct chunk_template unknown_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(0, 0, 0, 0),
        .ct_name = "unknown",
        .ct_type_idx = CHUNK_UNKNOWN
};
