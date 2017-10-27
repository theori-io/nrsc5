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
#define M (BLKSZ * SYMBOLS)

void acquire_process(acquire_t *st)
{
    float complex max_v = 0;
    float angle, max_mag = -1.0f;
    unsigned int samperr = 0, i, j, keep;
    unsigned int mink = 0, maxk = FFTCP;

    if (st->idx != FFTCP * (M + 1))
        return;

    if (st->input->sync.ready)
    {
        mink = FFTCP / 2 - 25;
        maxk = FFTCP / 2 + 25;
    }

    memset(st->sums, 0, sizeof(float complex) * FFTCP);
    for (i = mink; i < maxk + CP; ++i)
    {
        for (j = 0; j < M; ++j)
            st->sums[i] += st->buffer[i + j * FFTCP] * conjf(st->buffer[i + j * FFTCP + FFT]);
    }

    for (i = mink; i < maxk - 1; ++i)
    {
        float mag;
        float complex v = 0;

        for (j = 0; j < CP; ++j)
            v += st->sums[(i + j) % FFTCP];

        mag = normf(v);
        if (mag > max_mag)
        {
            max_mag = mag;
            max_v = v;
            samperr = i;
        }
    }

    // limited to (-pi, pi)
    angle = cargf(max_v);
    if (st->prev_angle)
    {
        if (st->prev_angle > M_PI*15/16 && angle < -M_PI*15/16)
            angle += M_PI * 2;
        else if (st->prev_angle < -M_PI*15/16 && angle > M_PI*15/16)
            angle -= M_PI * 2;
        angle = 0.5 * st->prev_angle + 0.5 * angle;
    }
    st->prev_angle = angle;

    for (i = 0; i < M; ++i)
    {
        int j;
        for (j = 0; j < FFTCP; ++j)
        {
            int n = i * FFTCP + j;
            float complex sample = fast_cexpf(angle * n / FFT) * st->buffer[i * FFTCP + j + samperr];
            if (j < CP)
                st->fftin[j] = st->shape[j] * sample;
            else if (j < FFT)
                st->fftin[j] = sample;
            else
                st->fftin[j - FFT] += st->shape[j] * sample;
        }

        fftwf_execute(st->fft);
        fftshift(st->fftout, FFT);
        sync_push(&st->input->sync, st->fftout);
    }

    keep = FFTCP * 3 / 2 - samperr;
    memmove(&st->buffer[0], &st->buffer[st->idx - keep], sizeof(float complex) * keep);
    st->idx = keep;
}

unsigned int acquire_push(acquire_t *st, float complex *buf, unsigned int length)
{
    unsigned int needed = FFTCP - st->idx % FFTCP;

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
    st->buffer = malloc(sizeof(float complex) * FFTCP * (M + 1));
    st->sums = malloc(sizeof(float complex) * (FFTCP + CP));
    st->idx = 0;
    st->prev_angle = 0;

    st->shape = malloc(sizeof(float) * FFTCP);
    for (i = 0; i < FFTCP; ++i)
    {
        // Pulse shaping window function
        if (i < CP)
            st->shape[i] = sinf(M_PI / 2 * i / CP);
        else if (i < FFT)
            st->shape[i] = 1;
        else
            st->shape[i] = cosf(M_PI / 2 * (i - FFT) / CP);
    }

    st->fftin = malloc(sizeof(float complex) * FFT);
    st->fftout = malloc(sizeof(float complex) * FFT);
    st->fft = fftwf_plan_dft_1d(FFT, st->fftin, st->fftout, FFTW_FORWARD, 0);
}
