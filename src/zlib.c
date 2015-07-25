#include <stdbool.h>
#include <stdio.h>

#include "zlib.h"
#include "error.h"

/* constants for parsing the header */
#define ZLIB_CM_DEFLATE 8
#define ZLIB_WSIZE_MAX (1UL << 15)
#define ZLIB_WSIZE_BIAS 8

ssize_t zlib_decompress(uint8_t *src, size_t src_size, uint8_t **dst)
{
        uint8_t cmi, flg, hdr;
        int log_wsize;
        bool has_dict;
        int zlevel;
        int fcheck;
        (void)dst;

        if (src_size < 3)
                return -P_E2SMALL;

        /* first byte: compression method and info about that method */
        cmi = *src++;

        /* second byte: flags */
        flg = *src++;

        /* first 4 bits: compression method. only deflate supported */
        printf("compression method: %d\n", cmi & 0xf);
        if ((cmi & 0xf) != ZLIB_CM_DEFLATE)
                return -P_EINVAL;

        log_wsize = ((cmi & 0xf0) >> 4) + ZLIB_WSIZE_BIAS;
        printf("log_wsize: %d\n", log_wsize);
        if (1UL << log_wsize > ZLIB_WSIZE_MAX)
                return -P_EINVAL;

        has_dict = flg & 0x10;
        printf("has_dict is: %d\n", has_dict);

        zlevel = (flg & 0xc0) >> 6;
        printf("zlevel is: %d\n", zlevel);

        fcheck = (cmi << 8) + flg;
        if (fcheck % 31) {
                printf("bad fcheck value: %d (%d*256 + %d)\n", fcheck, cmi, flg);
                return -P_EINVAL;
        }

        hdr = *src++;
        printf("BFINAL is: %d\n", hdr & 0x1);
        printf("BTYPE is: %d\n", (hdr & 0x6) >> 1);

        return 2;
}
