/**
 * @file galois.c
 * Generate a Galois field with the given parameters.
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

#include <stdint.h>

#include "galois.h"

#ifdef MIN
#undef MIN
#endif /* MIN */
#define MIN(a,b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
       _a < _b ? _a : _b; })

#ifdef MAX
#undef MAX
#endif /* MAX */
#define MAX(a,b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
       _a > _b ? _a : _b; })

/**
 * Main program function.
 * Generate the finite field GF(2^r) in a pair of lookup tables. */
int32_t
gf_generate_field(gf_t *gf, uint8_t r, uint32_t poly)
{
    uint32_t i;
    uint32_t tmp;

    if(!gf) {
        return -1;
    }

    /* Make sure we are working with a field that isn't trivially small or
     * too big to fit in our lookup tables. */
    gf->len = 1 << r;
    if(gf->len < 1 || gf->len > GF_MAX) {
        return -1;
    }

    /* The (r-1)th bit of poly should be set if it is the same order as the
     * field. If the rth bit or higher is set, the order is too high. */
    if(!(poly >> r) || poly >> (r + 1)) {
        return -1;
    }

    /* Assign an exponent to the zero element. Use an unused value. */
    gf->exp[gf->len - 1] = 0; /* exp(2^r - 1) = 0 */
    gf->log[0] = gf->len - 1; /* log(0) = 2^r - 1 */
    gf->exp[0] = 1;           /* exp(0) = 1 */
    gf->log[1] = 0;           /* log(1) = 0 */
    for(i = 1; i < gf->len - 1; i++)
    {
        tmp = (uint32_t) gf->exp[i - 1] << 1;
        if(tmp & (1 << r))
        {
            /* subtract (addition and subtraction are both XOR) the generator
             * polynomial to generate a field modulo poly */
            tmp ^= poly;
        }
        gf->exp[i] = tmp;
        gf->log[tmp] = i;
    }

    return 0;
}
