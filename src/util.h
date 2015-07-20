#ifndef PNG_UTIL_H
#define PNG_UTIL_H

#include <stddef.h>

/* random shit goes here */

#define container_of(__ptr, __type, __member)   \
        ((__type *)((char *)(__ptr) - offsetof(__type, __member)))

#endif /* PNG_UTIL_H */
