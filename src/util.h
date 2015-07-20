#ifndef PNG_UTIL_H
#define PNG_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* random shit goes here */

#define container_of(__ptr, __type, __member)   \
        ((__type *)((char *)(__ptr) - offsetof(__type, __member)))


#define BUG()                                                           \
        do {                                                            \
                fprintf(stderr, "BUG! %s:%d %s", __FILE__, __LINE__, __func__); \
                exit(1);                                                \
        } while (0);                                                    \

#endif /* PNG_UTIL_H */
