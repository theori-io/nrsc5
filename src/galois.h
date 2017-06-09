/**
 * @file galois.h
 *
 * Copyright (c) 2015 Dan Chokola
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef GALOIS_H
#define GALOIS_H

#include <stdint.h>

/* Maximum number of field elements. We can't dynamically allocate on embedded
 * targets. */
#define GF_MAX 256

/* Define some useful primitive polynomials for generating the Galois field. */
/* GF(2^1) */
#define GF_PRIMPOLY_2_1 0x2   /* 0b10 */
/* GF(2^2) */
#define GF_PRIMPOLY_2_2 0x7   /* 0b111 */
/* GF(2^3) */
#define GF_PRIMPOLY_2_3 0xb   /* 0b1011 */
/* GF(2^4) */
#define GF_PRIMPOLY_2_4 0x13  /* 0b10011 */
/* GF(2^5) */
#define GF_PRIMPOLY_2_5 0x25  /* 0b100101 */
/* GF(2^6) */
#define GF_PRIMPOLY_2_6 0x43  /* 0b1000011 */
/* GF(2^7) */
#define GF_PRIMPOLY_2_7 0x83  /* 0b10000011 */
/* GF(2^8) */
#define GF_PRIMPOLY_2_8 0x11d /* 0b100011101 */

/* Log/Anti-log representation of the Galois field. */
typedef struct gf {
    uint8_t exp[GF_MAX];   /* alpha ^ n */
    uint8_t log[GF_MAX];   /* log(alpha ^ n) */
    uint32_t len;
} gf_t;

int32_t gf_generate_field(gf_t *gf, uint8_t r, uint32_t poly);

#endif /* GALOIS_H */
