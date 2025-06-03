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
#include "private.h"

#define FILTER_DELAY 15
#define DECIMATION_FACTOR_FM 2
#define DECIMATION_FACTOR_AM 32

static float filter_taps_fm[] = {
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

static float filter_taps_am[] = {
    -0.00038464731187559664,
    -0.00021618751634377986,
    0.0026779419276863337,
    -0.00029802651260979474,
    -0.0012626448879018426,
    -0.0013182522961869836,
    -0.012252614833414555,
    0.015980124473571777,
    0.037112727761268616,
    -0.05451361835002899,
    -0.05804193392395973,
    0.11320608854293823,
    0.055298302322626114,
    -0.16878043115139008,
    -0.022917453199625015,
    0.19178225100040436,
    -0.022917453199625015,
    -0.16878043115139008,
    0.055298302322626114,
    0.11320608854293823,
    -0.05804193392395973,
    -0.05451361835002899,
    0.037112727761268616,
    0.015980124473571777,
    -0.012252614833414555,
    -0.0013182522961869836,
    -0.0012626448879018426,
    -0.00029802651260979474,
    0.0026779419276863337,
    -0.00021618751634377986,
    -0.00038464731187559664,
    0
};

void acquire_process(acquire_t *st)
{
    float complex max_v = 0, phase_increment;
    float angle, angle_diff, angle_factor, max_mag = -1.0f;
    int samperr = 0;
    int i, j, keep;

    if (st->idx != (unsigned int)st->fftcp * (ACQUIRE_SYMBOLS + 1))
        return;

    output_advance(st->input->output);

    if (st->input->sync_state == SYNC_STATE_FINE)
    {
        samperr = st->fftcp / 2 + st->input->sync.samperr;
        st->input->sync.samperr = 0;

        angle_diff = -st->input->sync.angle;
        st->input->sync.angle = 0;
        angle = st->prev_angle + angle_diff;
        st->prev_angle = angle;
    }
    else
    {
        cint16_t y;
        for (i = 0; i < st->fftcp * (ACQUIRE_SYMBOLS + 1); i++)
        {
            fir_q15_execute((st->mode == NRSC5_MODE_FM) ? st->filter_fm : st->filter_am, &st->in_buffer[i], &y);
            st->buffer[i] = (st->mode == NRSC5_MODE_FM) ? cq15_to_cf_conj(y) : cq15_to_cf(y);
        }

        memset(st->sums, 0, sizeof(float complex) * st->fftcp);
        for (i = 0; i < st->fftcp; ++i)
        {
            for (j = 0; j < ACQUIRE_SYMBOLS; ++j)
                st->sums[i] += st->buffer[i + j * st->fftcp] * conjf(st->buffer[i + j * st->fftcp + st->fft]);
        }

        for (i = 0; i < st->fftcp; ++i)
        {
            float mag;
            float complex v = 0;

            for (j = 0; j < st->cp; ++j)
                v += st->sums[(i + j) % st->fftcp] * st->shape[j] * st->shape[j + st->fft];

            mag = normf(v);
            if (mag > max_mag)
            {
                max_mag = mag;
                max_v = v;
                samperr = (i + st->fftcp - FILTER_DELAY) % st->fftcp;
            }
        }

        angle_diff = cargf(max_v * cexpf(I * -st->prev_angle));
        angle_factor = (st->prev_angle) ? 0.25 : 1.0;
        angle = st->prev_angle + (angle_diff * angle_factor);
        st->prev_angle = angle;
        input_set_sync_state(st->input, SYNC_STATE_COARSE);
    }

    for (i = 0; i < st->fftcp * (ACQUIRE_SYMBOLS + 1); i++)
        st->buffer[i] = (st->mode == NRSC5_MODE_FM) ? cq15_to_cf_conj(st->in_buffer[i]) : cq15_to_cf(st->in_buffer[i]);

    sync_adjust(&st->input->sync, st->fftcp / 2 - samperr);
    angle -= 2 * M_PI * st->cfo;

    st->phase *= cexpf(-(st->fftcp / 2 - samperr) * angle / st->fft * I);

    phase_increment = cexpf(angle / st->fft * I);

    if (st->mode == NRSC5_MODE_AM)
    {
        float y, sum_y = 0, sum_xy = 0, sum_x2 = 0;
        float complex last_carrier;
        float complex temp_phase = st->phase;
        float mag_sums[FFT_AM] = {0};

        for (i = 0; i < ACQUIRE_SYMBOLS; ++i)
        {
            int offset = (st->mode == NRSC5_MODE_FM) ? 0 : (FFT_AM - CP_AM) / 2;
            for (j = 0; j < st->fftcp; ++j)
            {
                float complex sample = temp_phase * st->buffer[i * st->fftcp + j + samperr];
                if (j < st->cp)
                    st->fftin[(j + offset) % st->fft] = st->shape[j] * sample;
                else if (j < st->fft)
                    st->fftin[(j + offset) % st->fft] = sample;
                else
                    st->fftin[(j + offset) % st->fft] += st->shape[j] * sample;

                temp_phase *= phase_increment;
            }
            temp_phase /= cabsf(temp_phase);

            fftwf_execute((st->mode == NRSC5_MODE_FM) ? st->fft_plan_fm : st->fft_plan_am);
            fftshift(st->fftout, st->fft);

            float x = st->fftcp * (i - (float) (ACQUIRE_SYMBOLS - 1) / 2);
            if (i == 0)
                y = cargf(st->fftout[CENTER_AM]);
            else
                y += cargf(st->fftout[CENTER_AM] / last_carrier);
            last_carrier = st->fftout[CENTER_AM];

            sum_y += y;
            sum_xy += x * y;
            sum_x2 += x * x;

            if (st->input->sync_state != SYNC_STATE_FINE)
            {
                for (j = CENTER_AM - PIDS_OUTER_INDEX_AM; j <= CENTER_AM + PIDS_OUTER_INDEX_AM; j++)
                {
                    mag_sums[j] += cabsf(st->fftout[j]);
                }
            }
        }

        if (st->input->sync_state != SYNC_STATE_FINE)
        {
            float max_mag = -1.0f;
            int max_index = -1;
            for (j = CENTER_AM - PIDS_OUTER_INDEX_AM; j <= CENTER_AM + PIDS_OUTER_INDEX_AM; j++)
            {
                if (mag_sums[j] > max_mag)
                {
                    max_mag = mag_sums[j];
                    max_index = j;
                }
            }
            acquire_cfo_adjust(st, max_index - CENTER_AM);
        }

        phase_increment *= cexpf(-sum_xy / sum_x2 * I);
        // TODO: Investigate why 0.06 is needed below
        st->phase *= cexpf((-sum_y / ACQUIRE_SYMBOLS + (sum_xy / sum_x2)*(ACQUIRE_SYMBOLS)*st->fftcp/2 - 0.06) * I);
    }

    for (i = 0; i < ACQUIRE_SYMBOLS; ++i)
    {
        int offset = (st->mode == NRSC5_MODE_FM) ? 0 : (FFT_AM - CP_AM) / 2;
        for (j = 0; j < st->fftcp; ++j)
        {
            float complex sample = st->phase * st->buffer[i * st->fftcp + j + samperr];
            if (j < st->cp)
                st->fftin[(j + offset) % st->fft] = st->shape[j] * sample;
            else if (j < st->fft)
                st->fftin[(j + offset) % st->fft] = sample;
            else
                st->fftin[(j + offset) % st->fft] += st->shape[j] * sample;

            st->phase *= phase_increment;
        }
        st->phase /= cabsf(st->phase);

        fftwf_execute((st->mode == NRSC5_MODE_FM) ? st->fft_plan_fm : st->fft_plan_am);
        fftshift(st->fftout, st->fft);
        sync_push(&st->input->sync, st->fftout);
    }

    keep = st->fftcp + (st->fftcp / 2 - samperr) + st->keep_extra;
    st->keep_extra = 0;
    memmove(&st->in_buffer[0], &st->in_buffer[st->idx - keep], sizeof(cint16_t) * keep);
    st->idx = keep;
}

void acquire_keep_extra(acquire_t *st, int extra)
{
    st->keep_extra = extra;
}

void acquire_cfo_adjust(acquire_t *st, int cfo)
{
    st->cfo += cfo;
}

unsigned int acquire_push(acquire_t *st, cint16_t *buf, unsigned int length)
{
    unsigned int needed = st->fftcp - st->idx % st->fftcp;

    if (length < needed)
        return 0;

    memcpy(&st->in_buffer[st->idx], buf, sizeof(cint16_t) * needed);
    st->idx += needed;

    return needed;
}

void acquire_reset(acquire_t *st)
{
    firdecim_q15_reset(st->filter_fm);
    firdecim_q15_reset(st->filter_am);
    st->idx = 0;
    st->prev_angle = 0;
    st->phase = 1;
    st->keep_extra = 0;
    st->cfo = 0;
}

void acquire_init(acquire_t *st, input_t *input)
{
    int i;

    st->mode = NRSC5_MODE_FM;
    st->fft = FFT_FM;
    st->fftcp = FFTCP_FM;
    st->cp = CP_FM;

    st->input = input;

    st->filter_fm = firdecim_q15_create(filter_taps_fm, sizeof(filter_taps_fm) / sizeof(filter_taps_fm[0]));
    st->filter_am = firdecim_q15_create(filter_taps_am, sizeof(filter_taps_am) / sizeof(filter_taps_am[0]));

    pthread_mutex_lock(&fftw_mutex);
    st->fft_plan_fm = fftwf_plan_dft_1d(FFT_FM, st->fftin, st->fftout, FFTW_FORWARD, FFTW_ESTIMATE);
    st->fft_plan_am = fftwf_plan_dft_1d(FFT_AM, st->fftin, st->fftout, FFTW_FORWARD, FFTW_ESTIMATE);
    pthread_mutex_unlock(&fftw_mutex);

    for (i = 0; i < FFTCP_FM; ++i)
    {
        // Pulse shaping window function for FM
        if (i < CP_FM)
            st->shape_fm[i] = sinf(M_PI / 2 * i / CP_FM);
        else if (i < FFT_FM)
            st->shape_fm[i] = 1;
        else
            st->shape_fm[i] = cosf(M_PI / 2 * (i - FFT_FM) / CP_FM);
    }

    for (i = 0; i < FFTCP_AM; ++i)
    {
        // Pulse shaping window function for AM
        if (i < CP_AM)
            st->shape_am[i] = sinf(M_PI / 2 * i / CP_AM);
        else if (i < FFT_AM)
            st->shape_am[i] = 1;
        else
            st->shape_am[i] = cosf(M_PI / 2 * (i - FFT_AM) / CP_AM);
    }

    st->shape = st->shape_fm;

    acquire_reset(st);
}

void acquire_set_mode(acquire_t *st, int mode)
{
    st->mode = mode;

    if (st->mode == NRSC5_MODE_FM)
    {
        st->fft = FFT_FM;
        st->fftcp = FFTCP_FM;
        st->cp = CP_FM;
        st->shape = st->shape_fm;
    }
    else
    {
        st->fft = FFT_AM;
        st->fftcp = FFTCP_AM;
        st->cp = CP_AM;
        st->shape = st->shape_am;
    }
}

void acquire_free(acquire_t *st)
{
    firdecim_q15_free(st->filter_fm);
    firdecim_q15_free(st->filter_am);

    pthread_mutex_lock(&fftw_mutex);
    fftwf_destroy_plan(st->fft_plan_fm);
    fftwf_destroy_plan(st->fft_plan_am);
    pthread_mutex_unlock(&fftw_mutex);
}
