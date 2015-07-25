/*
 * Eric Mueller -- hacky PNG decoding
 */

#include "chunk.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define array_size(a) (sizeof (a) / sizeof (a[0]))

/* magic 8 bytes at the beginning of an image */
static uint8_t png_magic[] = {137, 80, 78, 71, 13, 10, 26, 10};


/* print an error message and bail */
void error(const char *msg)
{
        fprintf(stderr, "%s\n", msg);
        exit(1);
}

/* stat a file and read its size */
size_t get_fsize(int fd)
{
        struct stat s;
        int ret;

        ret = fstat(fd, &s);
        if (ret == -1)
                error("fstat barfed");

        return s.st_size;
}

size_t parse_magic(const uint8_t *buf, size_t buf_size)
{
        unsigned i;
        size_t magic_size;
        
        magic_size = array_size(png_magic);
        if (buf_size < magic_size)
                return 0;
        
        for (i = 0; i < magic_size; i++)
                if (png_magic[i] != buf[i])
                        return 0;

        return magic_size;
}

int main(int argc, char **argv)
{
        const char *fname;
        const uint8_t *fbuf;
        int fd;
        size_t size;
        size_t offset;
        ssize_t ret;
        struct chunk *chunk;
        struct png_image image;

        image.first = NULL;

        if (argc < 2)
                error("must provide a filename");
        
        fname = argv[1];
        fd = open(fname, O_RDONLY);
        if (fd == -1)
                error("open failed");

        size = get_fsize(fd);
        fbuf = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (fbuf == MAP_FAILED)
                error("mmap failed");

        offset = parse_magic(fbuf, size);
        if (!offset)
                error("failed to parse magic");

        printf("start of buff is at %p, end at %p\n", (void*)fbuf,
               (void*)(fbuf + size));

        for (;;) {
                printf("offset is %zu\n", offset);
                ret = parse_next_chunk(fbuf + offset, size - offset, &image);
                if (ret < 0)
                        break;

                offset += ret;
        }

        if (size != offset)
                printf("ended parsing chunks without traversing whole file\n");
        
        chunk = image.first;
        while (chunk) {
                fprintf(stderr, "printing info for %s chunk\n",
                        chunk->c_tmpl->ct_name);
                
                if (chunk->c_tmpl->ct_ops.print_info)
                        chunk->c_tmpl->ct_ops.print_info(stderr,chunk);

                chunk = chunk->next;
        }

        munmap((void*)fbuf, size);
        close(fd);
        return 0;
}
