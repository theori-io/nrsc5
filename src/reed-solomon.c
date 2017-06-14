/**
 * @file reed-solomon.c
 * This is a reed-solomon codec.
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

/* Define the characteristics of the Reed-Solomon codec. */
#define M 8     /* symbol size */
#define E 4     /* number of errors that can be corrected */

/* Do not change below this line. */
#define Q 2                /* code is over a binary field */
#define N ((1 << M) - 1)   /* length of codeword (bytes) */
#define D (E * 2)          /* number of parity symbols */
#define K (N - D)          /* length of message (K < N) */
#define A0 N               /* field.log[A0] = 0 */

#include "reed-solomon.h"
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

static gf_t field;
static uint8_t gen[D+1];

static void rs_generate_generator_polynomial();
static uint32_t rs_calculate_syndromes(const uint8_t msg[N], uint8_t syndromes[D]);
static uint32_t rs_calculate_error_locator_polynomial(const uint8_t syndromes[D], uint8_t errpoly[D + 1]);
static int32_t rs_calculate_error_values(uint32_t errdeg, const uint8_t errpoly[D + 1], uint8_t roots[D + 1], uint8_t locpoly[E]);
static uint32_t rs_generate_error_evaluator_polynomial(const uint8_t syndromes[D], uint32_t errdeg, const uint8_t *errpoly, uint8_t evalpoly[D]);

/**
 * Main program function.
 */
int32_t
rs_init(void)
{
    if(gf_generate_field(&field, M, GF_PRIMPOLY_2_8)) {
		return -1;
    }

    rs_generate_generator_polynomial();

    return 0;
}

/* Find the generator polynomial for the BCH/RS code. */
static void
rs_generate_generator_polynomial()
{
    uint32_t i, j;

    gen[0] = 1;
    for(i = 0; i < D; i++)
    {
        gen[i + 1] = 1;
        j = i;
        for(; j > 0; j--)
        {
            if(gen[j])
            {
                gen[j] = gen[j - 1] ^ field.exp[(field.log[gen[j]] + i + 1) % N];
            }
            else
            {
                gen[j] = gen[j - 1];
            }
        }
        gen[0] = field.exp[(field.log[gen[0]] + i + 1) % N];
    }

    for(i = 0; i <= D; i++)
    {
        gen[i] = field.log[gen[i]];
    }
}

/* Encode a message.
 * Parity information is computed from msg and stored in parity in-place.
 * Messages of length < K will be zero-padded.
 * Returns 0 on completion or -1 on error.
 */
int32_t
rs_encode(const uint8_t msg[], uint32_t len, uint8_t parity[D])
{
    int32_t i, j;
    uint8_t fb;

    /* Message must be length <= K. */
    if(len > K)
    {
        return -1;
    }

    /* Zero-fill the parity bytes. */
    for(i = 0; i < D; i++)
    {
        parity[i] = 0;
    }

    /* Linear feedback shift register. */
    for(i = K - 1; i >= 0; i--)
    {
        fb = field.log[((i >= len) ? 0 : msg[i]) ^ parity[D-1]];
        if(fb != A0)
        {
            for(j = D - 1; j > 0; j--)
            {
                parity[j] = parity[j - 1];
                if(gen[j] != A0)
                {
                    parity[j] ^= field.exp[(gen[j] + fb) % N];
                }
            }
            parity[0] = field.exp[(gen[0] + fb) % N];
        }
        else
        {
            for(j = D - 1; j > 0; j--)
            {
                parity[j] = parity[j - 1];
            }
            parity[0] = 0;
        }
    }

    return 0;
}

/* Decode a message.
 * msg will be modified in-place if there are recoverable errors.
 * Returns the number of errors in the message or -1 if it was unrecoverable.
 */
int32_t
rs_decode(uint8_t msg[N])
{
    int32_t i, j, cnt;
    uint8_t syndromes[D];
    uint8_t errpoly[D + 1];
    uint8_t roots[D + 1];
    uint8_t locpoly[E];
    uint8_t evalpoly[D];
    uint32_t errdeg;
    int32_t rootscnt;
    uint32_t evaldeg;
    uint8_t n1, n2, tmp;

    if(!rs_calculate_syndromes(msg, syndromes))
    {
        return 0;
    }
    /* FIXME: Berlekamp-Massey algorithm implementation is BROKEN! */
    errdeg = rs_calculate_error_locator_polynomial(syndromes, errpoly);
    /* FIXME: Chien search might be broken, too. */
    rootscnt = rs_calculate_error_values(errdeg, errpoly, roots, locpoly);
    if(rootscnt < 0)
    {
        return -1;
    }
    evaldeg = rs_generate_error_evaluator_polynomial(syndromes, errdeg, errpoly, evalpoly);

    cnt = rootscnt;
    for(j = cnt - 1; j >= 0; j--)
    {
        n1 = 0;
        for(i = evaldeg; i >= 0; i--)
        {
            if(evalpoly[i] != A0)
            {
                n1 ^= field.exp[(evalpoly[i] + i * roots[j]) % N];
            }
        }
        n2 = field.exp[(N - roots[j]) % N];
        tmp = 0;
        for(i = MIN(errdeg, D - 1) & ((unsigned int)-1 << 1); i >= 0; i -= 2)
        {
            if(errpoly[i + 1] != A0)
            {
                tmp ^= field.exp[(evalpoly[i] + i * roots[j]) % N];
            }
        }

        if(!tmp)
        {
            return (cnt = -1);
        }

        if(n1 && j < E)
        {
            msg[locpoly[j]] ^= field.exp[(field.log[n1] + field.log[n2] + N - field.log[tmp]) % N];
        }
    }

    return cnt;
}

/* Calculate the syndromes of the message.
 * Returns zero if there are no errors in the message.
 */
static uint32_t
rs_calculate_syndromes(const uint8_t msg[N], uint8_t syndromes[D])
{
    int32_t i, j, err;
    uint8_t tmp;

    for(i = 0; i < D; i++)
    {
        syndromes[i] = msg[0];
    }

    for(i = 1; i < N; i++)
    {
        if(!msg[i])
        {
            continue;
        }
        tmp = field.log[msg[i]];
        for(j = 0; j < D; j++)
        {
            syndromes[j] ^= field.exp[(tmp + j * i) % N];
        }
    }

    err = 0;
    for(i = 0; i < D; i++)
    {
        err += !!syndromes[i];
        syndromes[i] = field.log[syndromes[i]];
    }

    return err;
}

/* Calculate the error locator polynomial for the BCH/RS code.
 * Uses the Berlekamp-Massey Algorithm.
 * Returns the degree of the error polynomial (the number of errors in the message).
 */
static uint32_t
rs_calculate_error_locator_polynomial(const uint8_t syndromes[D], uint8_t errpoly[D + 1])
{
    int32_t i, r, el, discr_r;
    uint32_t deg = 0;
    uint8_t b[D + 1];
    uint8_t t[D + 1];

    errpoly[0] = 1;
    b[0] = field.log[1];
    for(i = 1; i <= D; i++)
    {
        errpoly[i] = 0;
        b[i] = field.log[0];
    }

    el = 0;
    for(r = 0; r < D; r++)
    {
        discr_r = 0;

        for(i = 0; i < r; i++)
        {
            if(errpoly[i] && syndromes[r - i] != A0)
            {
                discr_r ^= field.exp[(errpoly[i] + syndromes[r - i]) % N];
            }
        }
        discr_r = field.log[discr_r];

        if(discr_r == A0)
        {
            for(i = D - 1; i >= 0; i--)
            {
                b[i + 1] = b[i];
            }
            b[0] = A0;
        }
        else
        {
            t[0] = errpoly[0];
            for(i = 0; i < D; i++)
            {
                t[i + 1] = errpoly[i + 1];
                if(b[i] != A0)
                {
                    t[i + 1] ^= field.exp[(discr_r + b[i]) % N];
                }
            }
            if(2 * el <= r)
            {
                el = r - el;
                for(i = 0; i <= D; i++)
                {
                    if(errpoly[i])
                    {
                        b[i] = (field.log[errpoly[i]] - discr_r + N) % N;
                    }
                    else
                    {
                        b[i] = A0;
                    }
                }
            }
            else
            {
                for(i = D - 1; i >= 0; i--)
                {
                    b[i + 1] = b[i];
                }
                b[0] = A0;
            }
            for(i = 0; i <= D; i++)
            {
                errpoly[i] = t[i];
            }
        }
    }

    for(i = 0; i <= D; i++)
    {
        if(errpoly[i])
        {
            deg = i;
        }
        errpoly[i] = field.log[errpoly[i]];
    }

    return deg;
}

/* Calculate the roots of the error locator polynomial given an error locator polynomial and its degree.
 * Also calculate the error values at each location.
 * Uses the Chien search and Forney algoritms.
 * Returns the number of roots of the error polynomial.
 */
static int32_t
rs_calculate_error_values(uint32_t errdeg, const uint8_t errpoly[D + 1], uint8_t roots[D + 1], uint8_t locpoly[E])
{
    int32_t i, j, k, q;
    uint8_t reg[D + 1];
    int32_t rootscnt = 0;

    for(i = 1; i <= D; i++)
    {
        reg[i] = errpoly[i];
    }
    k = N - 1;
    for(i = 1; i <= N; i++)
    {
        q = 1;
        for(j = errdeg; j > 0; j--)
        {
            if(reg[j] != A0)
            {
                reg[j] = (reg[j] + j) % N;
                q ^= field.exp[reg[j]];
            }
        }

        if(q)
        {
            k = (N + k - 1) % N;
            continue;
        }
        roots[rootscnt] = i;
        if(rootscnt < E)
        {
            locpoly[rootscnt] = k;
        }

        if(++rootscnt == errdeg)
        {
            break;
        }
        k = (N + k - 1) % N;
    }

    if(rootscnt != errdeg)
    {
        rootscnt = -1;
    }

    return rootscnt;
}

/* Generate the error evaluator polynomial.
 * Returns the degree of the error evaluator polynomial.
 */
static uint32_t
rs_generate_error_evaluator_polynomial(const uint8_t syndromes[D], uint32_t errdeg, const uint8_t *errpoly, uint8_t evalpoly[D])
{
    int32_t i, j;
    uint32_t deg = 0;
    uint8_t tmp;

    for(i = 0; i < D; i++)
    {
        tmp = 0;
        for(j = (deg < i) ? deg : i; j >= 0; j--)
        {
            if(syndromes[i-j] != A0 && errpoly[j] != A0)
            {
                tmp ^= field.exp[(syndromes[i-j] + errpoly[j]) % N];
            }
        }
        if(tmp)
        {
            deg = i;
        }
        evalpoly[i] = field.log[tmp];
    }

    return deg;
}
