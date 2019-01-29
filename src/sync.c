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

#include <math.h>

#include "defines.h"
#include "input.h"
#include "private.h"
#include "sync.h"

static void adjust_ref(sync_t *st, unsigned int ref, int cfo)
{
    unsigned int n;
    float cfo_freq = 2 * M_PI * cfo * CP / FFTCP;

    // sync bits (after DBPSK)
    static const signed char sync[] = {
        -1, 1, -1, -1, -1, 1, 1
    };

    for (n = 0; n < BLKSZ; n++)
    {
        float error = cargf(st->buffer[ref][n] * st->buffer[ref][n] * cexpf(-I * 2 * st->costas_phase[ref])) * 0.5;

        st->phases[ref][n] = st->costas_phase[ref];
        st->buffer[ref][n] *= cexpf(-I * st->costas_phase[ref]);

        st->costas_freq[ref] += st->beta * error;
        if (st->costas_freq[ref] > 0.5) st->costas_freq[ref] = 0.5;
        if (st->costas_freq[ref] < -0.5) st->costas_freq[ref] = -0.5;
        st->costas_phase[ref] += st->costas_freq[ref] + cfo_freq + (st->alpha * error);
        if (st->costas_phase[ref] > M_PI) st->costas_phase[ref] -= 2 * M_PI;
        if (st->costas_phase[ref] < -M_PI) st->costas_phase[ref] += 2 * M_PI;
    }

    // compare to sync bits
    float x = 0;
    for (n = 0; n < sizeof(sync); n++)
        x += crealf(st->buffer[ref][n]) * sync[n];
    if (x < 0)
    {
        // adjust phase by pi to compensate
        for (n = 0; n < BLKSZ; n++)
        {
            st->phases[ref][n] += M_PI;
            st->buffer[ref][n] *= -1;
        }
        st->costas_phase[ref] += M_PI;
    }
}

static void decode_dbpsk(const float complex *buf, unsigned char *data, int size)
{
    unsigned char prev = 0;

    for (int n = 0; n < size; n++)
    {
        unsigned char bit = crealf(buf[n]) <= 0 ? 0 : 1;
        data[n] = bit ^ prev;
        prev = bit;
    }
}

static int fuzzy_match(const signed char *needle, unsigned int needle_size, const unsigned char *data, int size)
{
    for (int n = 0; n < size; n++)
    {
        unsigned int i;
        for (i = 0; i < needle_size; i++)
        {
            // first bit of data may be wrong, so ignore
            if ((n + i) % size == 0) continue;
            // ignore don't care bits
            if (needle[i] < 0) continue;
            // test if bit is correct
            if (needle[i] != data[(n + i) % size])
                break;
        }
        if (i == needle_size)
            return n;
    }
    return -1;
}

static int find_first_block(sync_t *st, unsigned int ref, int *psmi)
{
    static const signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, 1, 0, 0, 1, 0, -1, 0, 0, 0, 0, -1, 1, 1, 1
    };
    unsigned char data[BLKSZ];
    int n;

    *psmi = -1;
    decode_dbpsk(st->buffer[ref], data, BLKSZ);
    n = fuzzy_match(needle, sizeof(needle), data, BLKSZ);
    if (n == 0)
        *psmi = (data[25] << 5) | (data[26] << 4) | (data[27] << 3) | (data[28] << 2) | (data[29] << 1) | data[30];
    return n;
}

static int find_ref(sync_t *st, unsigned int ref, unsigned int rsid)
{
    signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, rsid >> 1, rsid & 1, 0, (rsid >> 1) ^ (rsid & 1), 0, -1, -1, -1, -1, -1, -1, 1, 1, 1
    };
    unsigned char data[BLKSZ];

    decode_dbpsk(st->buffer[ref], data, BLKSZ);
    return fuzzy_match(needle, sizeof(needle), data, BLKSZ);
}

static float calc_smag(sync_t *st, unsigned int ref)
{
    float sum = 0;
    // phase was already corrected, so imaginary component is zero
    for (int n = 0; n < BLKSZ; n++)
        sum += fabsf(crealf(st->buffer[ref][n]));
    return sum / BLKSZ;
}

static void adjust_data(sync_t *st, unsigned int lower, unsigned int upper)
{
    float smag0, smag19;
    smag0 = calc_smag(st, lower);
    smag19 = calc_smag(st, upper);

    for (int n = 0; n < BLKSZ; n++)
    {
        float complex upper_phase = cexpf(st->phases[upper][n] * I);
        float complex lower_phase = cexpf(st->phases[lower][n] * I);

        for (int k = 1; k < 19; k++)
        {
            // average phase difference
            float complex C = CMPLXF(19,19) / (k * smag19 * upper_phase + (19 - k) * smag0 * lower_phase);
            // adjust sample
            st->buffer[lower + k][n] *= C;
        }
    }
}

float phase_diff(float a, float b)
{
    float diff = a - b;
    while (diff > M_PI / 2) diff -= M_PI;
    while (diff < -M_PI / 2) diff += M_PI;
    return diff;
}

void sync_process(sync_t *st)
{
    int i, partitions_per_band;
    static int psmi = 1;

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
        adjust_ref(st, LB_START + i, 0);
        adjust_ref(st, UB_END - i, 0);
    }

    // check if we lost synchronization or now have it
    if (st->ready)
    {
        if (decode_get_block(&st->input->decode) == 0 && find_first_block(st, LB_START, &psmi) != 0)
        {
            if (find_first_block(st, UB_END, &psmi) != 0)
            {
                log_debug("lost sync (%d, %d)!", find_first_block(st, LB_START, &psmi), find_first_block(st, UB_END, &psmi));
                nrsc5_report_lost_sync(st->input->radio);
                st->ready = 0;
            }
        }
    }
    else
    {
        // First and last reference subcarriers have the same data. Try both
        // in case one of the sidebands is too corrupted.
        int offset = find_first_block(st, LB_START, &psmi);
        if (offset < 0)
            offset = find_first_block(st, UB_END, &psmi);

        if (offset > 0)
        {
            log_debug("First block @ %d", offset);
            input_set_skip(st->input, offset * FFTCP);
        }
        else if (offset == 0)
        {
            log_info("Synchronized!");
            nrsc5_report_sync(st->input->radio);
            decode_reset(&st->input->decode);
            st->ready = 1;
        }
        else if (st->cfo_wait == 0)
        {
            for (i = -38; i < 38; ++i)
            {
                int offset2;
                adjust_ref(st, LB_START + (PM_PARTITIONS * 19) + i, i);
                offset = find_ref(st, LB_START + (PM_PARTITIONS * 19) + i, 0);
                if (offset < 0)
                    continue;
                // We think we found the start. Check upperband to confirm.
                adjust_ref(st, UB_END - (PM_PARTITIONS * 19) + i, i);
                offset2 = find_ref(st, UB_END - (PM_PARTITIONS * 19) + i, 0);
                if (offset2 == offset)
                {
                    // The offsets matched, so 'i' is likely the CFO.
                    input_set_skip(st->input, offset * FFTCP);
                    acquire_cfo_adjust(&st->input->acq, i);

                    log_debug("Block @ %d", offset);

                    // Wait until the buffers have cleared before measuring again.
                    st->cfo_wait = 8;
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
        float samperr = 0, angle = 0;
        float sum_xy = 0, sum_x2 = 0;
        for (i = 0; i < partitions_per_band * 19; i += 19)
        {
            adjust_data(st, LB_START + i, LB_START + i + 19);
            adjust_data(st, UB_END - i - 19, UB_END - i);

            samperr += phase_diff(st->phases[LB_START + i][0], st->phases[LB_START + i + 19][0]);
            samperr += phase_diff(st->phases[UB_END - i - 19][0], st->phases[UB_END - i][0]);
        }
        samperr = samperr / (partitions_per_band * 2) * 2048 / 19 / (2 * M_PI);

        for (i = 0; i < partitions_per_band * 19 + 1; i += 19)
        {
            float x, y;

            x = LB_START + i - 1024;
            y = st->costas_freq[LB_START + i];
            angle += y;
            sum_xy += x * y;
            sum_x2 += x * x;

            x = UB_END - i - 1024;
            y = st->costas_freq[UB_END - i];
            angle += y;
            sum_xy += x * y;
            sum_x2 += x * x;
        }
        samperr -= (sum_xy / sum_x2) * 2048 / (2 * M_PI) * ACQUIRE_SYMBOLS;
        st->samperr = roundf(samperr);

        angle /= (partitions_per_band + 1) * 2;
        st->angle = angle;

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
                    c = st->buffer[LB_START + i + j][n];
                    ideal = CMPLXF(crealf(c) >= 0 ? 1 : -1, cimagf(c) >= 0 ? 1 : -1);
                    error_lb += normf(ideal - c);

                    c = st->buffer[UB_END - i - 19 + j][n];
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
            float signal = 2 * BLKSZ * (partitions_per_band * 18) * st->mer_cnt;
            float mer_db_lb = 10 * log10f(signal / st->error_lb);
            float mer_db_ub = 10 * log10f(signal / st->error_ub);

            nrsc5_report_mer(st->input->radio, mer_db_lb, mer_db_ub);

            st->mer_cnt = 0;
            st->error_lb = 0;
            st->error_ub = 0;
        }

        // Soft demod based on MER for each sideband
        float mer_lb = 2 * BLKSZ * (partitions_per_band * 18) / error_lb;
        float mer_ub = 2 * BLKSZ * (partitions_per_band * 18) / error_ub;
        float mult_lb = fmaxf(fminf(mer_lb * 10, 127), 1);
        float mult_ub = fmaxf(fminf(mer_ub * 10, 127), 1);

#define DEMOD(x) ((x) >= 0 ? 1 : -1)
        for (int n = 0; n < BLKSZ; n++)
        {
            float complex c;
            for (i = LB_START; i < LB_START + (PM_PARTITIONS * 19); i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = st->buffer[i + j][n];
                    decode_push_pm(&st->input->decode, DEMOD(crealf(c)) * mult_lb);
                    decode_push_pm(&st->input->decode, DEMOD(cimagf(c)) * mult_lb);
                }
            }
            for (i = UB_END - (PM_PARTITIONS * 19); i < UB_END; i += 19)
            {
                unsigned int j;
                for (j = 1; j < 19; j++)
                {
                    c = st->buffer[i + j][n];
                    decode_push_pm(&st->input->decode, DEMOD(crealf(c)) * mult_ub);
                    decode_push_pm(&st->input->decode, DEMOD(cimagf(c)) * mult_ub);
                }
            }
            if (psmi == 3) {
                for (i = LB_START + (PM_PARTITIONS * 19); i < LB_START + (PM_PARTITIONS * 19) + 38; i += 19)
                {
                    unsigned int j;
                    for (j = 1; j < 19; j++)
                    {
                        c = st->buffer[i + j][n];
                        decode_push_px1(&st->input->decode, DEMOD(crealf(c)) * mult_lb);
                        decode_push_px1(&st->input->decode, DEMOD(cimagf(c)) * mult_lb);
                    }
                }
                for (i = UB_END - (PM_PARTITIONS * 19) - 38; i < UB_END - (PM_PARTITIONS * 19); i += 19)
                {
                    unsigned int j;
                    for (j = 1; j < 19; j++)
                    {
                        c = st->buffer[i + j][n];
                        decode_push_px1(&st->input->decode, DEMOD(crealf(c)) * mult_ub);
                        decode_push_px1(&st->input->decode, DEMOD(cimagf(c)) * mult_ub);
                    }
                }
            }
        }
    }
}

void sync_adjust(sync_t *st, int sample_adj)
{
    int i;
    for (i = 0; i < FFT; i++)
        st->costas_phase[i] -= sample_adj * (i - 1024) * 2 * M_PI / FFT;
}

void sync_push(sync_t *st, float complex *fftout)
{
    unsigned int i;
    for (i = 0; i < FFT; ++i)
        st->buffer[i][st->idx] = fftout[i];

    if (++st->idx == BLKSZ)
    {
        st->idx = 0;

        sync_process(st);
    }
}

void sync_init(sync_t *st, input_t *input)
{
    unsigned int i;
    float loop_bw = 0.05, damping = 0.70710678;
    float denom = 1 + (2 * damping * loop_bw) + (loop_bw * loop_bw);
    st->alpha = (4 * damping * loop_bw) / denom;
    st->beta = (4 * loop_bw * loop_bw) / denom;
    for (i = 0; i < FFT; i++)
    {
        st->costas_freq[i] = 0;
        st->costas_phase[i] = 0;
    }

    st->input = input;
    st->ready = 0;
    st->idx = 0;
    st->cfo_wait = 0;
    st->mer_cnt = 0;
    st->error_lb = 0;
    st->error_ub = 0;
}
