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

#include "config.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include "defines.h"
#include "input.h"
#include "private.h"

#define INPUT_BUF_LEN (2160 * 512)

/*
 * GNU Radio Filter Design Tool
 * FIR, Low Pass, Kaiser Window
 * Sample rate: 1488375
 * End of pass band: 372094
 * Start of stop band: 530000
 * Stop band attenuation: 40
 */
static float decim_taps[] = {
    0.303116679191589,
    -0.067407339811325,
    0.016459735110402,
    -0.00205476838164
};

static void input_push_to_acquire(input_t *st)
{
    if (st->skip)
    {
        if (st->skip > st->avail - st->used)
        {
            st->skip -= st->avail - st->used;
            st->used = st->avail;
        }
        else
        {
            st->used += st->skip;
            st->skip = 0;
        }
    }

    st->used += acquire_push(&st->acq, &st->buffer[st->used], st->avail - st->used);
}

void input_pdu_push(input_t *st, uint8_t *pdu, unsigned int len, unsigned int program)
{
    output_push(st->output, pdu, len, program);
}

void input_set_skip(input_t *st, unsigned int skip)
{
    st->skip += skip;
}

static void measure_snr(input_t *st, cint16_t *buf, uint32_t len)
{
    unsigned int i, j;

    // use a small FFT to calculate magnitude of frequency ranges
    for (j = 64; j <= len; j += 64)
    {
        for (i = 0; i < 64; i++)
        {
            st->snr_fft_in[i] = cq15_to_cf(buf[i + j - 64]) * pow(sinf(M_PI*i/63), 2);
        }
        fftwf_execute(st->snr_fft);
        fftshift(st->snr_fft_out, 64);

        for (i = 0; i < 64; i++)
            st->snr_power[i] += normf(st->snr_fft_out[i]);
        st->snr_cnt++;
    }

    if (st->snr_cnt >= SNR_FFT_COUNT)
    {
        // noise bands are the frequncies near our signal
        float noise_lo = 0;
        for (i = 19; i < 23; i++)
            noise_lo += st->snr_power[i];
        noise_lo /= 4;
        float noise_hi = 0;
        for (i = 41; i < 45; i++)
            noise_hi += st->snr_power[i];
        noise_hi /= 4;
        // signal bands are the frequencies in our signal
        float signal_lo = (st->snr_power[24] + st->snr_power[25]) / 2;
        float signal_hi = (st->snr_power[39] + st->snr_power[40]) / 2;

        #if 0
        float snr_lo = noise_lo == 0 ? 0 : signal_lo / noise_lo;
        float snr_hi = noise_hi == 0 ? 0 : signal_hi / noise_hi;
        log_debug("%f %f (SNR: %f) %f %f (SNR: %f)", signal_lo, noise_lo, snr_lo, signal_hi, noise_hi, snr_hi);
        #endif

        float signal = (signal_lo + signal_hi) / 2 / st->snr_cnt;
        float noise = (noise_lo + noise_hi) / 2 / st->snr_cnt;
        float snr = signal / noise;

        if (st->snr_cb(st->snr_cb_arg, snr) == 0)
            st->snr_cb = NULL;

        st->snr_cnt = 0;
        for (i = 0; i < 64; ++i)
            st->snr_power[i] = 0;
    }
}

void input_cb(cint16_t *buf, uint32_t len, void *arg)
{
    unsigned int i, avail, new_avail;
    input_t *st = arg;

    if (st->decimation == 4)
    {
        for (i = 0; i < len; i += 2)
        {
            cint16_t x[2];

            x[0].r = buf[i].r;
            x[0].i = buf[i].i;
            x[1].r = buf[i + 1].r;
            x[1].i = buf[i + 1].i;

            halfband_q15_execute(st->firdecim2, x, &buf[i / 2]);
        }
        len /= 2;
    }

    // correct frequency offset
    for (i = 0; i < len; i++)
    {
        st->phase *= st->phase_increment;
        buf[i] = cf_to_cq15(cq15_to_cf(buf[i]) * st->phase);
    }
    st->phase /= cabsf(st->phase);

    if (st->snr_cb)
    {
        measure_snr(st, buf, len);
        return;
    }

    nrsc5_report_iq(st->radio, buf, len * sizeof(buf[0]));

    if (len / 2 + st->avail > INPUT_BUF_LEN)
    {
        if (st->avail > st->used)
        {
            memmove(&st->buffer[0], &st->buffer[st->used], (st->avail - st->used) * sizeof(st->buffer[0]));
            st->avail -= st->used;
            st->used = 0;
        }
        else
        {
            st->avail = 0;
            st->used = 0;
        }
    }
    avail = st->avail;
    new_avail = st->avail;

    if (len / 2 + new_avail > INPUT_BUF_LEN)
    {
        log_error("input buffer overflow!");
        return;
    }

    for (i = 0; i < len; i += 2)
    {
        cint16_t x[2], y;

        x[0].r = buf[i].r;
        x[0].i = -buf[i].i;
        x[1].r = buf[i + 1].r;
        x[1].i = -buf[i + 1].i;

        halfband_q15_execute(st->firdecim, x, &y);
        st->buffer[new_avail++] = y;
    }

    st->avail = new_avail;
    while (st->avail - st->used >= FFTCP)
    {
        input_push_to_acquire(st);
        acquire_process(&st->acq);
    }
}

void input_set_snr_callback(input_t *st, input_snr_cb_t cb, void *arg)
{
    st->snr_cb = cb;
    st->snr_cb_arg = arg;
}

void input_reset(input_t *st)
{
    st->avail = 0;
    st->used = 0;
    st->skip = 0;
    for (int i = 0; i < 64; ++i)
        st->snr_power[i] = 0;
    st->snr_cnt = 0;
}

int input_set_decimation(input_t *st, int decimation)
{
    // we only support two sample rates: 1.488 MHz (2x), 2.977 MHz (4x)
    if (decimation != 2 && decimation != 4)
        return 1;
    st->decimation = decimation;
    return 0;
}

void input_set_freq_offset(input_t *st, float offset)
{
    st->phase_increment = cexpf(2 * M_PI * offset / SAMPLE_RATE * I);
    st->phase = st->phase_increment;
}

void input_init(input_t *st, nrsc5_t *radio, output_t *output)
{
    st->buffer = malloc(sizeof(cint16_t) * INPUT_BUF_LEN);
    st->radio = radio;
    st->output = output;
    st->snr_cb = NULL;
    st->snr_cb_arg = NULL;

    st->phase_increment = cexpf(2 * M_PI * FREQ_OFFSET / SAMPLE_RATE * I);
    st->phase = st->phase_increment;
    st->decimation = 2;
    st->firdecim = firdecim_q15_create(decim_taps, sizeof(decim_taps) / sizeof(decim_taps[0]));
    st->firdecim2 = firdecim_q15_create(decim_taps, sizeof(decim_taps) / sizeof(decim_taps[0]));
    st->snr_fft = fftwf_plan_dft_1d(64, st->snr_fft_in, st->snr_fft_out, FFTW_FORWARD, 0);

    input_reset(st);

    acquire_init(&st->acq, st);
    decode_init(&st->decode, st);
    frame_init(&st->frame, st);
    sync_init(&st->sync, st);
}

void input_aas_push(input_t *st, uint8_t *psd, unsigned int len)
{
    output_aas_push(st->output, psd, len);
}
