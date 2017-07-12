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
    unsigned int samperr = 0, i, j;
    unsigned int mink = 0, maxk = FFT;

    if (st->idx != FFTCP * (M + 1))
        return;

    if (st->ready && fabsf(st->slope) < 1 && st->samperr > 10)
    {
        mink = st->samperr - 10;
        maxk = st->samperr + 10;
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
    st->samperr = samperr;

    // compare with previous timing offset
    if (abs((int)samperr - (int)st->history[(st->history_size-1) % ACQ_HISTORY]) > FFT/2)
    {
        // clear the history if we "rolled over"
        st->history_size = 0;
    }

    st->history[st->history_size % ACQ_HISTORY] = samperr;
    if (++st->history_size > ACQ_HISTORY)
    {
        float avgerr, slope;
        int sum = 0;
        for (i = 0; i < ACQ_HISTORY; i++)
            sum += st->history[i];
        avgerr = sum / (float)ACQ_HISTORY;
        slope = ((float)samperr - avgerr) / (ACQ_HISTORY / 2 * SYMBOLS);
        st->ready = 1;
        st->slope = slope;

        if ((st->history_size % ACQ_HISTORY) == 0)
            log_debug("Timing offset: %f, slope: %f", avgerr, slope);

        // avoid adjusting the rate too much
        if (fabsf(slope) > 1.0)
        {
            log_info("Timing offset: %f, slope: %f (adjust)", avgerr, slope);

            input_rate_adjust(st->input, (-slope / BLKSZ) / FFTCP);

            // clear the history so we don't overadjust
            st->history_size = 0;
        }
        // we don't want the samperr to go < 0
        else if (slope < 0)
        {
            input_rate_adjust(st->input, (-slope / BLKSZ / 8) / FFTCP);
            st->history_size = 0;
        }

        // skip samples instead of having samperr > FFT
        // NB adjustment must be greater than FFT/2
        if (samperr > 7*FFT/8)
        {
            input_set_skip(st->input, 6*FFT/8);
            st->samperr = 0;
        }
    }

    if (st->ready)
    {
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
    }

    memmove(&st->buffer[0], &st->buffer[st->idx - FFTCP], sizeof(float complex) * FFTCP);
    st->idx = FFTCP;
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
    st->sums = malloc(sizeof(float complex) * FFTCP);
    st->idx = 0;
    st->ready = 0;
    st->samperr = 0;
    st->slope = 0;
    st->prev_angle = 0;

    st->shape = malloc(sizeof(float) * FFTCP);
    for (i = 0; i < FFTCP; ++i)
    {
        // The first CP samples overlap with last CP samples. Due to ISI, we
        // don't want to use the samples on the edges of our symbol.
        // We use the identity: sin^2 x + cos^2 x = 1.
        if (i < CP)
            st->shape[i] = powf(sinf(M_PI / 2 * i / CP), 2);
        else if (i < FFT)
            st->shape[i] = 1;
        else
            st->shape[i] = powf(cosf(M_PI / 2 * (i - FFT) / CP), 2);
    }

    st->history_size = 0;
    for (i = 0; i < ACQ_HISTORY; ++i)
        st->history[i] = 0;

    st->fftin = malloc(sizeof(float complex) * FFT);
    st->fftout = malloc(sizeof(float complex) * FFT);
    st->fft = fftwf_plan_dft_1d(FFT, st->fftin, st->fftout, FFTW_FORWARD, 0);
}
