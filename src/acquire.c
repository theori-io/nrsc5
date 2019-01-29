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

#define FILTER_DELAY 15

static float filter_taps[] = {
    -0.000685643230099231,
    0.005636964458972216,
    0.009015781804919243,
    -0.015486305579543114,
    -0.035108357667922974,
    0.017446253448724747,
    0.08155813068151474,
    0.007995186373591423,
    -0.13311293721199036,
    -0.0727422907948494,
    0.15914097428321838,
    0.16498781740665436,
    -0.1324498951435089,
    -0.2484012246131897,
    0.051773931831121445,
    0.2821577787399292,
    0.051773931831121445,
    -0.2484012246131897,
    -0.1324498951435089,
    0.16498781740665436,
    0.15914097428321838,
    -0.0727422907948494,
    -0.13311293721199036,
    0.007995186373591423,
    0.08155813068151474,
    0.017446253448724747,
    -0.035108357667922974,
    -0.015486305579543114,
    0.009015781804919243,
    0.005636964458972216,
    -0.000685643230099231,
    0
};

void acquire_process(acquire_t *st)
{
    float complex max_v = 0, phase_increment;
    float angle, angle_diff, angle_factor, max_mag = -1.0f;
    int samperr = 0;
    unsigned int i, j, keep;
    unsigned int mink = 0, maxk = FFTCP;

    if (st->idx != FFTCP * (ACQUIRE_SYMBOLS + 1))
        return;

    if (st->input->sync.ready)
    {
        samperr = FFTCP / 2 + st->input->sync.samperr;
        st->input->sync.samperr = 0;

        angle_diff = -st->input->sync.angle;
        st->input->sync.angle = 0;
        angle = st->prev_angle + angle_diff;
        st->prev_angle = angle;
    }
    else
    {
        cint16_t y;
        for (i = 0; i < FFTCP * (ACQUIRE_SYMBOLS + 1); i++)
        {
            fir_q15_execute(st->filter, &st->in_buffer[i], &y);
            st->buffer[i] = cq15_to_cf(y);
        }

        memset(st->sums, 0, sizeof(float complex) * FFTCP);
        for (i = mink; i < maxk + CP; ++i)
        {
            for (j = 0; j < ACQUIRE_SYMBOLS; ++j)
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
                samperr = (i + FFTCP - FILTER_DELAY) % FFTCP;
            }
        }

        angle_diff = cargf(max_v * cexpf(I * -st->prev_angle));
        angle_factor = (st->prev_angle) ? 0.25 : 1.0;
        angle = st->prev_angle + (angle_diff * angle_factor);
        st->prev_angle = angle;
    }

    for (i = 0; i < FFTCP * (ACQUIRE_SYMBOLS + 1); i++)
        st->buffer[i] = cq15_to_cf(st->in_buffer[i]);

    sync_adjust(&st->input->sync, FFTCP / 2 - samperr);
    angle -= 2 * M_PI * st->cfo;

    st->phase *= cexpf(-(FFTCP / 2 - samperr) * angle / FFT * I);

    phase_increment = cexpf(angle / FFT * I);
    for (i = 0; i < ACQUIRE_SYMBOLS; ++i)
    {
        int j;
        for (j = 0; j < FFTCP; ++j)
        {
            float complex sample = st->phase * st->buffer[i * FFTCP + j + samperr];
            if (j < CP)
                st->fftin[j] = st->shape[j] * sample;
            else if (j < FFT)
                st->fftin[j] = sample;
            else
                st->fftin[j - FFT] += st->shape[j] * sample;

            st->phase *= phase_increment;
        }
        st->phase /= cabsf(st->phase);

        fftwf_execute(st->fft);
        fftshift(st->fftout, FFT);
        sync_push(&st->input->sync, st->fftout);
    }

    keep = FFTCP + (FFTCP / 2 - samperr);
    memmove(&st->in_buffer[0], &st->in_buffer[st->idx - keep], sizeof(cint16_t) * keep);
    st->idx = keep;
}

void acquire_cfo_adjust(acquire_t *st, int cfo)
{
    float hz;

    if (cfo == 0)
        return;

    st->cfo += cfo;
    hz = st->cfo * 744187.5 / FFT;

    log_info("CFO: %f Hz", hz);
}

unsigned int acquire_push(acquire_t *st, cint16_t *buf, unsigned int length)
{
    unsigned int needed = FFTCP - st->idx % FFTCP;

    if (length < needed)
        return 0;

    memcpy(&st->in_buffer[st->idx], buf, sizeof(cint16_t) * needed);
    st->idx += needed;

    return needed;
}

void acquire_init(acquire_t *st, input_t *input)
{
    int i;

    st->input = input;
    st->filter = firdecim_q15_create(filter_taps, sizeof(filter_taps) / sizeof(filter_taps[0]));
    st->idx = 0;
    st->prev_angle = 0;
    st->phase = 1;
    st->cfo = 0;

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

    st->fft = fftwf_plan_dft_1d(FFT, st->fftin, st->fftout, FFTW_FORWARD, 0);
}

void acquire_free(acquire_t *st)
{
    firdecim_q15_free(st->filter);
    fftwf_destroy_plan(st->fft);
}
