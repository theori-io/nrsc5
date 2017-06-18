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

static float filter_taps[] = {
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
};

static void *input_worker(void *arg)
{
    input_t *st = arg;

    while (1)
    {
        pthread_mutex_lock(&st->mutex);
        while (st->avail - st->used < K)
            pthread_cond_wait(&st->cond, &st->mutex);

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
        pthread_mutex_unlock(&st->mutex);
        pthread_cond_signal(&st->cond);

        acquire_process(&st->acq);
    }

    return NULL;
}

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
    st->skip = skip;
}

void input_wait(input_t *st, int flush)
{
    pthread_mutex_lock(&st->mutex);
    while (st->avail - st->used > (flush ? 1 : 256) * K)
        pthread_cond_wait(&st->cond, &st->mutex);
    pthread_mutex_unlock(&st->mutex);

    if (flush)
    {
        sync_wait(&st->sync);
    }
}

void input_cb(uint8_t *buf, uint32_t len, void *arg)
{
    unsigned int i, new_avail, cnt = len / 4;
    input_t *st = arg;

    if (st->outfp)
        fwrite(buf, 1, len, st->outfp);

    pthread_mutex_lock(&st->mutex);
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
    pthread_mutex_unlock(&st->mutex);

    if (cnt + new_avail > INPUT_BUF_LEN)
        ERR("input buffer overflow!\n");
    assert(len % 4 == 0);

#define U8_F(x) ( (((float)(x)) - 127) / 128 )
#define U8_Q15(x) ( ((int16_t)(x) - 127) << 8 )
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

    pthread_mutex_lock(&st->mutex);
    st->avail = new_avail;
    pthread_mutex_unlock(&st->mutex);
    pthread_cond_signal(&st->cond);
}

void input_reset(input_t *st)
{
    st->avail = 0;
    st->used = 0;
    st->skip = 0;
    st->resamp_rate = 1.0f;
}

void input_init(input_t *st, output_t *output, unsigned int program, FILE *outfp)
{
    st->buffer = malloc(sizeof(float complex) * INPUT_BUF_LEN);
    st->output = output;
    st->outfp = outfp;

    st->filter = firdecim_q15_create(2, filter_taps, sizeof(filter_taps) / sizeof(filter_taps[0]));
    st->resamp = resamp_q15_create(8, 0.45f, 60.0f, 16);

    input_reset(st);

    pthread_cond_init(&st->cond, NULL);
    pthread_mutex_init(&st->mutex, NULL);
    pthread_create(&st->worker_thread, NULL, input_worker, st);

    acquire_init(&st->acq, st);
    decode_init(&st->decode, st);
    frame_init(&st->frame, st);
    frame_set_program(&st->frame, program);
    sync_init(&st->sync, st);
}
