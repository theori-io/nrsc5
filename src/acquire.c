/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <string.h>

#include "acquire.h"
#include "defines.h"
#include "input.h"

#define SYMBOLS 2
#define M (N * SYMBOLS)
#define WINDOW 16
static int window[WINDOW];
static unsigned int window_size;

static inline float normf(float complex v)
{
    float realf = crealf(v);
    float imagf = cimagf(v);
    return realf * realf + imagf * imagf;
}

void acquire_process(acquire_t *st)
{
    float complex max_v = 0;
    float angle, freqerr, max_mag = -1.0f;
    unsigned int samperr = 0, i, j;
    unsigned int mink = 0, maxk = K - CP;

    if (st->idx != K * (M + 1))
        return;

    if (st->samperr >= 3 && st->samperr < K-CP-4)
    {
        mink = st->samperr - 3;
        maxk = st->samperr + 4;
    }

    memset(st->sums, 0, sizeof(float complex) * K);
    for (i = mink; i < maxk + CP; ++i)
    {
        for (j = 0; j < M; ++j)
            st->sums[i] += st->buffer[i + j * K] * conjf(st->buffer[i + j * K + FFT]) * st->shape[i];
    }

    for (i = mink; i < maxk - 1; ++i)
    {
        float mag;
        float complex v = 0;

        for (j = 0; j < CP; ++j)
            v += st->sums[(i + j) % K];

        mag = normf(v);
        if (mag > max_mag)
        {
            max_mag = mag;
            max_v = v;
            samperr = i;
        }
    }

    angle = cargf(max_v);
    freqerr = (-744187.5 / (2 * M_PI * FFT)) * angle;
    window[window_size % WINDOW] = samperr;
    if (++window_size > WINDOW)
    {
        float avgerr, slope;
        int sum = 0;
        for (i = 0; i < WINDOW; i++)
            sum += window[i];
        avgerr = sum / (float)WINDOW;
        slope = ((float)samperr - window[window_size % WINDOW]) / (WINDOW * SYMBOLS);
        printf("avg: %f, slope: %f, freqerr: %f\n", avgerr, slope, freqerr);
        st->ready = 1;
        st->samperr = avgerr;

        if (slope < 0)
        {
            // TODO adjust sample rate upwards
            window_size = 0;
        }
        else if (slope > 0)
        {
            // TODO adjust sample rate downwards
            window_size = 0;
        }
    }

    if (st->ready)
    {
        for (i = 0; i < M; ++i)
        {
            int j;
            for (j = 0; j < FFT; ++j)
            {
                int n = i * K + j;
                float complex adj = cexpf(-I * (float)(2 * M_PI * freqerr * n / 744187.5));
                st->fftin[j] = adj * st->buffer[n + samperr] * st->shape[j];
            }

            fft_execute(st->fft);
            fft_shift(st->fftout, FFT);
            sync_push(&st->input->sync, st->fftout);
        }
    }

    memmove(&st->buffer[0], &st->buffer[st->idx - K], sizeof(float complex) * K);
    st->idx = K;
}

unsigned int acquire_push(acquire_t *st, float complex *buf, unsigned int length)
{
    unsigned int needed = K - st->idx % K;

    if (length < needed)
        return 0;

    memcpy(&st->buffer[st->idx], buf, sizeof(float complex) * needed);
    st->idx += needed;

    return needed;
}

void acquire_init(acquire_t *st, input_t *input)
{
    int i;

    st->input = input;
    st->buffer = malloc(sizeof(float complex) * K * (M + 1));
    st->sums = malloc(sizeof(float complex) * K);
    st->idx = 0;
    st->ready = 0;
    st->samperr = 0;

    st->sintbl = malloc(sizeof(float) * CP);
    for (i = 0; i < CP; ++i)
        st->sintbl[i] = sinf(i * M_PI / CP);

    st->shape = malloc(sizeof(float) * (CP + FFT));
    for (i = 0; i < CP; ++i)
        st->shape[i] = cosf(M_PI * (CP - i) / 224.0);
    for (; i < FFT; ++i)
        st->shape[i] = 1;
    for (; i < FFT + CP; ++i)
        st->shape[i] = cosf(M_PI * (FFT - i) / 224.0);

    st->fftin = malloc(sizeof(float complex) * FFT);
    st->fftout = malloc(sizeof(float complex) * FFT);
    st->fft = fft_create_plan(FFT, st->fftin, st->fftout, LIQUID_FFT_FORWARD, 0);
}
