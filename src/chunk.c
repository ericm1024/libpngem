#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
extern struct chunk_template dimension_chunk_tmpl;
extern struct chunk_template time_chunk_tmpl;
extern struct chunk_template text_chunk_tmpl;
extern struct chunk_template unknown_chunk_tmpl;

static struct chunk_template* c_tmpl_mapping[] = {
        [CHUNK_IHDR] = &header_chunk_tmpl,
        [CHUNK_PLTE] = &palette_chunk_tmpl,
        [CHUNK_IDAT] = &data_chunk_tmpl,
        [CHUNK_IEND] = &end_chunk_tmpl,
        [CHUNK_SRGB] = &srgb_chunk_tmpl,
        [CHUNK_BKGD] = &background_chunk_tmpl,
        [CHUNK_PHYS] = &dimension_chunk_tmpl,
        [CHUNK_TIME] = &time_chunk_tmpl,
        [CHUNK_TEXT] = &text_chunk_tmpl,
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

        if ((unsigned long long)ret != chunk->length)
                return -P_EINVAL; /* XXX: return a better error value */

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


/* handle unknown chunks somewhat nicely this way */

struct chunk_template unknown_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(0, 0, 0, 0),
        .ct_name = "unknown",
        .ct_type_idx = CHUNK_UNKNOWN
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


/* definitions for pixel dimensions chunk 11.3.5.3 */

struct dimension_chunk {
        /* base chunk */
        struct chunk chunk;

        /* pixels per unit, X */
        uint32_t ppu_x;

        /* pixels per unit, Y */
        uint32_t ppu_y;

        /* unit of meausure */
        uint8_t unit;
};

#define DIMEN_UNIT_UNKNOWN 0
#define DIMEN_UNIT_METER 1

#define DIMEN_DISK_SIZE 9

static inline struct dimension_chunk *dimension_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct dimension_chunk, chunk);
}

static ssize_t dimension_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct dimension_chunk *dc;
        uint32_t ppu_x, ppu_y;
        uint8_t unit;

        dc = dimension_chunk(chunk);

        if (size < DIMEN_DISK_SIZE)
                return -P_E2SMALL;

        /* first field is pixels per unit x */
        if (!read_png_uint(buf, &ppu_x))
                return -P_ERANGE;
        buf += sizeof ppu_x;

        /* next is pixels per unit y */
        if (!read_png_uint(buf, &ppu_y))
                return -P_ERANGE;
        buf += sizeof ppu_y;

        /* final field is the unit of measure */
        unit = *buf;
        if (unit != DIMEN_UNIT_UNKNOWN && unit != DIMEN_UNIT_METER)
                return -P_EINVAL;

        dc->ppu_x = ppu_x;
        dc->ppu_y = ppu_y;
        dc->unit = unit;
        return DIMEN_DISK_SIZE;
}

static void dimension_print_info(FILE *stream, const struct chunk *chunk)
{
        struct dimension_chunk *dc;
        const char *unit;

        dc = dimension_chunk(chunk);

        unit = dc->unit == DIMEN_UNIT_UNKNOWN ? "unit" : "meter";

        fprintf(stream, "pixels per %s x: %u\n", unit, dc->ppu_x);
        fprintf(stream, "pixels per %s y: %u\n", unit, dc->ppu_x);
}

static void dimension_free(struct chunk *chunk)
{
        free(dimension_chunk(chunk));
}

static struct chunk *dimension_alloc(struct png_image *img, size_t length)
{
        struct dimension_chunk *dc;
        (void)img;
        (void)length;

        dc = malloc(sizeof *dc);
        if (!dc)
                return NULL;

        return &dc->chunk;
}

struct chunk_template dimension_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(112, 72, 89, 115),
        .ct_name = "physical dimensions",
        .ct_type_idx = CHUNK_PHYS,
        .ct_ops = {
                .read = dimension_read,
                .print_info = dimension_print_info,
                .free = dimension_free,
                .alloc = dimension_alloc
        }
};


/* definitions for timestamp chunk 11.3.6.1 */

struct time_chunk {
        /* base chunk */
        struct chunk chunk;

        /* full year, i.e. 1995. (what happens in year 65536??) */
        uint16_t year;

        /* month, 1-12 */
        uint8_t month;

        /* day of month, 1-31 */
        uint8_t day;

        /* hour, 1-23 */
        uint8_t hour;

        /* minute, 0-59 */
        uint8_t minute;

        /* second, 0-60 (for leap seconds) */
        uint8_t second;
};

#define TIME_DISK_SIZE 7

enum month {
        JANUARY = 1,
        FEBRUARY,
        MARCH,
        APRIL,
        MAY,
        JUNE,
        JULY,
        AUGUST,
        SEPTEMBER,
        OCTOBER,
        NOVEMBER,
        DECEMBER
};

/* months of the year are capitalized in English */
static const char *month_names[] = {
        [JANUARY]   = "January",
        [FEBRUARY]  = "February",
        [MARCH]     = "March",
        [APRIL]     = "April",
        [MAY]       = "May",
        [JUNE]      = "June",
        [JULY]      = "July",
        [AUGUST]    = "August",
        [SEPTEMBER] = "September",
        [OCTOBER]   = "October",
        [NOVEMBER]  = "November",
        [DECEMBER]  = "December"
};

static inline struct time_chunk *time_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct time_chunk, chunk);
}

static ssize_t time_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct time_chunk *tc;
        uint16_t year;
        uint8_t month, day, hour, minute, second;

        tc = time_chunk(chunk);

        if (size < TIME_DISK_SIZE)
                return -P_E2SMALL;

        /* first field is year. any values are valid (lol, sort of) */
        year = read_png_uint16(buf);
        buf += sizeof year;

        /* next is month, 1 indexed */
        month = *buf++;
        if (month < 1 || month > 12)
                return -P_EINVAL;

        /* day, 1 indexed */
        day = *buf++;
        if (day < 1 || day > 31)
                return -P_EINVAL;

        /* hour, 0 indexed */
        hour = *buf++;
        if (hour > 23)
                return -P_EINVAL;

        /* minute, 0 indexed */
        minute = *buf++;
        if (minute > 59)
                return -P_EINVAL;

        /* seconds, 0 indexed, and leap seconds are okay */
        second = *buf++;
        if (second > 60)
                return -P_EINVAL;

        /* validate day against month */
        switch (month) {
        case APRIL:
        case JUNE:
        case SEPTEMBER:
        case NOVEMBER:
                if (day > 30)
                        return -P_EINVAL;
                break;

        case FEBRUARY:
                /* fuck you february */
                if (year%4 == 0 && !(year%100 == 0 && year%400 != 0)) {
                        if (day > 29)
                                return -P_EINVAL;
                } else {
                        if (day > 28)
                                return -P_EINVAL;
                }
                break;

        default:
                /* we already validated the general case */
                break;
        }

        tc->year = year;
        tc->month = month;
        tc->day = day;
        tc->hour = hour;
        tc->second = second;

        return TIME_DISK_SIZE;
}

static void time_print_info(FILE *stream, const struct chunk *chunk)
{
        struct time_chunk *tc;

        tc = time_chunk(chunk);

        fprintf(stream, "image timestamp is %s %d, %d %d:%d:%d\n",
                month_names[tc->month], tc->day, tc->year, tc->hour,
                tc->minute, tc->second);
}

static void time_free(struct chunk *chunk)
{
        free(time_chunk(chunk));
}

static struct chunk *time_alloc(struct png_image *img, size_t length)
{
        struct time_chunk *tc;
        (void)img;
        (void)length;

        tc = malloc(sizeof *tc);
        if (!tc)
                return NULL;

        return &tc->chunk;
}

struct chunk_template time_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(116, 73, 77, 69),
        .ct_name = "timestamp",
        .ct_type_idx = CHUNK_TIME,
        .ct_ops = {
                .read = time_read,
                .print_info = time_print_info,
                .free = time_free,
                .alloc = time_alloc
        }
};


/* definitions for text chunk 11.3.4.3 */

#define TEXT_KEYWORD_MAXLEN 80

struct text_chunk {
        /* base chunk */
        struct chunk chunk;

        /* null-terminated keyword string of length <= TEXT_KEYWORD_LEN */
        char *keyword;
        size_t key_len;

        /*
         * heap allocated text string of unbounded length. NOT null terminated
         * (because the on disk representation isn't)
         */
        char *text;
        size_t text_len;
};

static inline struct text_chunk *text_chunk(const struct chunk *chunk)
{
        return container_of(chunk, struct text_chunk, chunk);
}

static ssize_t text_read(struct chunk *chunk, const char *buf, size_t size)
{
        struct text_chunk *tc;
        size_t chunk_len, key_len, text_len;
        char *keyword, *text;
        (void)size;

        tc = text_chunk(chunk);
        chunk_len = tc->chunk.length;

        /*
         * look for a key with length <= TEXT_KEYWORD_MAXLEN with a null byte
         * at the end. make sure we don't go past the end of the chunk.
         */
        for (key_len = 0; *(buf + key_len); key_len++)
                if (key_len > TEXT_KEYWORD_MAXLEN || key_len >= chunk_len)
                        return -P_EINVAL;

        /* count the null byte */
        key_len++;

        /* keyword is required to be 1 byte, plus the null */
        if (key_len < 2)
                return -P_E2SMALL;

        /* allocate and copy the keyword */
        keyword = malloc(sizeof *keyword * key_len);
        if (!keyword)
                return -P_ENOMEM;
        memcpy(keyword, buf, key_len);

        /*
         * allocate and copy the text body (defined to be the rest of the
         * chunk), if one is present.
         */
        text = NULL;
        text_len = chunk_len - key_len;
        if (text_len) {
                text = malloc(sizeof *text * text_len);
                if (!text)
                        return -P_ENOMEM;

                memcpy(text, buf + key_len, text_len);
        }

        tc->keyword = keyword;
        tc->key_len = key_len;
        tc->text = text;
        tc->text_len = text_len;

        return chunk_len;
}

static void text_print_info(FILE *stream, const struct chunk *chunk)
{
        struct text_chunk *tc;

        tc = text_chunk(chunk);

        fprintf(stream, "keyword (len %zu): %s\n", tc->key_len, tc->keyword);
        fprintf(stream, "text (len %zu): ", tc->text_len);
        fwrite(tc->text, sizeof *tc->text, sizeof *tc->text * tc->text_len,
               stream);
        fprintf(stream, "\n");
}

static void text_free(struct chunk *chunk)
{
        free(text_chunk(chunk));
}

static struct chunk *text_alloc(struct png_image *img, size_t len)
{
        struct text_chunk *tc;
        (void)img;
        (void)len;

        tc = malloc(sizeof *tc);
        if (!tc)
                return NULL;

        return &tc->chunk;
}

struct chunk_template text_chunk_tmpl = {
        .ct_type = BYTES_TO_TYPE(116, 69, 88, 116),
        .ct_name = "text",
        .ct_type_idx = CHUNK_TEXT,
        .ct_ops = {
                .read = text_read,
                .print_info = text_print_info,
                .free = text_free,
                .alloc = text_alloc
        }
};
