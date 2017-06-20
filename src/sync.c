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
    float phase, slope;
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

    *out_phase = phase;
    *out_slope = slope;
}

static void adjust_ref(float complex *buf, float *phases, unsigned int ref)
{
    // sync bits (after DBPSK)
    static const signed char sync[] = {
        -1, 1, -1, -1, -1, 1, 1
    };
    float phase, slope;
    calc_phase(buf, ref, &phase, &slope);

    for (int n = 0; n < N; n++)
    {
        float item_phase = phase + slope * (n - ((N-1)/2));
        phases[ref * N + n] = item_phase;
        buf[ref * N + n] *= cexpf(-I * item_phase);
    }

    // compare to sync bits
    float x = 0;
    for (int n = 0; n < sizeof(sync); n++)
        x += crealf(buf[ref * N + n]) * sync[n];
    if (x < 0)
    {
        // adjust phase by pi to compensate
        for (int n = 0; n < N; n++)
        {
            phases[ref * N + n] += M_PI;
            buf[ref * N + n] *= -1;
        }
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

static int find_ref (float complex *buf, unsigned int ref, unsigned int rsid)
{
    signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, rsid >> 1, rsid & 1, 0, (rsid >> 1) ^ (rsid & 1), 0, -1, -1, -1, -1, -1, -1, 1, 1, 1
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
    // phase was already corrected, so imaginary component is zero
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
        for (int k = 1; k < 19; k++)
        {
            // average phase difference
            float complex C = CMPLXF(19,19) / (k * smag19 * cexpf(I * phases[upper * N + n]) + (19 - k) * smag0 * cexpf(I * phases[lower * N + n]));
            // adjust sample
            buf[(lower + k) * N + n] *= C;
        }
    }
}

void sync_process(sync_t *st)
{
    float complex *buffer = &st->buffer[st->used * N * FFT];
    int i;

    for (i = 0; i < BAND_LENGTH; i += 19)
    {
        adjust_ref(buffer, st->phases, LB_START + i);
        adjust_ref(buffer, st->phases, UB_START + i);
    }

    // check if we lost synchronization or now have it
    if (st->ready)
    {
        if (decode_get_block(&st->input->decode) == 0 && find_first_block(buffer, LB_START + 0) != 0)
        {
            if (find_first_block(buffer, UB_START + 19 * 10) != 0)
            {
                printf("lost sync (%d, %d)!\n", find_first_block(buffer, LB_START), find_first_block(buffer, UB_START + 19*10));
                st->ready = 0;
            }
        }
    }
    else
    {
        int offset = find_first_block(buffer, LB_START + 0);
        if (offset > 0)
        {
            printf("First block @ %d\n", offset);
            input_set_skip(st->input, offset * K);
        }
        else if (offset == 0)
        {
            printf("Synchronized!\n");
            decode_reset(&st->input->decode);
            st->ready = 1;
        }
        else if (st->cfo_wait == 0)
        {
            for (i = -300; i < 300; ++i)
            {
                int j, offset2;
                adjust_ref(buffer, st->phases, LB_START + i);
                offset = find_ref(buffer, LB_START + i, 2);
                if (offset < 0)
                    continue;
                // We think we found the start. Check upperband to confirm.
                adjust_ref(buffer, st->phases, UB_START + 19 * 10 + i);
                offset2 = find_ref(buffer, UB_START + 19 * 10 + i, 2);
                if (offset2 == offset)
                {
                    // The offsets matched, so 'i' is likely the CFO.
                    input_set_skip(st->input, offset * K);
                    input_cfo_adjust(st->input, i);

                    // Wait until the buffers have cleared before measuring again.
                    st->cfo_wait = (st->buf_idx + 2 - st->used) % BUFS;
                    break;
                }
            }
        }
        else
        {
            // Decrease wait counter.
            st->cfo_wait--;
        }
    }

    // if we are still synchronized
    if (st->ready)
    {
        for (i = 0; i < BAND_LENGTH - 1; i += 19)
        {
            adjust_data(buffer, st->phases, LB_START + i, LB_START + i + 19);
            adjust_data(buffer, st->phases, UB_START + i, UB_START + i + 19);
        }

        // Calculate modulation error
        float error_lb = 0, error_ub = 0;
        for (int n = 0; n < N; n++)
        {
            float complex c, ideal;
            for (i = 0; i < BAND_LENGTH - 1; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = buffer[(LB_START + i + j) * N + n];
                    ideal = CMPLXF(crealf(c) >= 0 ? 1 : -1, cimagf(c) >= 0 ? 1 : -1);
                    error_lb += normf(ideal - c);

                    c = buffer[(UB_START + i + j) * N + n];
                    ideal = CMPLXF(crealf(c) >= 0 ? 1 : -1, cimagf(c) >= 0 ? 1 : -1);
                    error_ub += normf(ideal - c);
                }
            }
        }

        st->error_lb += error_lb;
        st->error_ub += error_ub;

        // Display average MER for each sideband
        if (++st->mer_cnt == 16)
        {
            float signal = 2 * N * 180 * st->mer_cnt;
            printf("MER: %f dB (lower), %f dB (upper)\n", signal / st->error_lb, signal / st->error_ub);
            st->mer_cnt = 0;
            st->error_lb = 0;
            st->error_ub = 0;
        }

        // Soft demod based on MER for each sideband
        float mer_lb = 2 * N * 180 / error_lb;
        float mer_ub = 2 * N * 180 / error_ub;
        float mult_lb = fmaxf(fminf(mer_lb * 10, 63), 1);
        float mult_ub = fmaxf(fminf(mer_ub * 10, 63), 1);

#define DEMOD(x) ((x) >= 0 ? 1 : -1)
        for (int n = 0; n < N; n++)
        {
            float complex c;
            for (i = 0; i < BAND_LENGTH - 1; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = buffer[(LB_START + i + j) * N + n];
                    decode_push(&st->input->decode, DEMOD(crealf(c)) * mult_lb);
                    decode_push(&st->input->decode, DEMOD(cimagf(c)) * mult_lb);
                }
            }
            for (i = 0; i < BAND_LENGTH - 1; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = buffer[(UB_START + i + j) * N + n];
                    decode_push(&st->input->decode, DEMOD(crealf(c)) * mult_ub);
                    decode_push(&st->input->decode, DEMOD(cimagf(c)) * mult_ub);
                }
            }

            c = buffer[LB_START + n];
            st->ref_buf[n] = crealf(c) <= 0 ? 0 : 1;
            if (n == 0) dump_ref(st->ref_buf);
        }
    }
}

void sync_push(sync_t *st, float complex *fftout)
{
    unsigned int i;
    for (i = 0; i < FFT; ++i)
        st->buffer[i * N + st->idx + st->buf_idx * N * FFT] = fftout[i];

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
    st->buffer = malloc(sizeof(float complex) * N * FFT * BUFS);
    st->phases = malloc(sizeof(float) * N * FFT);
    st->ref_buf = malloc(N);
    st->ready = 0;
    st->idx = 0;
    st->used = 0;
    st->cfo_wait = 0;
    st->mer_cnt = 0;
    st->error_lb = 0;
    st->error_ub = 0;

    pthread_cond_init(&st->cond, NULL);
    pthread_mutex_init(&st->mutex, NULL);
    pthread_create(&st->worker_thread, NULL, sync_worker, st);
}
