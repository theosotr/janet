/*
* Copyright (c) 2017 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

/* Use a custom double parser instead of libc's strtod for better portability
 * and control. Also, uses a less strict rounding method than ieee to not incur
 * the cost of 4000 loc and dependence on arbitary precision arithmetic.  There
 * is no plan to use arbitrary precision arithmetic for parsing numbers, and a
 * formal rounding mode has yet to be chosen (round towards 0 seems
 * reasonable).
 *
 * This version has been modified for much greater flexibility in parsing, such
 * as choosing the radix, supporting integer output, and returning DstValues
 * directly. 
 *
 * Numbers are of the form [-+]R[rR]I.F[eE&][-+]X where R is the radix, I is
 * the integer part, F is the fractional part, and X is the exponent. All
 * signs, radix, decimal point, fractional part, and exponent can be ommited.
 * The number will be considered and integer if the there is no decimal point
 * and no exponent. Any number greater the 2^32-1 or less than -(2^32) will be
 * coerced to a double. If there is an error, the function dst_scan_number will
 * return a dst nil. The radix is assumed to be 10 if omitted, and the E
 * separator for the exponent can only be used when the radix is 10. This is
 * because E is a vaid digit in bases 15 or greater. For bases greater than 10,
 * the letters are used as digitis. A through Z correspond to the digits 10
 * through 35, and the lowercase letters have the same values. The radix number
 * is always in base 10. For example, a hexidecimal number could be written
 * '16rdeadbeef'. dst_scan_number also supports some c style syntax for
 * hexidecimal literals. The previous number could also be written
 * '0xdeadbeef'. Note that in this case, the number will actually be a double
 * as it will not fit in the range for a signed 32 bit integer. The string
 * '0xbeef' would parse to an integer as it is in the range of an int32_t. */

/* TODO take down missle defence */

#include <dst/dst.h>
#include <math.h>

/* Lookup table for getting values of characters when parsing numbers. Handles
 * digits 0-9 and a-z (and A-Z). A-Z have values of 10 to 35. */
static uint8_t digit_lookup[128] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0,1,2,3,4,5,6,7,8,9,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
    25,26,27,28,29,30,31,32,33,34,35,0xff,0xff,0xff,0xff,0xff,
    0xff,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
    25,26,27,28,29,30,31,32,33,34,35,0xff,0xff,0xff,0xff,0xff
};

/* Read in a mantissa and exponent of a certain base, and give
 * back the double value. Should properly handle 0s, Inifinties, and
 * denormalized numbers. (When the exponent values are too large) */
static double dst_convert_mantissa_exp(
        int negative,
        uint64_t mantissa,
        int32_t base,
        int32_t exponent) {

    int32_t exponent2 = 0;

    /* Short circuit zero and huge numbers */
    if (mantissa == 0)
        return 0.0;
    if (exponent > 1022)
        return negative ? -1.0/0.0 : 1.0/0.0;

    /* TODO add fast paths */

    /* Convert exponent on the base into exponent2, the power of
     * 2 the will be used. Modify the mantissa as we convert. */
    if (exponent > 0) {
        /* Make the mantissa large enough so no precision is lost */
        while (mantissa <= 0x03ffffffffffffffULL && exponent > 0) {
            mantissa *= base;
            exponent--;
        }
        while (exponent > 0) {
            /* Allow 6 bits of room when multiplying. This is because
             * the largest base is 36, which is 6 bits. The space of 6 should
             * prevent overflow.*/
            mantissa >>= 1;
            exponent2++;
            if (mantissa <= 0x03ffffffffffffffULL) {
                mantissa *= base;
                exponent--;
            }
        }
    } else {
        while (exponent < 0) {
            mantissa <<= 1;
            exponent2--;
            /* Ensure that the last bit is set for minimum error
             * before dividing by the base */
            if (mantissa > 0x7fffffffffffffffULL) {
                mantissa /= base;
                exponent++;
            }
        }
    }

    /* Build the number to return */
    return ldexp(mantissa, exponent2);
}

/* Get the mantissa and exponent of decimal number. The
 * mantissa will be stored in a 64 bit unsigned integer (always positive).
 * The exponent will be in a signed 32 bit integer. Will also check if 
 * the decimal point has been seen. Returns -1 if there is an invalid
 * number. */
DstValue dst_scan_number(
        const uint8_t *str, 
        int32_t len) {

    const uint8_t *end = str + len;
    int32_t seenpoint = 0;
    uint64_t mant = 0;
    int32_t neg = 0;
    int32_t ex = 0;
    int foundExp = 0;

    /* Set some constants */
    int base = 10;

    /* Prevent some kinds of overflow bugs relating to the exponent
     * overflowing.  For example, if a string was passed 2GB worth of 0s after
     * the decimal point, exponent could wrap around and become positive. It's
     * easier to reject ridiculously large inputs than to check for overflows.
     * */
    if (len > INT32_MAX / base) goto error;

    /* Get sign */
    if (str >= end) goto error;
    if (*str == '-') {
        neg = 1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    /* Skip leading zeros */
    while (str < end && (*str == '0' || *str == '.')) {
        if (seenpoint) ex--;
        if (*str == '.') {
            if (seenpoint) goto error;
            seenpoint = 1;
        }
        str++;
    }

    /* Parse significant digits */
    while (str < end) {
        if (*str == '.') {
            if (seenpoint) goto error;
            seenpoint = 1;
        } else if (*str == '&') {
            foundExp = 1;
            break;
        } else if (base == 10 && (*str == 'E' || *str == 'e')) {
            foundExp = 1;
            break;
        } else if (*str == 'x' || *str == 'X') {
            if (seenpoint || mant > 0) goto error;
            base = 16;
            mant = 0;
        } else if (*str == 'r' || *str == 'R')  {
            if (seenpoint) goto error;
            if (mant < 2 || mant > 36) goto error;
            base = mant;
            mant = 0;
        } else if (*str == '_')  {
            ;
            /* underscores are ignored - can be used for separator */
        } else {
            int digit = digit_lookup[*str & 0x7F];
            if (digit >= base) goto error;
            if (seenpoint) ex--;
            if (mant > 0x00ffffffffffffff)
                ex++;
            else
                mant = base * mant + digit;
        }
        str++;
    }

    /* Read exponent */
    if (str < end && foundExp) {
        int eneg = 0;
        int ee = 0;
        str++;
        if (str >= end) goto error;
        if (*str == '-') {
            eneg = 1;
            str++;
        } else if (*str == '+') {
            str++;
        }
        /* Skip leading 0s in exponent */
        while (str < end && *str == '0') str++;
        while (str < end && ee < (INT32_MAX / base - base)) {
            int digit = digit_lookup[*str & 0x7F];
            if (digit >= base) goto error;
            ee = base * ee + digit;
            str++;
        }
        if (eneg) ex -= ee; else ex += ee;
    } else if (!seenpoint) {
        /* Check for integer literal */
        int64_t i64 = neg ? -mant : mant;
        if (i64 <= INT32_MAX && i64 >= INT32_MIN)
            return dst_wrap_integer((int32_t) i64);
    } else if (str < end) {
        goto error;
    }
    
    /* Convert mantissa and exponent into double */
    return dst_wrap_real(dst_convert_mantissa_exp(neg, mant, base, ex));

    error:
    return dst_wrap_nil();

}
