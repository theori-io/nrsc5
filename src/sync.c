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

#include "defines.h"
#include "input.h"
#include "sync.h"

#define BUFS 16

static void dump_ref(uint8_t *ref_buf)
{
    uint32_t value = ref_buf[0];
    for (int i = 1; i < 32; i++)
        value = (value << 1) | (ref_buf[i - 1] ^ ref_buf[i]);
    // printf("REF %08X\n", value);
}

static void calc_phase(float complex *buf, unsigned int ref, float *out_phase, float *out_slope)
{
    float phase, slope, item_phase;
    float complex sum;

    sum = 0;
    for (int r = 0; r < N; r++)
        sum += buf[ref * N + r] * buf[ref * N + r];
    phase = cargf(sum) * 0.5;

    sum = 0;
    for (int r = 1; r < N; r++)
    {
        float complex tmp = conjf(buf[ref * N + r - 1]) * buf[ref * N + r];
        sum += tmp * tmp;
    }
    slope = cargf(sum) * 0.5;

    item_phase = phase + slope * -(N - 1.0) / 2.0;
    if (crealf(buf[ref * N] * cexpf(-I * item_phase)) >= 0)
        *out_phase = M_PI + phase;
    else
        *out_phase = phase;

    *out_slope = slope;
}

static void adjust_ref(float complex *buf, float *phases, unsigned int ref)
{
    float phase, slope;
    calc_phase(buf, ref, &phase, &slope);

    for (int n = 0; n < N; n++)
    {
        float item_phase = phase + slope * (1.0 * n) / 2.0;
        phases[ref * N + n] = item_phase;
        buf[ref * N + n] *= cexpf(-I * item_phase);
    }
}

static int find_first_block (float complex *buf, unsigned int ref)
{
    static const signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, 1, 0, 0, 1, 0, -1, 0, 0, 0, 0, -1, 1, 1, 1
    };
    unsigned char data[N], prev = 0;
    for (int n = 0; n < N; n++)
    {
        unsigned char bit = crealf(buf[ref * N + n]) <= 0 ? 0 : 1;
        data[n] = bit ^ prev;
        prev = bit;
    }

    for (int n = 0; n < N; n++)
    {
        int i;
        for (i = 0; i < sizeof(needle); i++)
        {
            // first bit of data may be wrong, so ignore
            if ((n + i) % N == 0) continue;
            // ignore don't care bits
            if (needle[i] < 0) continue;
            // test if bit is correct
            if (needle[i] != data[(n + i) % N])
                break;
        }
        if (i == sizeof(needle))
            return n;
    }
    return -1;
}

static float calc_smag(float complex *buf, unsigned int ref)
{
    float sum = 0;
    for (int n = 0; n < N; n++)
        sum += fabsf(crealf(buf[ref * N + n]));
    return sum / N;
}

static void adjust_data(float complex *buf, float *phases, unsigned int lower, unsigned int upper)
{
    float smag0, smag19;
    smag0 = calc_smag(buf, lower);
    smag19 = calc_smag(buf, upper);

    for (int n = 0; n < N; n++)
    {
        if (phases[lower * N + n] - phases[upper * N + n] > M_PI)
            phases[upper * N + n] += M_PI * 2;
        if (phases[lower * N + n] - phases[upper * N + n] < M_PI)
            phases[upper * N + n] -= M_PI * 2;
        for (int k = 1; k < 19; k++)
        {
            // average phase difference
            float complex C = CMPLXF(19.0f, 19.0f) / ((19 - k) * smag19 * cexpf(I * phases[upper * N + n]) + k * smag0 * cexpf(I * phases[lower * N + n]));
            // adjust sample
            buf[(lower + 19 - k) * N + n] *= C;
        }
    }
}

void sync_process(sync_t *st)
{
    float complex *buffer = &st->buffer[st->used * N * SYNCLEN];
    unsigned int i;

    for (i = 0; i < BAND_LENGTH; i += 19)
    {
        adjust_ref(buffer, st->phases, i);
        adjust_ref(buffer, st->phases, UB_OFFSET + i);
    }

    // check if we lost synchronization or now have it
    if (st->ready)
    {
        if (decode_get_block(&st->input->decode) == 0 && find_first_block(buffer, 0) != 0)
        {
            printf("lost sync!\n");
            st->ready = 0;
        }
    }
    else
    {
        int offset = find_first_block(buffer, 0);
        if (offset > 0)
        {
            printf("first block @ %d\n", offset);
            input_set_skip(st->input, offset * K);
        }
        else if (offset == 0)
        {
            printf("synchronized!\n");
            decode_reset(&st->input->decode);
            st->ready = 1;
        }
    }

    // if we are still synchronized
    if (st->ready)
    {
        for (i = 0; i < BAND_LENGTH - 1; i += 19)
        {
            adjust_data(buffer, st->phases, i, i + 19);
            adjust_data(buffer, st->phases, UB_OFFSET + i, UB_OFFSET + i + 19);
        }
// #define DEMOD(x) fminf(fmaxf(15*(x), -15), 15)
#define DEMOD(x) ((x) >= 0 ? 1 : -1)
        for (int n = 0; n < N; n++)
        {
            float complex c;
            for (i = 0; i < BAND_LENGTH - 1; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = buffer[(i + j) * N + n];
                    decode_push(&st->input->decode, DEMOD(crealf(c)));
                    decode_push(&st->input->decode, DEMOD(cimagf(c)));
                }
            }
            for (i = 0; i < BAND_LENGTH - 1; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = buffer[(UB_OFFSET + i + j) * N + n];
                    decode_push(&st->input->decode, DEMOD(crealf(c)));
                    decode_push(&st->input->decode, DEMOD(cimagf(c)));
                }
            }

            c = buffer[n];
            st->ref_buf[n] = crealf(c) <= 0 ? 0 : 1;
            if (n == 0) dump_ref(st->ref_buf);
        }
    }
}

void sync_push(sync_t *st, float complex *fftout)
{
    unsigned int i;
    for (i = 0; i < SYNCLEN; ++i)
        st->buffer[i * N + st->idx + st->buf_idx * N * SYNCLEN] = fftout[LB_START + i];

    if (++st->idx == N)
    {
        st->idx = 0;

        pthread_mutex_lock(&st->mutex);
        while ((st->buf_idx + 1) % BUFS == st->used)
            pthread_cond_wait(&st->cond, &st->mutex);
        st->buf_idx = (st->buf_idx + 1) % BUFS;
        pthread_mutex_unlock(&st->mutex);

        pthread_cond_signal(&st->cond);
    }
}

static void *sync_worker(void *arg)
{
    sync_t *st = arg;
    while(1)
    {
        pthread_mutex_lock(&st->mutex);
        while (st->buf_idx == st->used)
            pthread_cond_wait(&st->cond, &st->mutex);
        pthread_mutex_unlock(&st->mutex);

        sync_process(st);

        pthread_mutex_lock(&st->mutex);
        st->used = (st->used + 1) % BUFS;
        pthread_mutex_unlock(&st->mutex);

        pthread_cond_signal(&st->cond);
    }
}

void sync_wait(sync_t *st)
{
    pthread_mutex_lock(&st->mutex);
    while (st->buf_idx != st->used)
        pthread_cond_wait(&st->cond, &st->mutex);
    pthread_mutex_unlock(&st->mutex);
}

void sync_init(sync_t *st, input_t *input)
{
    st->input = input;
    st->buffer = malloc(sizeof(float complex) * N * SYNCLEN * BUFS);
    st->phases = malloc(sizeof(float) * N * SYNCLEN);
    st->ref_buf = malloc(N);
    st->ready = 0;
    st->idx = 0;
    st->used = 0;

    pthread_cond_init(&st->cond, NULL);
    pthread_mutex_init(&st->mutex, NULL);
    pthread_create(&st->worker_thread, NULL, sync_worker, st);
}
