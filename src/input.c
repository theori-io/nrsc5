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

#define INPUT_BUF_LEN (2160 * 512)

#ifdef USE_FAST_MATH
#define RESAMP_NUM_TAPS 8
#else
#define RESAMP_NUM_TAPS 16
#endif

static float filter_taps[] = {
#ifdef USE_FAST_MATH
    /*
     * http://t-filter.engineerjs.com/
     *    0 Hz - 80,000 Hz (-40 dB)
     *    120,000 Hz - 200,000 Hz (5 dB)
     *    300,000 Hz - 700,000 Hz (-30 dB)
     */
    0.0753194224484977,
    0.04765846660553231,
    -0.014652799369167866,
    -0.10253099542978061,
    -0.14104873779974123,
    -0.07894851027726302,
    0.05494952393379453,
    0.16461021103340587,
    0.16461021103340587,
    0.05494952393379453,
    -0.07894851027726302,
    -0.14104873779974123,
    -0.10253099542978061,
    -0.014652799369167866,
    0.04765846660553231,
    0.0753194224484977
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
    st->cfo_idx %= FFT;

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

static void measure_power(input_t *st, uint8_t *buf, uint32_t len)
{
    unsigned int i;

    for (i = 0; i < len; i += 2)
        st->agc_power += normf(CMPLXF(U8_F(buf[i]), U8_F(buf[i + 1])));
    st->agc_cnt += len;

    if (st->agc_cnt >= 32768)
    {
        if (st->agc_cb(st->agc_cb_arg, st->agc_power / st->agc_cnt) == 0)
            st->agc_cb = NULL;

        st->agc_cnt = 0;
        st->agc_power = 0;
    }
}

void input_cb(uint8_t *buf, uint32_t len, void *arg)
{
    unsigned int i, new_avail, cnt = len / 4;
    input_t *st = arg;

    if (st->outfp)
        fwrite(buf, 1, len, st->outfp);

    if (st->agc_cb)
    {
        measure_power(st, buf, len);
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
        x[0].i = -U8_Q15(buf[i * 4 + 1]);
        x[1].r = U8_Q15(buf[i * 4 + 2]);
        x[1].i = -U8_Q15(buf[i * 4 + 3]);

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

void input_set_agc_callback(input_t *st, input_agc_cb_t cb, void *arg)
{
    st->agc_cb = cb;
    st->agc_cb_arg = arg;
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
    st->agc_power = 0;
    st->agc_cnt = 0;
}

void input_init(input_t *st, output_t *output, double center, unsigned int program, FILE *outfp)
{
    st->buffer = malloc(sizeof(float complex) * INPUT_BUF_LEN);
    st->output = output;
    st->outfp = outfp;
    st->center = center;
    st->agc_cb = NULL;
    st->agc_cb_arg = NULL;

    st->filter = firdecim_q15_create(2, filter_taps, sizeof(filter_taps) / sizeof(filter_taps[0]));
    st->resamp = resamp_q15_create(RESAMP_NUM_TAPS / 2, 0.45f, 60.0f, 16);

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

void input_aas_push(input_t *st, uint8_t *psd, unsigned int len)
{
    output_aas_push(st->output, psd, len);
}
