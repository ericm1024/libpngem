#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "int.h"
#include "util.h"
#include "zlib.h"

/* constants for parsing the header */
#define ZLIB_CM_DEFLATE 8
#define ZLIB_WSIZE_MAX (1UL << 15)
#define ZLIB_WSIZE_BIAS 8

/* constants for parsing block header */
#define BLK_BFINAL_BTS 1
#define BLK_BTYPE_BTS 2
#define BLK_BTYPE_NONE 0
#define BLK_BTYPE_STATIC 1
#define BLK_BTYPE_DYNAMIC 2
#define BLK_BTYPE_RESERVED 3

static const uint8_t *stream_src(struct zlib_stream *stream)
{
        return stream->z_src + stream->z_src_idx;
}

static uint8_t *stream_dst(struct zlib_stream *stream)
{
        return stream->z_dst + stream->z_dst_idx;
}

static size_t stream_sbytes(struct zlib_stream *stream)
{
        return stream->z_src_end - stream->z_src_idx;
}

static size_t stream_dbytes(struct zlib_stream *stream)
{
        return stream->z_dst_end - stream->z_dst_idx;
}

/* read the next nbits from a stream */
static uint32_t read_bits(struct zlib_stream *stream, unsigned nbits)
{
        uint32_t bits, tmp;  /* result, tmp */
        unsigned rbits; /* number of bits read */
        unsigned ebits; /* number of extra bits read */

        assert(nbits <= 8 * sizeof bits);

        /* read the first (possibly partial) byte */
        tmp = *stream_src(stream);
        bits = tmp >> stream->z_src_bidx;
        stream->z_src_idx++;
        rbits = 8 - stream->z_src_bidx;

        /* read following bytes until we get enough bits */
        while (rbits < nbits) {
                tmp = *stream_src(stream);
                bits |= tmp << rbits;
                stream->z_src_idx++;
                rbits += 8;
        }

        /*
         * find the index into next byte of the stream. If we read any extra
         * bits, then we need to decriment the index into the source buffer
         * as there are still some bits left in it
         */
        ebits = nbits - rbits;
        if (ebits)
                stream->z_src_idx--;
        stream->z_src_bidx = (stream->z_src_bidx + nbits) % 8;

        return bits & (~((uint32_t)0) >> (32 - nbits));
}

static uint8_t read_bit(struct zlib_stream *stream)
{
        uint8_t bit;
        bit = *stream_src(stream);
        bit >>= stream->z_src_bidx;
        stream->z_src_bidx++;
        if (stream->z_src_bidx == 8) {
                stream->z_src_bidx = 0;
                stream->z_src_idx++;
        }
        return bit & 1;
}

static uint8_t read_byte(struct zlib_stream *stream)
{
        return read_bits(stream, 8);
}

static int parse_header(struct zlib_stream *stream)
{
        uint8_t cmf, flg;
        size_t wsize;
        bool fdict;

        printf("parse_header: entering\n");

        if (stream_sbytes(stream) < 2)
                return -P_E2SMALL;

        cmf = read_byte(stream);
        flg = read_byte(stream);

        printf("cmf: 0x%x, flg: 0x%x\n", cmf, flg);

        if ((cmf*256 + flg) % 31)
                return -P_EBADCSUM;

        if ((cmf & 0xf) != ZLIB_CM_DEFLATE)
                return -P_EINVAL;

        wsize = ((cmf & 0xf0) >> 4) + ZLIB_WSIZE_BIAS;
        if (wsize > ZLIB_WSIZE_MAX)
                return -P_EINVAL;

        fdict = flg & 0x20;
        if (fdict) {
                printf("got fdict. I don't know what to do with this\n");
                return -P_ENOTSUP;
        }

        return 0;
}

static int realloc_stream(struct zlib_stream *stream)
{
        stream->z_dst_end *= 2;
        stream->z_dst = realloc(stream->z_dst, stream->z_dst_end);
        return stream->z_dst ? 0 : -P_ENOMEM;
}

struct huff_sym {
        uint16_t s_sym:11;
        uint16_t s_len:5;
};

#define SYM_INIT(sym, len)                                      \
        (struct huff_sym) { .s_sym = sym, .s_len = len }

/*
 * A struct huff_range represents a range of symbols whose corresponding
 * Huffman codes all have the same length and by extension are
 * lexicographically consecutive. Consecutiveness of encoded values is
 * guarenteed by the Huffman encoded rules defined in section 3.2.2 of the
 * deflate standard.
 */
struct huff_range {
        /* number of symbols in this range */
        uint16_t r_count;

        /* length in bits of the huffman encodings in this range */
        uint8_t r_len;

        /*
         * pointer into the h_syms field of the parent struct huff_tree
         * where this range starts.
         */
        struct huff_sym *r_syms;

        /* huffman encoding of the first symbol in this range */
        uint16_t r_start;

        /* huffman encoding of the last symbol in this range */
        uint16_t r_end;
};

#define HUFF_LL_SIZE 288
#define HUFF_DIST_SIZE 32
#define HUFF_NR_RANGES 16

/*
 * A struct huff_tree is a mapping from the huffman encodings of a given
 * alphabet to their actual values. It is specialized to the representation
 * used in deflate streams. The h_syms field contains every symbol in the
 * represented alphabet, ordered first by the length of their corresponding
 * Huffman codes, and within codes of the same length ordered
 * lexicographically.
 */
struct huff_tree {
        /* all sybols in the alphabet */
        struct huff_sym *h_syms;
        unsigned h_nsyms;

        /*
         * range metadata. used to answer "what is the symbol coresponding
         * to Huffamn code x with length L"
         */
        struct huff_range h_ranges[HUFF_NR_RANGES];
};

static struct huff_tree *huff_alloc(unsigned entries)
{
        struct huff_tree *t;
        struct huff_sym *s;

        t = malloc(sizeof *t);
        if (!t)
                return NULL;

        s = malloc(entries * sizeof *s);
        if (!s) {
                free(t);
                return NULL;
        }

        memset(t, 0, sizeof *t);
        t->h_nsyms = entries;
        t->h_syms = s;
        return t;
}

static void huff_free(struct huff_tree *t)
{
        if (t)
                free(t->h_syms);
        free(t);
}

/* for qsort */
static int huff_cmp(const void *_lhs, const void *_rhs)
{
        const struct huff_sym *lhs = _lhs;
        const struct huff_sym *rhs = _rhs;

        return lhs->s_len == rhs->s_len
                ? lhs->s_sym - rhs->s_sym
                : lhs->s_len - rhs->s_len;
}

static void dump_ranges(const struct huff_tree *tree)
{
        unsigned i, j;
        const struct huff_range *range;

        printf("begin range dump for huff_tree (%p)\n", (void*)tree);
        printf("tree->h_nsyms: %u\n", tree->h_nsyms);

        for (i = 0; i < HUFF_NR_RANGES; i++) {
                range = &tree->h_ranges[i];
                printf("range %d. r_count: %d, r_len: %d, r_start: 0x%x\n",
                       i, range->r_count, range->r_len, range->r_start);
                for (j = 0; j < range->r_count; j++) {
                        printf("(%d,%d) ", range->r_syms[j].s_sym,
                               range->r_syms[j].s_len);
                }
                if (range->r_count)
                        printf("\n");
        }
        printf("end range dump\n");
}

/*
 * fill out the struct huff_range metadata structures based on a fully
 * filled out h_syms
 */
static int huff_init_ranges(struct huff_tree *tree)
{
        struct huff_range *range;
        struct huff_sym sym;
        unsigned i;
        uint16_t code, offset, old_count;
        int ret = 0;

        /*
         * sort the symbols first by length then lexicographically within
         * symbols of the same length
         */
        qsort(tree->h_syms, tree->h_nsyms, sizeof *tree->h_syms, huff_cmp);

        /* count the number of elements of each bit length */
        for (i = 0; i < tree->h_nsyms; i++) {
                sym = tree->h_syms[i];
                tree->h_ranges[sym.s_len].r_count++;
        }

        /* initialize the r_syms pointers and set the length of each range */
        offset = 0;
        for (i = 0; i < HUFF_NR_RANGES; i++) {
                range = &tree->h_ranges[i];
                range->r_len = i;
                range->r_syms = tree->h_syms + offset;
                offset += range->r_count;
        }

        /*
         * wierdness: the first range has things of length zero (i.e.
         * symbols that never appear in the output stream), so we don't
         * want to confuse our Huffman encoding by counting them.
         */
        old_count = tree->h_ranges[0].r_count;
        tree->h_ranges[0].r_count = 0;

        /* compute the starting and ending code for each range */
        code = 0;
        range = &tree->h_ranges[0];
        for (i = 1; i < HUFF_NR_RANGES; i++) {
                code = (code + range->r_count) << 1;
                range = &tree->h_ranges[i];
                range->r_start = code;
                range->r_end = code + range->r_count;
        }

        /* restore the range 0 count in case someone else depends on it */
        tree->h_ranges[0].r_count = old_count;

        /*
         * todo: reoder the ranges so that empty ranges appear at the end.
         * this will make lookups faster
         */

        /* XXX: should probably be doing some validation in this function */
        for (i = 0; i < HUFF_NR_RANGES; i++) {
                range = &tree->h_ranges[i];
                if (!range->r_end)
                        continue;

                if (range->r_end - 1 & ~((1U << range->r_len) - 1)) {
                        printf("bad range: len %d, end 0x%x\n",
                               range->r_len, range->r_end);
                        ret = -P_EINVAL;
                }
        }

        //dump_ranges(tree);

        return ret;
}

static int huff_read(struct zlib_stream *stream, struct huff_tree *tree,
                     uint16_t *out)
{
        int i, error, bits = 0;
        uint16_t code = 0;
        struct huff_range *range;

        for (i = 0; i < HUFF_NR_RANGES; i++) {
                range = &tree->h_ranges[i];
                if (!range->r_count || !range->r_len)
                        continue;

                /* huffman codes come most significant bit first */
                do {
                        code <<= 1;
                        code |= read_bit(stream);
                        bits++;
                } while (range->r_len > bits);

                if (code < range->r_start) {
                        printf("got code < r_start? probably bad\n");
                        printf("code: %d, r_start %d, bits: %d\n",
                               code, range->r_start, bits);
                        error = -P_EINVAL;
                        goto out;
                } else if (code < range->r_end) {
                        *out = range->r_syms[code - range->r_start].s_sym;
                        error = 0;
                        goto out;
                }
        }

        error = -P_EINVAL;
        printf("couldn't read symbol from stream\n");
        printf("code 0x%x, bits %d\n", code, bits);
        //dump_ranges(tree);

out:
        return error;
}

/*
 * Deflate streams can opt not to include dymanic huffman trees and instead
 * rely on defaults defined in the standard. This function creates the
 * struct huff_tree representing that encoding defined in the standard. Heads
 * up that there are a lot of magic numbers in this function. The standard
 * gives the following table for the litteral/length alphabets:
 *
 *              Lit Value    Bits        Codes
 *              ---------    ----        -----
 *                0 - 143     8          00110000 through
 *                                       10111111  (0x30 -- 0xbf)
 *              144 - 255     9          110010000 through
 *                                       111111111 (0x190 -- 0x1ff)
 *              256 - 279     7          0000000 through
 *                                       0010111 (0x0 -- 0x17)
 *              280 - 287     8          11000000 through
 *                                       11000111 (0xc0 -- 0xc7)
 *
 * The standard also defines the distance alphabet to be a direct mapping,
 * i.e. all codes just have length 5 and map directly from encoding to actual
 * value.
 *
 * XXX: we don't really need to generate these trees on the fly, we
 * technically know them at compile time
 */
static int make_static_trees(struct zlib_stream *stream)
{
        struct huff_tree *lltree, *dtree;
        struct huff_range *range;
        uint16_t tmp;
        unsigned i, offset;

        lltree = huff_alloc(HUFF_LL_SIZE);
        if (!lltree)
                return -P_ENOMEM;

        dtree = huff_alloc(HUFF_DIST_SIZE);
        if (!dtree) {
                huff_free(lltree);
                return -P_ENOMEM;
        }

        /*
         * Fill out the huffman tree ranges for the length/litteral alphabet.
         * See the table above to make sense of all these constants. Start
         * with the range for codes of length 7
         */
        range = &lltree->h_ranges[0];
        range->r_len = 7;
        range->r_syms = lltree->h_syms;
        range->r_start = 0x0;
        range->r_end =  0x17;
        range->r_count = range->r_end - range->r_start;
        offset = range->r_count;
        for (tmp = 256, i = 0; tmp <= 279; tmp++, i++)
                range->r_syms[i] = SYM_INIT(tmp, range->r_len);

        /* next the range for codes of lenth 8 */
        range = &lltree->h_ranges[1];
        range->r_len = 8;
        range->r_syms = lltree->h_syms + offset;
        range->r_start = 0x30;
        range->r_end = 0xc7;
        range->r_count = range->r_end - range->r_start;
        offset += range->r_count;
        for (tmp = 0, i = 0; tmp <= 143; tmp++, i++)
                range->r_syms[i] = SYM_INIT(tmp, range->r_len);
        for (tmp = 280; tmp <= 287; tmp++, i++)
                range->r_syms[i] = SYM_INIT(tmp, range->r_len);

        /* finally the range for codes of lenth 9 */
        range = &lltree->h_ranges[2];
        range->r_len = 9;
        range->r_syms = lltree->h_syms + offset;
        range->r_start = 0x190;
        range->r_end = 0x1ff;
        range->r_count = range->r_end - range->r_start;
        for (tmp = 144, i = 0; tmp <= 255; tmp++, i++)
                range->r_syms[i] = SYM_INIT(tmp, range->r_len);

        /* fill out the single struct huff_range for the distance alphabet */
        range = &dtree->h_ranges[0];
        range->r_len = 5;
        range->r_syms = dtree->h_syms;
        range->r_start = 0;
        range->r_end = 31;
        range->r_count = range->r_end - range->r_start;
        for (tmp = 0, i = 0; tmp <= 31; tmp++, i++)
                range->r_syms[i] = SYM_INIT(tmp, range->r_len);

        stream->z_lltree = lltree;
        stream->z_dtree = dtree;
        return 0;
}

#define HLIT_BITS 5
#define HDIST_BITS 5
#define HCLEN_BITS 4

#define HLIT_BIAS 257
#define HDIST_BIAS 1
#define HCLEN_BIAS 4

#define CLEN_BITS 3

/*
 * The code length alphabet at the beginning of dymanic chunks is encoded
 * in this order. (See section 3.2.7 of the standard)
 */
static const uint8_t code_length_mapping[] =
{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/*
 * Parse Huffman trees out of a stream. The trees are represented as the
 * lengths of the Huffman encoding of each symbol in the alphabet
 * represented, in order. So in essence, the trees are just a list of
 * lenghts. However, to make things interesting, the lengths are themselves
 * Huffman encoded, so before the actual lengths is another list of lengths
 * representing the Huffman tree with which the subsequent lengths are
 * encoded.
 *
 * Preceeding the lengths are a few lenghts telling us the length of the
 * subsequent sets of lengths (ow my head). They are hlit, hdist, and hclen,
 * and they tell us the number of elements in the litteral alphabet, the
 * distance alphabet, and the code length alphabet respectively. These
 * lengths are biased so they fit in fewer bits.
 *
 * So, to recap, the start of a block looks something like:
 *     - hlit
 *     - hdist
 *     - hclen
 *     - hclen + bias 3-bit lengths encoding the code length Huffman tree
 *     - hlit codes from the code length alphabet encoding the length/
 *       litteral Huffman tree
 *     - hdist lengths from the code length alphabet encoding the distance
 *       Huffman tree
 *
 * To complicate things further, the code length alphabet has the following
 * representation (taken from section 3.2.7 the standard)
 *
 *      0 - 15: Represent code lengths of 0 - 15
 *          16: Copy the previous code length 3 - 6 times.
 *              The next 2 bits indicate repeat length
 *                    (0 = 3, ... , 3 = 6)
 *                 Example:  Codes 8, 16 (+2 bits 11),
 *                           16 (+2 bits 10) will expand to
 *                           12 code lengths of 8 (1 + 6 + 5)
 *          17: Repeat a code length of 0 for 3 - 10 times.
 *              (3 bits of length)
 *          18: Repeat a code length of 0 for 11 - 138 times
 *              (7 bits of length)
 */
static int make_dynamic_trees(struct zlib_stream *stream)
{
        struct huff_tree *cltree, *lltree, *dtree, *tree;
        unsigned hlit, hdist, hclen, i, rcount, offset;
        uint16_t len, prev_len;
        int error;

        /* parse the 3 lengths at the beginning of the tree */
        if (stream_sbytes(stream) < (HLIT_BITS+HDIST_BITS+HCLEN_BITS)/8 + 1)
                return -P_E2SMALL;

        hlit = read_bits(stream, HLIT_BITS) + HLIT_BIAS;
        hdist = read_bits(stream, HDIST_BITS) + HDIST_BIAS;
        hclen = read_bits(stream, HCLEN_BITS) + HCLEN_BIAS;

        printf("hlit: %d, hdist: %d, hclen: %d\n", hlit, hdist, hclen);

        /* allocate and initialize the code length tree */
        cltree = huff_alloc(hclen);
        if (!cltree)
                return -P_ENOMEM;

        for (i = 0; i < hclen; i++) {
                if (stream_sbytes(stream) < 2) {
                        error = -P_E2SMALL;
                        goto free_cltree;
                }

                len = read_bits(stream, 3);
                cltree->h_syms[i] = SYM_INIT(code_length_mapping[i], len);
        }

        printf("initializing ranges for cltree\n");

        error = huff_init_ranges(cltree);
        if (error)
                goto free_cltree;

        /* allocate and initialize length/litteral and distance trees */
        error = -P_ENOMEM;
        lltree = huff_alloc(hlit);
        if (!lltree)
                goto free_cltree;

        dtree = huff_alloc(hdist);
        if (!dtree)
                goto free_lltree;

        printf("about to read dynamic trees\n");

        rcount = 0;
        prev_len = 0;
        tree = lltree;
        offset = 0;
        for (i = 0; i < hlit + hdist; i++) {
                if (i == hlit) {
                        offset = i;
                        tree = dtree;
                }

                /*
                 * we're not repeating the previous value, so read the next
                 * length from the stream
                 */
                if (!rcount) {
                        /*
                         * we don't actually need 3 bytes in all cases
                         * per se, but the logic is simpler if we always
                         * require it, and having anything less while
                         * we're still in the block header is pretty much
                         * impossible
                         *
                         * XXX: is it really though?
                         */
                        if (stream_sbytes(stream) < 3) {
                                error = -P_E2SMALL;
                                goto free_dtree;
                        }

                        error = huff_read(stream, cltree, &len);
                        if (error) {
                                goto free_dtree;
                        }

                        switch (len) {
                        case 16:
                                rcount = read_bits(stream, 2) + 3;
                                break;
                        case 17:
                                rcount = read_bits(stream, 3) + 3;
                                prev_len = 0;
                                break;
                        case 18:
                                rcount = read_bits(stream, 7) + 11;
                                prev_len = 0;
                                break;
                        default:
                                if (len > 18)
                                        BUG();
                        }
                }

                if (rcount) {
                        len = prev_len;
                        rcount--;
                }

                tree->h_syms[i - offset] = SYM_INIT(i - offset, len);
                prev_len = len;
        }

        printf("about to init lltree and dtree ranges\n");
        error = huff_init_ranges(lltree);
        if (error)
                goto free_dtree;
        error = huff_init_ranges(dtree);
        if (error)
                goto free_dtree;

        stream->z_lltree = lltree;
        stream->z_dtree = dtree;

        /*
         * everything succeeded, but we still need to clean up the cltree
         * since it is not used out side of constructing the lltree and
         * dtree
         */
        printf("about to return sucessfully\n");
        error = 0;
        goto free_cltree;

free_dtree:
        huff_free(dtree);
free_lltree:
        huff_free(lltree);
free_cltree:
        huff_free(cltree);
        return error;
}

/*
 * handle decompression for an uncompressed block. (i.e. compression type
 * == non) Starts on byte boundary, and next 4 bytes are a 2 byte
 * big-endian length followed by a 2 byte bit-endian negated length (for
 * integrity).
 */
static int deflate_none(struct zlib_stream *stream)
{
        uint16_t len, nlen;
        int error;

        /* eat any remaining bits in the byte we're in */
        if (stream->z_src_bidx) {
                stream->z_src_bidx = 0;
                stream->z_src_idx++;
        }

        if (stream_sbytes(stream) < 4) {
                printf("stream too small when parsing len and nlen\n");
                return -P_E2SMALL;
        }

        len = read_png_uint16(stream_src(stream));
        stream->z_src_idx += sizeof len;
        nlen = read_png_uint16(stream_src(stream));
        stream->z_src_idx += sizeof nlen;

        if (nlen != ~len) {
                printf("len != ~nlen. len=%x, nlen=%x\n", len, nlen);
                return -P_EINVAL;
        }

        if (len > stream_sbytes(stream)) {
                printf("not enough bytes in stream.\n");
                return -P_EINVAL;
        } else if (len > stream_dbytes(stream)) {
                error = realloc_stream(stream);
                if (error)
                        return error;
        }

        memcpy(stream_dst(stream), stream_src(stream), len);
        stream->z_src_idx += len;
        stream->z_dst_idx += len;

        return 0;
}

#define HUFF_END_OF_BLOCK 256
#define HUFF_LEN_BASE 257
#define HUFF_LL_MAX 285

/*
 * The following arrays of magic are taken from this table from section
 * 3.2.5 of the standard. They are used for reading length and distance
 * values from the input stream. Lengths are encoded as the values 257-285,
 * and each value signifies that a certain number of extra bits should be
 * read from the input stream and added to some base offset. This is
 * defined by the following table
 *
 *             Extra               Extra               Extra
 *       Code Bits Length(s) Code Bits Lengths   Code Bits Length(s)
 *       ---- ---- ------     ---- ---- -------   ---- ---- -------
 *        257   0     3       267   1   15,16     277   4   67-82
 *        258   0     4       268   1   17,18     278   4   83-98
 *        259   0     5       269   2   19-22     279   4   99-114
 *        260   0     6       270   2   23-26     280   4  115-130
 *        261   0     7       271   2   27-30     281   5  131-162
 *        262   0     8       272   2   31-34     282   5  163-194
 *        263   0     9       273   3   35-42     283   5  195-226
 *        264   0    10       274   3   43-50     284   5  227-257
 *        265   1  11,12      275   3   51-58     285   0    258
 *        266   1  13,14      276   3   59-66
 *
 * Distance is encoded similarly:
 *
 *             Extra           Extra               Extra
 *        Code Bits Dist  Code Bits   Dist     Code Bits Distance
 *        ---- ---- ----  ---- ----  ------    ---- ---- --------
 *          0   0    1     10   4     33-48    20    9   1025-1536
 *          1   0    2     11   4     49-64    21    9   1537-2048
 *          2   0    3     12   5     65-96    22   10   2049-3072
 *          3   0    4     13   5     97-128   23   10   3073-4096
 *          4   1   5,6    14   6    129-192   24   11   4097-6144
 *          5   1   7,8    15   6    193-256   25   11   6145-8192
 *          6   2   9-12   16   7    257-384   26   12  8193-12288
 *          7   2  13-16   17   7    385-512   27   12 12289-16384
 *          8   3  17-24   18   8    513-768   28   13 16385-24576
 *          9   3  25-32   19   8   769-1024   29   13 24577-32768
 */

static const uint8_t len_extra_bits[] =
        {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
         4, 4, 4, 4, 5, 5, 5, 5, 0};

static const uint16_t len_base_offsets[] =
        {3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
         15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
         67, 83, 99, 115, 131, 163, 195, 227, 258};

static const uint8_t dist_extra_bits[] =
        {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
         9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static const uint16_t dist_base_offsets[] =
        {1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
         33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
         1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};

/*
 * memcpy does not allow src and dst to overlap, and memmove doesn't
 * actually behave like we want, so we need to write it ourselves.
 * This is a ripe place for optimization in the future.
 */
static void zlib_memcpy(uint8_t *dst, const uint8_t *src, size_t n)
{
        size_t i;

        for (i = 0; i < n; i++) {
                dst[i] = src[i];
        }
}

static int deflate_huffman(struct zlib_stream *stream)
{
        int error;
        uint16_t llvalue, len, dist;
        uint8_t ebits, *start;

        printf("entering %s\n", __func__);

        for (;;) {
                if (stream_sbytes(stream) < 3)
                        return -P_E2SMALL;

                error = huff_read(stream, stream->z_lltree, &llvalue);
                if (error)
                        return error;

                if (llvalue < HUFF_END_OF_BLOCK) {
                        if (!stream_dbytes(stream)) {
                                error = realloc_stream(stream);
                                if (error)
                                        return error;
                        }

                        stream->z_dst[stream->z_dst_idx++] = llvalue;
                } else if (llvalue == HUFF_END_OF_BLOCK) {
                        return 0;
                } else if (llvalue <= HUFF_LL_MAX) {
                        /*
                         * we need at most 5 bits extra for length, 15 bits
                         * for a maximum length distance Huffman code, and
                         * 13 bits extra for distance, which is 33 bits,
                         * so anything more than 4 bytes is okay. We're
                         * always guarenteed this because there's a 4 byte
                         * checksum at the end anyway
                         */
                        if (stream_sbytes(stream) < 5)
                                return -P_E2SMALL;

                        len = len_base_offsets[llvalue - HUFF_LEN_BASE];
                        ebits = len_extra_bits[llvalue - HUFF_LEN_BASE];
                        if (ebits)
                                len += read_bits(stream, ebits);

                        error = huff_read(stream, stream->z_dtree, &dist);
                        if (error)
                                return error;

                        ebits = dist_extra_bits[dist];
                        dist = dist_base_offsets[dist];
                        if (ebits)
                                dist += read_bits(stream, ebits);

                        if (stream_dbytes(stream) < len) {
                                error = realloc_stream(stream);
                                if (error)
                                        return error;
                        }

                        start = stream_dst(stream) - dist;
                        zlib_memcpy(stream_dst(stream), start, len);
                        stream->z_dst_idx += len;
                } else {
                        BUG();
                }
        }

        return 0;
}

#define ADLER_MOD 65521

static uint32_t adler32(const uint8_t *buf, size_t size)
{
        uint32_t s1 = 1;
        uint32_t s2 = 0;
        size_t i;

        /* todo: is the compiler smart enough to get rid of these mods? */
        for (i = 0; i < size; i++) {
                s1 = (s1 + buf[i]) % ADLER_MOD;
                s2 = (s2 + s1) % ADLER_MOD;
        }

        return s2 << 16 | s1;
}

int zlib_decompress(struct zlib_stream *stream)
{
        int error, btype, bfinal;
        uint32_t adler;

        printf("entering zlib_decompress\n");

        stream->z_dst_end = 20*stream->z_src_end;
        stream->z_dst = malloc(stream->z_dst_end);
        if (!stream->z_dst)
                return -P_ENOMEM;

        error = parse_header(stream);
        if (error < 0)
                return error;
        if (!stream_sbytes(stream))
                return -P_E2SMALL;

        do {
                printf("zlib_decompress: entering main loop\n");

                /* read block header from input stream */
                bfinal = read_bits(stream, BLK_BFINAL_BTS);
                btype = read_bits(stream, BLK_BTYPE_BTS);

                printf("bfinal is %d, btype is %d\n", bfinal, btype);

                /* handle block types */
                switch (btype) {
                case BLK_BTYPE_RESERVED:
                        printf("got bad btype\n");
                        return -P_EINVAL;

                case BLK_BTYPE_NONE:
                        printf("zlib_decompress: btype none\n");
                        error = deflate_none(stream);
                        if (error)
                                return error;
                        continue;

                case BLK_BTYPE_DYNAMIC:
                        printf("zlib_decompress: btype dynamic\n");
                        error = make_dynamic_trees(stream);
                        if (error)
                                return error;
                        break;

                case BLK_BTYPE_STATIC:
                        printf("zlib_decompress: btype static\n");
                        error = make_static_trees(stream);
                        if (error)
                                return error;
                        break;

                default:
                        BUG();
                }

                error = deflate_huffman(stream);
                if (error)
                        return error;
        } while (!bfinal);

        /* validate the checksum.. first eat any remaining bits */
        if (stream->z_src_bidx)
                stream->z_src_idx++;
        adler = __read_png_int_raw(stream_src(stream));
        if (adler != adler32(stream->z_dst, stream->z_dst_idx)) {
                printf("adler32 checksum did not match\n");
                return -P_EBADCSUM;
        }

        /* woo we made it */
        printf("inflated stream size is %luK, ", stream->z_dst_idx >> 10);
        printf("compression ratio: %f\n",
               (double)stream->z_dst_idx/(double)stream->z_src_idx);
        return 0;
}
