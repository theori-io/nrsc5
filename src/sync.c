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

#define BUFS 4

float prev_slope[FFT];

static void dump_ref(uint8_t *ref_buf)
{
    uint32_t value = ref_buf[0];
    for (int i = 1; i < 32; i++)
        value = (value << 1) | (ref_buf[i - 1] ^ ref_buf[i]);
    // log_debug("REF %08X", value);
}

static void calc_phase(float complex *buf, unsigned int ref, float *out_phase, float *out_slope)
{
    float phase, slope;
    float complex sum;

    sum = 0;
    for (int r = 0; r < BLKSZ; r++)
        sum += buf[ref * BLKSZ + r] * buf[ref * BLKSZ + r];
    phase = cargf(sum) * 0.5;

    sum = 0;
    for (int r = 1; r < BLKSZ; r++)
    {
        float complex tmp = conjf(buf[ref * BLKSZ + r - 1]) * buf[ref * BLKSZ + r];
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

    if (prev_slope[ref])
        slope = slope * 0.1 + prev_slope[ref] * 0.9;
    prev_slope[ref] = slope;

    for (int n = 0; n < BLKSZ; n++)
    {
        float item_phase = phase + slope * (n - ((BLKSZ-1)/2));
        phases[ref * BLKSZ + n] = item_phase;
        buf[ref * BLKSZ + n] *= cexpf(-I * item_phase);
    }

    // compare to sync bits
    float x = 0;
    for (unsigned int n = 0; n < sizeof(sync); n++)
        x += crealf(buf[ref * BLKSZ + n]) * sync[n];
    if (x < 0)
    {
        // adjust phase by pi to compensate
        for (int n = 0; n < BLKSZ; n++)
        {
            phases[ref * BLKSZ + n] += M_PI;
            buf[ref * BLKSZ + n] *= -1;
        }
    }
}

static int find_first_block (float complex *buf, unsigned int ref, int *psmi)
{
    static const signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, 1, 0, 0, 1, 0, -1, 0, 0, 0, 0, -1, 1, 1, 1
    };
    unsigned char data[BLKSZ], prev = 0;
    *psmi = -1;
    for (int n = 0; n < BLKSZ; n++)
    {
        unsigned char bit = crealf(buf[ref * BLKSZ + n]) <= 0 ? 0 : 1;
        data[n] = bit ^ prev;
        prev = bit;
    }

    for (int n = 0; n < BLKSZ; n++)
    {
        unsigned int i;
        for (i = 0; i < sizeof(needle); i++)
        {
            // first bit of data may be wrong, so ignore
            if ((n + i) % BLKSZ == 0) continue;
            // ignore don't care bits
            if (needle[i] < 0) continue;
            // test if bit is correct
            if (needle[i] != data[(n + i) % BLKSZ])
                break;
        }
        if (i == sizeof(needle)) {
            if (n == 0) {
                *psmi = (data[25] << 5) | (data[26] << 4) | (data[27] << 3) | (data[28] << 2) | (data[29] << 1) | data[30];
            }
            return n;
        }
    }
    return -1;
}

static int find_ref (float complex *buf, unsigned int ref, unsigned int rsid)
{
    signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, rsid >> 1, rsid & 1, 0, (rsid >> 1) ^ (rsid & 1), 0, -1, -1, -1, -1, -1, -1, 1, 1, 1
    };
    unsigned char data[BLKSZ], prev = 0;
    for (int n = 0; n < BLKSZ; n++)
    {
        unsigned char bit = crealf(buf[ref * BLKSZ + n]) <= 0 ? 0 : 1;
        data[n] = bit ^ prev;
        prev = bit;
    }

    for (int n = 0; n < BLKSZ; n++)
    {
        unsigned int i;
        for (i = 0; i < sizeof(needle); i++)
        {
            // first bit of data may be wrong, so ignore
            if ((n + i) % BLKSZ == 0) continue;
            // ignore don't care bits
            if (needle[i] < 0) continue;
            // test if bit is correct
            if (needle[i] != data[(n + i) % BLKSZ])
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
    for (int n = 0; n < BLKSZ; n++)
        sum += fabsf(crealf(buf[ref * BLKSZ + n]));
    return sum / BLKSZ;
}

static void adjust_data(float complex *buf, float *phases, unsigned int lower, unsigned int upper)
{
    float smag0, smag19;
    smag0 = calc_smag(buf, lower);
    smag19 = calc_smag(buf, upper);

    for (int n = 0; n < BLKSZ; n++)
    {
        for (int k = 1; k < 19; k++)
        {
            // average phase difference
            float complex C = CMPLXF(19,19) / (k * smag19 * fast_cexpf(phases[upper * BLKSZ + n]) + (19 - k) * smag0 * fast_cexpf(phases[lower * BLKSZ + n]));
            // adjust sample
            buf[(lower + k) * BLKSZ + n] *= C;
        }
    }
}

void sync_process(sync_t *st, float complex *buffer)
{
    int i;
    static int psmi = 1;
    unsigned int partitions_per_band;

    switch (psmi) {
        case 2:
            partitions_per_band = 11;
            break;
        case 3:
            partitions_per_band = 12;
            break;
        case 5:
        case 6:
        case 11:
            partitions_per_band = 14;
            break;
        default:
            partitions_per_band = 10;
    }

    for (i = 0; i < partitions_per_band * 19 + 1; i += 19)
    {
        adjust_ref(buffer, st->phases, LB_START + i);
        adjust_ref(buffer, st->phases, UB_END - i);
    }

    // check if we lost synchronization or now have it
    if (st->ready)
    {
        if (decode_get_block(&st->input->decode) == 0 && find_first_block(buffer, LB_START, &psmi) != 0)
        {
            if (find_first_block(buffer, UB_END, &psmi) != 0)
            {
                log_debug("lost sync (%d, %d)!", find_first_block(buffer, LB_START, &psmi), find_first_block(buffer, UB_END, &psmi));
                st->ready = 0;
            }
        }
    }
    else
    {
        for (i = 0; i < FFT; i++)
            prev_slope[i] = 0;

        // First and last reference subcarriers have the same data. Try both
        // in case one of the sidebands is too corrupted.
        int offset = find_first_block(buffer, LB_START, &psmi);
        if (offset < 0)
            offset = find_first_block(buffer, UB_END, &psmi);

        if (offset > 0)
        {
            log_debug("First block @ %d", offset);
            input_set_skip(st->input, offset * FFTCP);
        }
        else if (offset == 0)
        {
            log_info("Synchronized!");
            decode_reset(&st->input->decode);
            st->ready = 1;
        }
        else if (st->cfo_wait == 0)
        {
            for (i = -300; i < 300; ++i)
            {
                int offset2;
                adjust_ref(buffer, st->phases, LB_START + i + P1_BAND_LENGTH - 1);
                offset = find_ref(buffer, LB_START + i + P1_BAND_LENGTH - 1, 0);
                if (offset < 0)
                    continue;
                // We think we found the start. Check upperband to confirm.
                adjust_ref(buffer, st->phases, UB_END + i - P1_BAND_LENGTH + 1);
                offset2 = find_ref(buffer, UB_END + i - P1_BAND_LENGTH + 1, 0);
                if (offset2 == offset)
                {
                    // The offsets matched, so 'i' is likely the CFO.
                    input_set_skip(st->input, offset * FFTCP);
                    input_cfo_adjust(st->input, i);

                    log_debug("First block @ %d", offset);

                    // Wait until the buffers have cleared before measuring again.
                    st->cfo_wait = 2 * BUFS;
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
        for (i = 0; i < partitions_per_band * 19; i += 19)
        {
            adjust_data(buffer, st->phases, LB_START + i, LB_START + i + 19);
            adjust_data(buffer, st->phases, UB_END - i - 19, UB_END - i);
        }

        // Calculate modulation error
        float error_lb = 0, error_ub = 0;
        for (int n = 0; n < BLKSZ; n++)
        {
            float complex c, ideal;
            for (i = 0; i < partitions_per_band * 19; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = buffer[(LB_START + i + j) * BLKSZ + n];
                    ideal = CMPLXF(crealf(c) >= 0 ? 1 : -1, cimagf(c) >= 0 ? 1 : -1);
                    error_lb += normf(ideal - c);

                    c = buffer[(UB_END - i - 19 + j) * BLKSZ + n];
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
            float signal = 2 * BLKSZ * P1_DATA_PER_BAND * st->mer_cnt;
            float mer_db_lb = 10 * log10f(signal / st->error_lb);
            float mer_db_ub = 10 * log10f(signal / st->error_ub);
            log_info("MER: %f dB (lower), %f dB (upper)", mer_db_lb, mer_db_ub);
            st->mer_cnt = 0;
            st->error_lb = 0;
            st->error_ub = 0;
        }

        // Soft demod based on MER for each sideband
        float mer_lb = 2 * BLKSZ * P1_DATA_PER_BAND / error_lb;
        float mer_ub = 2 * BLKSZ * P1_DATA_PER_BAND / error_ub;
        float mult_lb = fmaxf(fminf(mer_lb * 10, 127), 1);
        float mult_ub = fmaxf(fminf(mer_ub * 10, 127), 1);

#define DEMOD(x) ((x) >= 0 ? 1 : -1)
        for (int n = 0; n < BLKSZ; n++)
        {
            float complex c;
            for (i = LB_START; i < LB_START + 190; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = buffer[(i + j) * BLKSZ + n];
                    decode_push_pm(&st->input->decode, DEMOD(crealf(c)) * mult_lb);
                    decode_push_pm(&st->input->decode, DEMOD(cimagf(c)) * mult_lb);
                }
            }
            for (i = UB_END - 190; i < UB_END; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = buffer[(i + j) * BLKSZ + n];
                    decode_push_pm(&st->input->decode, DEMOD(crealf(c)) * mult_ub);
                    decode_push_pm(&st->input->decode, DEMOD(cimagf(c)) * mult_ub);
                }
            }
            if (psmi == 3) {
                for (i = LB_START + 190; i < LB_START + 190 + 38; i += 19)
                {
                    unsigned int j;
                    for (j = 1; j < 19; j++)
                    {
                        c = buffer[(i + j) * BLKSZ + n];
                        decode_push_px1(&st->input->decode, DEMOD(crealf(c)) * mult_lb);
                        decode_push_px1(&st->input->decode, DEMOD(cimagf(c)) * mult_lb);
                    }
                }
                for (i = UB_END - 190 - 38; i < UB_END - 190; i += 19)
                {
                    unsigned int j;
                    for (j = 1; j < 19; j++)
                    {
                        c = buffer[(i + j) * BLKSZ + n];
                        decode_push_px1(&st->input->decode, DEMOD(crealf(c)) * mult_ub);
                        decode_push_px1(&st->input->decode, DEMOD(cimagf(c)) * mult_ub);
                    }
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
        st->buffer[i * BLKSZ + st->idx + st->buf_idx * BLKSZ * FFT] = fftout[i];

    if (++st->idx == BLKSZ)
    {
        st->idx = 0;

#ifdef USE_THREADS
        pthread_mutex_lock(&st->mutex);
        while ((st->buf_idx + 1) % BUFS == st->used)
            pthread_cond_wait(&st->cond, &st->mutex);
        st->buf_idx = (st->buf_idx + 1) % BUFS;
        pthread_mutex_unlock(&st->mutex);

        pthread_cond_signal(&st->cond);
#else
        sync_process(st, st->buffer);
#endif
    }
}

#ifdef USE_THREADS
static void *sync_worker(void *arg)
{
    sync_t *st = arg;
    while(1)
    {
        pthread_mutex_lock(&st->mutex);
        while (st->buf_idx == st->used)
            pthread_cond_wait(&st->cond, &st->mutex);
        pthread_mutex_unlock(&st->mutex);

        sync_process(st, &st->buffer[st->used * BLKSZ * FFT]);

        pthread_mutex_lock(&st->mutex);
        st->used = (st->used + 1) % BUFS;
        pthread_mutex_unlock(&st->mutex);

        pthread_cond_signal(&st->cond);
    }

    return NULL;
}
#endif

void sync_wait(sync_t *st)
{
#ifdef USE_THREADS
    pthread_mutex_lock(&st->mutex);
    while (st->buf_idx != st->used)
        pthread_cond_wait(&st->cond, &st->mutex);
    pthread_mutex_unlock(&st->mutex);
#endif
}

void sync_init(sync_t *st, input_t *input)
{
    st->input = input;
    st->buffer = malloc(sizeof(float complex) * BLKSZ * FFT * BUFS);
    st->phases = malloc(sizeof(float) * BLKSZ * FFT);
    st->ref_buf = malloc(BLKSZ);
    st->ready = 0;
    st->buf_idx = 0;
    st->idx = 0;
    st->used = 0;
    st->cfo_wait = 0;
    st->mer_cnt = 0;
    st->error_lb = 0;
    st->error_ub = 0;

#ifdef USE_THREADS
    pthread_cond_init(&st->cond, NULL);
    pthread_mutex_init(&st->mutex, NULL);
    pthread_create(&st->worker_thread, NULL, sync_worker, st);
#ifdef HAVE_PTHREAD_SETNAME_NP
    pthread_setname_np(st->worker_thread, "sync_worker");
#endif
#endif
}
