#include "error.h"

/* mapping from enumerated errors to meaningful messages */

const char *error_to_msg[__P_EMAX] = {
        [P_EOK]       = "okay",
        [P_ENOMEM]    = "out of memory",
        [P_E2SMALL]   = "buffer too small",
        [P_ERANGE]    = "out of range",
        [P_EINVAL]    = "invalid value",
        [P_ENOCHUNK]  = "missing chunk",
        [P_EBADCSUM]  = "bad checksum"
};
