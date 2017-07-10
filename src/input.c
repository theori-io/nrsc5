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

#include <assert.h>
#include <math.h>
#include <string.h>

#include "defines.h"
#include "input.h"

#define INPUT_BUF_LEN (2160 * 512)

#ifdef USE_FAST_MATH
#define RESAMP_NUM_TAPS 8
#else
#define RESAMP_NUM_TAPS 16
#endif

static float filter_taps[] = {
#ifdef USE_FAST_MATH
    0.021421859565886596,
    0.049732162378963,
    0.01818268402571698,
    -0.03934178091446433,
    -0.09570241119049687,
    -0.07252690527100883,
    0.021370885603405092,
    0.11086952366497926,
    0.11086952366497926,
    0.021370885603405092,
    -0.07252690527100883,
    -0.09570241119049687,
    -0.03934178091446433,
    0.01818268402571698,
    0.049732162378963,
    0.021421859565886596
#else
    -0.006910541036924275,
    -0.013268228805145532,
    -0.006644557670245421,
    0.018375039238181595,
    0.04259143500924495,
    0.03712705276833042,
    0.0017215227032129474,
    -0.024593813581821018,
    -0.009907236685353248,
    0.01767132823382834,
    -0.008287758762202712,
    -0.10098124598840287,
    -0.17157955612468512,
    -0.10926609589776617,
    0.08158909906685183,
    0.25361698433482543,
    0.25361698433482543,
    0.08158909906685183,
    -0.10926609589776617,
    -0.17157955612468512,
    -0.10098124598840287,
    -0.008287758762202712,
    0.01767132823382834,
    -0.009907236685353248,
    -0.024593813581821018,
    0.0017215227032129474,
    0.03712705276833042,
    0.04259143500924495,
    0.018375039238181595,
    -0.006644557670245421,
    -0.013268228805145532,
    -0.006910541036924275
#endif
};

static void input_push_to_acquire(input_t *st)
{
    // CFO is modified in sync, and is expected to be "immediately" applied
    for (unsigned int j = st->used + st->cfo_used; j < st->avail; j++)
        st->buffer[j] *= st->cfo_tbl[st->cfo_idx++ % FFT];

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
    st->cfo_used = st->avail - st->used;
}

#ifdef USE_THREADS
static void *input_worker(void *arg)
{
    input_t *st = arg;

    while (1)
    {
        pthread_mutex_lock(&st->mutex);
        while (st->avail - st->used < FFTCP)
            pthread_cond_wait(&st->cond, &st->mutex);

        input_push_to_acquire(st);

        pthread_mutex_unlock(&st->mutex);
        pthread_cond_signal(&st->cond);

        acquire_process(&st->acq);
    }

    return NULL;
}
#endif

void input_pdu_push(input_t *st, uint8_t *pdu, unsigned int len)
{
    output_push(st->output, pdu, len);
}

void input_rate_adjust(input_t *st, float adj)
{
    st->resamp_rate += adj;
}

void input_set_skip(input_t *st, unsigned int skip)
{
    st->skip += skip;
}

void input_cfo_adjust(input_t *st, int cfo)
{
    if (cfo == 0)
        return;

    st->cfo += cfo;
    float hz = st->cfo * 744187.5 / FFT;
    log_info("CFO: %f Hz (%d ppm)", hz, (int)round(hz * 1000000.0 / st->center));

    for (int i = 0; i < FFT; ++i)
        st->cfo_tbl[i] *= cexpf(-I * (float)(2 * M_PI * st->cfo * i / FFT));
}

void input_wait(input_t *st, int flush)
{
#ifdef USE_THREADS
    pthread_mutex_lock(&st->mutex);
    while (st->avail - st->used > (flush ? 1 : 256) * FFTCP)
        pthread_cond_wait(&st->cond, &st->mutex);
    pthread_mutex_unlock(&st->mutex);

    if (flush)
    {
        sync_wait(&st->sync);
    }
#endif
}

static void measure_snr(input_t *st, uint8_t *buf, uint32_t len)
{
    unsigned int i, j;

    // use a small FFT to calculate magnitude of frequency ranges
    for (j = 64; j <= len / 2; j += 64)
    {
        for (i = 0; i < 64; i++)
            st->snr_fft_in[i] = CMPLXF(U8_F(buf[(i+j-64) * 2 + 0]), U8_F(buf[(i+j-64) * 2 + 1])) * pow(sinf(M_PI*i/63),2);
        fftwf_execute(st->snr_fft);
        fftshift(st->snr_fft_out, 64);

        for (i = 0; i < 64; i++)
            st->snr_power[i] += normf(st->snr_fft_out[i]);
        st->snr_cnt++;
    }

    if (st->snr_cnt > 2048)
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

        if (st->snr_cb(st->snr_cb_arg, snr, signal, noise) == 0)
            st->snr_cb = NULL;

        st->snr_cnt = 0;
        for (i = 0; i < 64; ++i)
            st->snr_power[i] = 0;
    }
}

void input_cb(uint8_t *buf, uint32_t len, void *arg)
{
    unsigned int i, new_avail, cnt = len / 4;
    input_t *st = arg;

    if (st->outfp)
        fwrite(buf, 1, len, st->outfp);

    if (st->snr_cb)
    {
        measure_snr(st, buf, len);
        return;
    }

#ifdef USE_THREADS
    pthread_mutex_lock(&st->mutex);
#endif
    if (cnt + st->avail > INPUT_BUF_LEN)
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
    new_avail = st->avail;
    resamp_q15_set_rate(st->resamp, st->resamp_rate);
#ifdef USE_THREADS
    pthread_mutex_unlock(&st->mutex);
#endif

    if (cnt + new_avail > INPUT_BUF_LEN)
    {
        log_error("input buffer overflow!");
        return;
    }
    assert(len % 4 == 0);

    for (i = 0; i < cnt; i++)
    {
        unsigned int nw;
        cint16_t x[2], y;

        x[0].r = U8_Q15(buf[i * 4 + 0]);
        x[0].i = U8_Q15(buf[i * 4 + 1]);
        x[1].r = U8_Q15(buf[i * 4 + 2]);
        x[1].i = U8_Q15(buf[i * 4 + 3]);

        firdecim_q15_execute(st->filter, x, &y);
        resamp_q15_execute(st->resamp, &y, &st->buffer[new_avail], &nw);

        new_avail += nw;
    }

#ifdef USE_THREADS
    pthread_mutex_lock(&st->mutex);
    st->avail = new_avail;
    pthread_mutex_unlock(&st->mutex);
    pthread_cond_signal(&st->cond);
#else
    st->avail = new_avail;
    while (st->avail - st->used >= FFTCP)
    {
        input_push_to_acquire(st);
        acquire_process(&st->acq);
    }
#endif
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
    st->resamp_rate = 1.0f;
    st->cfo = 0;
    st->cfo_idx = 0;
    st->cfo_used = 0;
    for (int i = 0; i < FFT; ++i)
        st->cfo_tbl[i] = 1;
    for (int i = 0; i < 64; ++i)
        st->snr_power[i] = 0;
    st->snr_cnt = 0;
}

void input_init(input_t *st, output_t *output, double center, unsigned int program, FILE *outfp)
{
    st->buffer = malloc(sizeof(float complex) * INPUT_BUF_LEN);
    st->output = output;
    st->outfp = outfp;
    st->center = center;
    st->snr_cb = NULL;
    st->snr_cb_arg = NULL;

    st->filter = firdecim_q15_create(2, filter_taps, sizeof(filter_taps) / sizeof(filter_taps[0]));
    st->resamp = resamp_q15_create(RESAMP_NUM_TAPS / 2, 0.45f, 60.0f, 16);
    st->snr_fft = fftwf_plan_dft_1d(64, st->snr_fft_in, st->snr_fft_out, FFTW_FORWARD, 0);

    input_reset(st);

#ifdef USE_THREADS
    pthread_cond_init(&st->cond, NULL);
    pthread_mutex_init(&st->mutex, NULL);
    pthread_create(&st->worker_thread, NULL, input_worker, st);
#ifdef HAVE_PTHREAD_SETNAME_NP
    pthread_setname_np(st->worker_thread, "worker");
#endif
#endif

    acquire_init(&st->acq, st);
    decode_init(&st->decode, st);
    frame_init(&st->frame, st);
    frame_set_program(&st->frame, program);
    sync_init(&st->sync, st);
}

void input_psd_push(char *psd, unsigned int len)
{
    output_psd_push(psd, len);
}
