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
static unsigned char png_magic[] = {137, 80, 78, 71, 13, 10, 26, 10};


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

size_t parse_magic(const char *buf, size_t buf_size)
{
        unsigned i;
        size_t magic_size;
        
        magic_size = array_size(png_magic);
        if (buf_size < magic_size)
                return 0;
        
        for (i = 0; i < magic_size; i++)
                if (png_magic[i] != (unsigned char)buf[i])
                        return 0;

        return magic_size;
}

int main(int argc, char **argv)
{
        const char *fname;
        const char *fbuf;
        int fd;
        size_t size;
        size_t offset;
        size_t nbytes;
        struct chunk *chunks[50];
        int i, cidx;

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

        printf("start of buff is at %p\n", (void*)fbuf);
        
        cidx = 0;
        do {
                nbytes = parse_next_chunk(fbuf + offset, size - offset,
                                          NULL, &chunks[cidx++]);
                offset += nbytes;
        } while(nbytes);

        if (size != offset) {
                cidx--;
                printf("ended parsing chunks without traversing whole file\n");
        }
        

        for (i = 0; i < cidx; i++) {
                fprintf(stderr, "printing info for %s chunk\n",
                        chunks[i]->c_tmpl->ct_name);
                if (chunks[i]->c_tmpl->ct_ops.print_info)
                        chunks[i]->c_tmpl->ct_ops.print_info(stderr,
                                                             chunks[i]);
                
        }

        munmap((void*)fbuf, size);
        close(fd);
        return 0;
}
