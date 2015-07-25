#ifndef PNG_INT_H
#define PNG_INT_H

/*
 * The png spec has some wierd integer type requrements. specifically,
 * as per the spec:
 *
 *     PNG four-byte unsigned integers are limited to the range 0 to 2^31-1
 *     to accommodate languages that have difficulty with unsigned four-byte
 *     values. Similarly PNG four-byte signed integers are limited to the
 *     range -(2^31-1) to 2^31-1 to accommodate languages that have
 *     difficulty with the value -2^31.
 *
 *     Section 7.1 para 2.
 *
 * A little funky -- basically it means unsigned 32 bit ints can't use the
 * top bit and signed 32 ints can't use their maximum and minimum values
 */

#include <stdbool.h>
#include <stdint.h>

#define PNG_INT_MAX ((1L << 31) - 1)
#define PNG_INT_MIN (-PNG_INT_MAX)
#define PNG_UINT_MIN 0
#define PNG_UINT_MAX PNG_INT_MAX

/**
 * \brief Read a 4 byte big endian value from a buffer without bounds
 * checking. Useful to read other 4 byte fields and store them as int32_t's.
 * \param buf    Buffer double pointer. What this points to is moved by
 *               sizeof(int32_t) i.e. 4
 * \return the integer read
 */
static inline int32_t __read_png_int_raw(const uint8_t *buf)
{
        int32_t b0, b1, b2, b3;

        b0 = buf[0];
        b1 = buf[1];
        b2 = buf[2];
        b3 = buf[3];
        
        return b0 << 24 | b1 << 16 | b2 << 8 | b3;
}

/**
 * \brief read a signed integer from a buffer with range validation.
 * \param buf   Buff double pointer. What this points to is moved to
 *              point past the integer field that was parsed.
 * \param out   The integer is written here, regardless of its validity.
 * \returns True if the value read was vaid, otherwise false
 */ 
static inline bool read_png_int(const uint8_t *buf, int32_t *out)
{
        int32_t val;

        val = __read_png_int_raw(buf);
        *out = val;
        
        return PNG_INT_MIN <= val;
}

/**
 * \brief read an unsigned integer from a buffer with range validation.
 * \param buf   Buff double pointer. What this points to is moved to
 *              point past the integer field that was parsed.
 * \param out   The integer is written here, regardless of its validity.
 * \returns True if the value read was vaid, otherwise false
 */ 
static inline bool read_png_uint(const uint8_t *buf, uint32_t *out)
{
        uint32_t val;
        
        val = __read_png_int_raw(buf);
        *out = val;
        
        return PNG_UINT_MIN <= val && val <= PNG_UINT_MAX;
}

static uint16_t read_png_uint16(const uint8_t *buf)
{
        uint16_t b0, b1;

        b0 = buf[0];
        b1 = buf[1];

        return b0 << 8 | b1;
}

#endif /* PNG_INT_H */
