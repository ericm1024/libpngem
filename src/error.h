#ifndef PNG_ERROR_H
#define PNG_ERROR_H
/*
 * some error values to make error handling nicer. the 'P' prefix is
 * for PNG, and it's so these definitions don't collide with things
 */

enum {
        P_EOK = 0, /* no error */
        P_ENOMEM,
        P_E2SMALL,
        P_ERANGE,
        P_EINVAL,
        P_ENOCHUNK,
        P_EBADCSUM,
        __P_EMAX
};

extern const char *error_to_msg[__P_EMAX];

#define e2msg(err) (error_to_msg[err > 0 ? err : -err])

#endif /* PNG_ERROR_H */
