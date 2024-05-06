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
#include <string.h>

#include "defines.h"
#include "input.h"
#include "private.h"
#include "sync.h"

#define PM_PARTITIONS 10
#define MAX_PARTITIONS 14
#define PARTITION_DATA_CARRIERS 18
#define PARTITION_WIDTH 19
#define MIDDLE_REF_SC 30 // midpoint of Table 11-3 in 1011s.pdf

// Table 6-4 in 1011s.pdf
static const int compatibility_mode[64] = {
    0, 1, 2, 3, 1, 5, 6, 5, 6, 1, 2, 11, 1, 5, 6, 5,
    6, 1, 2, 3, 1, 5, 6, 5, 6, 1, 2, 11, 1, 5, 6, 5,
    6, 1, 2, 3, 1, 5, 6, 5, 6, 1, 2, 11, 1, 5, 6, 5,
    6, 1, 2, 3, 1, 5, 6, 5, 6, 1, 2, 11, 1, 5, 6, 5
};

static uint8_t gray4(float f)
{
    if (f < -1)
        return 0;
    else if (f < 0)
        return 2;
    else if (f < 1)
        return 3;
    else
        return 1;
}

static uint8_t gray8(float f)
{
    if (f < -3)
        return 0;
    else if (f < -2)
        return 4;
    else if (f < -1)
        return 6;
    else if (f < 0)
        return 2;
    else if (f < 1)
        return 3;
    else if (f < 2)
        return 7;
    else if (f < 3)
        return 5;
    else
        return 1;
}

static uint8_t qpsk(complex float cf)
{
    return (crealf(cf) < 0 ? 0 : 1) | (cimagf(cf) < 0 ? 0 : 2);
}

static uint8_t qam16(complex float cf)
{
    return gray4(crealf(cf)) | (gray4(cimagf(cf)) << 2);
}

static uint8_t qam64(complex float cf)
{
    return gray8(crealf(cf)) | (gray8(cimagf(cf)) << 3);
}

static void adjust_ref(sync_t *st, unsigned int ref, int cfo)
{
    unsigned int n;
    float cfo_freq = 2 * M_PI * cfo * CP_FM / FFT_FM;

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

static void reset_ref(sync_t *st, unsigned int ref)
{
    for (unsigned int n = 0; n < BLKSZ; n++)
        st->buffer[ref][n] *= cexpf(I * st->phases[ref][n]);
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

static int find_first_block(sync_t *st, unsigned int ref, unsigned int rsid)
{
    signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, rsid >> 1, rsid & 1, 0, (rsid >> 1) ^ (rsid & 1), 0, -1, 0, 0, 0, 0, -1, 1, 1, 1
    };
    unsigned char data[BLKSZ];
    int n;

    decode_dbpsk(st->buffer[ref], data, BLKSZ);
    n = fuzzy_match(needle, sizeof(needle), data, BLKSZ);
    if (n == 0)
        st->psmi = (data[25] << 5) | (data[26] << 4) | (data[27] << 3) | (data[28] << 2) | (data[29] << 1) | data[30];
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

static int find_block_am(sync_t *st, unsigned int ref)
{
    signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, -1, -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };
    unsigned char data[BLKSZ];
    int bc;

    for (int n = 0; n < BLKSZ; n++)
    {
        data[n] = cimagf(st->buffer[ref][n]) <= 0 ? 0 : 1;
        if ((needle[n] >= 0) && (data[n] != needle[n])) return -1;
    }

    // parity checks
    if (data[7] ^ data[8]) return -1;
    if (data[10] ^ data[11] ^ data[12] ^ data[13]) return -1;
    if (data[15] ^ data[16] ^ data[17] ^ data[18] ^ data[19] ^ data[20]) return -1;
    if (data[23] ^ data[24] ^ data[25] ^ data[26] ^ data[27] ^ data[28] ^ data[29] ^ data[30] ^ data[31]) return -1;

    bc = (data[17] << 2) | (data[18] << 1) | data[19];
    if (bc == 0)
        st->psmi = (data[26] << 4) | (data[27] << 3) | (data[28] << 2) | (data[29] << 1) | data[30];
    return bc;
}

static int find_ref_am(sync_t *st, unsigned int ref)
{
    signed char needle[] = {
        0, 1, 1, 0, 0, 1, 0, -1, -1, 1, -1, -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, 1, 1
    };
    unsigned char data[BLKSZ];

    for (int n = 0; n < BLKSZ; n++)
        data[n] = cimagf(st->buffer[ref][n]) <= 0 ? 0 : 1;

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

        for (int k = 1; k < PARTITION_WIDTH; k++)
        {
            // average phase difference
            float complex C = CMPLXF(PARTITION_WIDTH, PARTITION_WIDTH) / (k * smag19 * upper_phase + (PARTITION_WIDTH - k) * smag0 * lower_phase);
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

void detect_cfo(sync_t *st)
{
    for (int cfo = -2 * PARTITION_WIDTH; cfo < 2 * PARTITION_WIDTH; cfo++)
    {
        int offset;
        int best_offset = -1;
        unsigned int best_count = 0;
        unsigned int offset_count[BLKSZ];

        memset(offset_count, 0, BLKSZ * sizeof(unsigned int));

        for (int i = 0; i <= PM_PARTITIONS; i++)
        {
            adjust_ref(st, cfo + LB_START + i * PARTITION_WIDTH, cfo);
            offset = find_ref(st, cfo + LB_START + i * PARTITION_WIDTH, (MIDDLE_REF_SC-i) & 0x3);
            reset_ref(st, cfo + LB_START + i * PARTITION_WIDTH);
            if (offset >= 0)
                offset_count[offset]++;

            adjust_ref(st, cfo + UB_END - i * PARTITION_WIDTH, cfo);
            offset = find_ref(st, cfo + UB_END - i * PARTITION_WIDTH, (MIDDLE_REF_SC-i) & 0x3);
            reset_ref(st, cfo + UB_END - i * PARTITION_WIDTH);
            if (offset >= 0)
                offset_count[offset]++;
        }

        for (offset = 0; offset < BLKSZ; offset++)
        {
            if (offset_count[offset] > best_count) {
                best_offset = offset;
                best_count = offset_count[offset];
            }
        }

        if (best_offset >= 0 && best_count >= 3)
        {
            // At least three offsets matched, so this is likely the correct CFO.
            input_set_skip(st->input, best_offset * FFTCP_FM);
            acquire_cfo_adjust(&st->input->acq, cfo);

            log_debug("Block @ %d", best_offset);

            // Wait until the buffers have cleared before measuring again.
            st->cfo_wait = 8;
            break;
        }
    }
}

void sync_process_fm(sync_t *st)
{
    int i, partitions_per_band;

    switch (compatibility_mode[st->psmi]) {
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

    for (i = 0; i < partitions_per_band * PARTITION_WIDTH + 1; i += PARTITION_WIDTH)
    {
        adjust_ref(st, LB_START + i, 0);
        adjust_ref(st, UB_END - i, 0);
    }

    // check if we now have synchronization
    if (st->input->sync_state == SYNC_STATE_COARSE)
    {
        unsigned int good_refs = 0;
        for (i = 0; i <= partitions_per_band; i++)
        {
            if (find_first_block(st, LB_START + i * PARTITION_WIDTH, (MIDDLE_REF_SC-i) & 0x3) == 0)
                good_refs++;
            if (find_first_block(st, UB_END - i * PARTITION_WIDTH, (MIDDLE_REF_SC-i) & 0x3) == 0)
                good_refs++;
        }

        if (good_refs >= 4)
        {
            input_set_sync_state(st->input, SYNC_STATE_FINE);
            decode_reset(&st->input->decode);
            frame_reset(&st->input->frame);
        }
        else if (st->cfo_wait == 0)
        {
            detect_cfo(st);
        }
        else
        {
            // Decrease wait counter.
            st->cfo_wait--;
        }
    }

    // if we are still synchronized
    if (st->input->sync_state == SYNC_STATE_FINE)
    {
        float samperr = 0, angle = 0;
        float sum_xy = 0, sum_x2 = 0;
        for (i = 0; i < partitions_per_band * PARTITION_WIDTH; i += PARTITION_WIDTH)
        {
            adjust_data(st, LB_START + i, LB_START + i + PARTITION_WIDTH);
            adjust_data(st, UB_END - i - PARTITION_WIDTH, UB_END - i);

            samperr += phase_diff(st->phases[LB_START + i][0], st->phases[LB_START + i + PARTITION_WIDTH][0]);
            samperr += phase_diff(st->phases[UB_END - i - PARTITION_WIDTH][0], st->phases[UB_END - i][0]);
        }
        samperr = samperr / (partitions_per_band * 2) * FFT_FM / PARTITION_WIDTH / (2 * M_PI);

        for (i = 0; i < partitions_per_band * PARTITION_WIDTH + 1; i += PARTITION_WIDTH)
        {
            float x, y;

            x = LB_START + i - (FFT_FM / 2);
            y = st->costas_freq[LB_START + i];
            angle += y;
            sum_xy += x * y;
            sum_x2 += x * x;

            x = UB_END - i - (FFT_FM / 2);
            y = st->costas_freq[UB_END - i];
            angle += y;
            sum_xy += x * y;
            sum_x2 += x * x;
        }
        samperr -= (sum_xy / sum_x2) * FFT_FM / (2 * M_PI) * ACQUIRE_SYMBOLS;
        st->samperr = roundf(samperr);

        angle /= (partitions_per_band + 1) * 2;
        st->angle = angle;

        // Calculate modulation error
        float error_lb = 0, error_ub = 0;
        for (int n = 0; n < BLKSZ; n++)
        {
            float complex c, ideal;
            for (i = 0; i < partitions_per_band * PARTITION_WIDTH; i += PARTITION_WIDTH)
            {
                unsigned int j;
                for (j = 1; j < PARTITION_WIDTH; j++)
                {
                    c = st->buffer[LB_START + i + j][n];
                    ideal = CMPLXF(crealf(c) >= 0 ? 1 : -1, cimagf(c) >= 0 ? 1 : -1);
                    error_lb += normf(ideal - c);

                    c = st->buffer[UB_END - i - PARTITION_WIDTH + j][n];
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
            float signal = 2 * BLKSZ * (partitions_per_band * PARTITION_DATA_CARRIERS) * st->mer_cnt;
            float mer_db_lb = 10 * log10f(signal / st->error_lb);
            float mer_db_ub = 10 * log10f(signal / st->error_ub);

            nrsc5_report_mer(st->input->radio, mer_db_lb, mer_db_ub);

            st->mer_cnt = 0;
            st->error_lb = 0;
            st->error_ub = 0;
        }

        // Soft demod based on MER for each sideband
        float mer_lb = 2 * BLKSZ * (partitions_per_band * PARTITION_DATA_CARRIERS) / error_lb;
        float mer_ub = 2 * BLKSZ * (partitions_per_band * PARTITION_DATA_CARRIERS) / error_ub;
        float mult_lb = fmaxf(fminf(mer_lb * 10, 127), 1);
        float mult_ub = fmaxf(fminf(mer_ub * 10, 127), 1);

#define DEMOD(x) ((x) >= 0 ? 1 : -1)
        for (int n = 0; n < BLKSZ; n++)
        {
            float complex c;
            for (i = LB_START; i < LB_START + (PM_PARTITIONS * PARTITION_WIDTH); i += PARTITION_WIDTH)
            {
                unsigned int j;
                for (j = 1; j < PARTITION_WIDTH; j++)
                {
                    c = st->buffer[i + j][n];
                    decode_push_pm(&st->input->decode, DEMOD(crealf(c)) * mult_lb);
                    decode_push_pm(&st->input->decode, DEMOD(cimagf(c)) * mult_lb);
                }
            }
            for (i = UB_END - (PM_PARTITIONS * PARTITION_WIDTH); i < UB_END; i += PARTITION_WIDTH)
            {
                unsigned int j;
                for (j = 1; j < PARTITION_WIDTH; j++)
                {
                    c = st->buffer[i + j][n];
                    decode_push_pm(&st->input->decode, DEMOD(crealf(c)) * mult_ub);
                    decode_push_pm(&st->input->decode, DEMOD(cimagf(c)) * mult_ub);
                }
            }
            if (compatibility_mode[st->psmi] == 2) {
                unsigned int j;
                for (j = 1; j < PARTITION_WIDTH; j++)
                {
                    c = st->buffer[LB_START + (PM_PARTITIONS * PARTITION_WIDTH) + j][n];
                    decode_push_px1(&st->input->decode, DEMOD(crealf(c)) * mult_lb, P3_FRAME_LEN_FM / 2);
                    decode_push_px1(&st->input->decode, DEMOD(cimagf(c)) * mult_lb, P3_FRAME_LEN_FM / 2);
                }
                for (j = 1; j < PARTITION_WIDTH; j++)
                {
                    c = st->buffer[UB_END - (PM_PARTITIONS + 1) * PARTITION_WIDTH + j][n];
                    decode_push_px1(&st->input->decode, DEMOD(crealf(c)) * mult_ub, P3_FRAME_LEN_FM / 2);
                    decode_push_px1(&st->input->decode, DEMOD(cimagf(c)) * mult_ub, P3_FRAME_LEN_FM / 2);
                }
            }
            if ((compatibility_mode[st->psmi] == 3) || (compatibility_mode[st->psmi] == 11)) {
                for (i = LB_START + (PM_PARTITIONS * PARTITION_WIDTH); i < LB_START + (PM_PARTITIONS + 2) * PARTITION_WIDTH; i += PARTITION_WIDTH)
                {
                    unsigned int j;
                    for (j = 1; j < PARTITION_WIDTH; j++)
                    {
                        c = st->buffer[i + j][n];
                        decode_push_px1(&st->input->decode, DEMOD(crealf(c)) * mult_lb, P3_FRAME_LEN_FM);
                        decode_push_px1(&st->input->decode, DEMOD(cimagf(c)) * mult_lb, P3_FRAME_LEN_FM);
                    }
                }
                for (i = UB_END - (PM_PARTITIONS + 2) * PARTITION_WIDTH; i < UB_END - (PM_PARTITIONS * PARTITION_WIDTH); i += PARTITION_WIDTH)
                {
                    unsigned int j;
                    for (j = 1; j < PARTITION_WIDTH; j++)
                    {
                        c = st->buffer[i + j][n];
                        decode_push_px1(&st->input->decode, DEMOD(crealf(c)) * mult_ub, P3_FRAME_LEN_FM);
                        decode_push_px1(&st->input->decode, DEMOD(cimagf(c)) * mult_ub, P3_FRAME_LEN_FM);
                    }
                }
            }
            if (compatibility_mode[st->psmi] == 11) {
                for (i = LB_START + (PM_PARTITIONS + 2) * PARTITION_WIDTH; i < LB_START + (PM_PARTITIONS + 4) * PARTITION_WIDTH; i += PARTITION_WIDTH)
                {
                    unsigned int j;
                    for (j = 1; j < PARTITION_WIDTH; j++)
                    {
                        c = st->buffer[i + j][n];
                        decode_push_px2(&st->input->decode, DEMOD(crealf(c)) * mult_lb);
                        decode_push_px2(&st->input->decode, DEMOD(cimagf(c)) * mult_lb);
                    }
                }
                for (i = UB_END - (PM_PARTITIONS + 4) * PARTITION_WIDTH; i < UB_END - (PM_PARTITIONS + 2) * PARTITION_WIDTH; i += PARTITION_WIDTH)
                {
                    unsigned int j;
                    for (j = 1; j < PARTITION_WIDTH; j++)
                    {
                        c = st->buffer[i + j][n];
                        decode_push_px2(&st->input->decode, DEMOD(crealf(c)) * mult_ub);
                        decode_push_px2(&st->input->decode, DEMOD(cimagf(c)) * mult_ub);
                    }
                }
            }
        }
    }
}

void sync_process_am(sync_t *st)
{
    int offset;

    for (int i = REF_INDEX_AM; i <= MAX_INDEX_AM; i++)
    {
        for (int n = 0; n < BLKSZ; n++)
        {
            st->buffer[CENTER_AM - i][n] = -conjf(st->buffer[CENTER_AM - i][n]);
        }
    }

    if (st->psmi != SERVICE_MODE_MA3)
    {
        for (int i = REF_INDEX_AM; i <= PIDS_OUTER_INDEX_AM; i++)
        {
            for (int n = 0; n < BLKSZ; n++)
            {
                st->buffer[CENTER_AM + i][n] += st->buffer[CENTER_AM - i][n];
            }
        }
    }

    if (st->input->sync_state == SYNC_STATE_COARSE && st->cfo_wait == 0)
    {
        offset = find_ref_am(st, CENTER_AM + REF_INDEX_AM);
        if (offset > 0)
        {
            input_set_skip(st->input, offset * FFTCP_AM);
            log_debug("Block @ %d", offset);
            st->cfo_wait = 8;
        }
    }
    else
    {
        st->cfo_wait--;
    }

    if (st->input->sync_state == SYNC_STATE_COARSE)
    {
        int bc = find_block_am(st, CENTER_AM + REF_INDEX_AM);

        if (bc == -1)
            st->offset_history = 0;
        else
            st->offset_history = (st->offset_history << 4) | bc;

        if ((st->offset_history & 0xffff) == 0x5670)
        {
            log_debug("Sync!");
            st->input->sync_state = SYNC_STATE_FINE;
            decode_reset(&st->input->decode);
            frame_reset(&st->input->frame);
            st->offset_history = 0;
        }
    }

    if (st->input->sync_state == SYNC_STATE_FINE)
    {
        int pids_0_index = (st->psmi != SERVICE_MODE_MA3) ? PIDS_INNER_INDEX_AM : -PIDS_INNER_INDEX_AM;
        int pids_1_index = (st->psmi != SERVICE_MODE_MA3) ? PIDS_OUTER_INDEX_AM : PIDS_INNER_INDEX_AM;

        float complex pids1_mult = 2 * CMPLXF(1.5, -0.5) / (st->buffer[CENTER_AM + pids_0_index][8] + st->buffer[CENTER_AM + pids_0_index][24]);
        float complex pids2_mult = 2 * CMPLXF(1.5, -0.5) / (st->buffer[CENTER_AM + pids_1_index][8] + st->buffer[CENTER_AM + pids_1_index][24]);

        for (int n = 0; n < BLKSZ; n++)
        {
            st->buffer[CENTER_AM + pids_0_index][n] *= pids1_mult;
            decode_push_pids(&st->input->decode, qam16(st->buffer[CENTER_AM + pids_0_index][n]));

            st->buffer[CENTER_AM + pids_1_index][n] *= pids2_mult;
            decode_push_pids(&st->input->decode, qam16(st->buffer[CENTER_AM + pids_1_index][n]));
        }

        float complex pl_mult[PARTITION_WIDTH_AM];
        float complex pu_mult[PARTITION_WIDTH_AM];
        float complex s_mult[PARTITION_WIDTH_AM];
        float complex t_mult[PARTITION_WIDTH_AM];

        int primary_index = (st->psmi != SERVICE_MODE_MA3) ? OUTER_PARTITION_START_AM : INNER_PARTITION_START_AM;
        int secondary_index = MIDDLE_PARTITION_START_AM;
        int tertiary_index = (st->psmi != SERVICE_MODE_MA3) ? INNER_PARTITION_START_AM : MIDDLE_PARTITION_START_AM;

        float samperr = 0;
        for (int col = 0; col < PARTITION_WIDTH_AM; col++)
        {
            int train1 = (5 + 11*col) % 32;
            int train2 = (21 + 11*col) % 32;

            pl_mult[col] = 2 * CMPLXF(2.5, -2.5) / (st->buffer[CENTER_AM - primary_index - col][train1] + st->buffer[CENTER_AM - primary_index - col][train2]);
            pu_mult[col] = 2 * CMPLXF(2.5, -2.5) / (st->buffer[CENTER_AM + primary_index + col][train1] + st->buffer[CENTER_AM + primary_index + col][train2]);
            if (st->psmi != SERVICE_MODE_MA3)
            {
                s_mult[col] = 2 * CMPLXF(1.5, -0.5) / (st->buffer[CENTER_AM + secondary_index + col][train1] + st->buffer[CENTER_AM + secondary_index + col][train2]);
                t_mult[col] = 2 * CMPLXF(-0.5, 0.5) / (st->buffer[CENTER_AM + tertiary_index + col][train1] + st->buffer[CENTER_AM + tertiary_index + col][train2]);
            }
            else
            {
                s_mult[col] = 2 * CMPLXF(2.5, -2.5) / (st->buffer[CENTER_AM + secondary_index + col][train1] + st->buffer[CENTER_AM + secondary_index + col][train2]);
                t_mult[col] = 2 * CMPLXF(2.5, -2.5) / (st->buffer[CENTER_AM - tertiary_index - col][train1] + st->buffer[CENTER_AM - tertiary_index - col][train2]);
            }

            if (col > 0)
            {
                samperr += phase_diff(cargf(pl_mult[col]), cargf(pl_mult[col-1]));
                samperr += phase_diff(cargf(pu_mult[col]), cargf(pu_mult[col-1]));
            }
        }
        samperr = samperr / (2 * (PARTITION_WIDTH_AM-1)) * FFT_AM / (2 * M_PI);
        st->samperr = roundf(samperr);

        for (int n = 0; n < BLKSZ; n++)
        {
            for (int col = 0; col < PARTITION_WIDTH_AM; col++)
            {
                st->buffer[CENTER_AM - primary_index - col][n] *= pl_mult[col];
                st->buffer[CENTER_AM + primary_index + col][n] *= pu_mult[col];
                st->buffer[CENTER_AM + secondary_index + col][n] *= s_mult[col];
                if (st->psmi != SERVICE_MODE_MA3)
                    st->buffer[CENTER_AM + tertiary_index + col][n] *= t_mult[col];
                else
                    st->buffer[CENTER_AM - tertiary_index - col][n] *= t_mult[col];

                if (st->psmi != SERVICE_MODE_MA3)
                {
                    decode_push_pl_pu_s_t(
                        &st->input->decode,
                        qam64(st->buffer[CENTER_AM - primary_index - col][n]),
                        qam64(st->buffer[CENTER_AM + primary_index + col][n]),
                        qam16(st->buffer[CENTER_AM + secondary_index + col][n]),
                        qpsk(st->buffer[CENTER_AM + tertiary_index + col][n])
                    );
                }
                else
                {
                    decode_push_pl_pu_s_t(
                        &st->input->decode,
                        qam64(st->buffer[CENTER_AM - primary_index - col][n]),
                        qam64(st->buffer[CENTER_AM + primary_index + col][n]),
                        qam64(st->buffer[CENTER_AM + secondary_index + col][n]),
                        qam64(st->buffer[CENTER_AM - tertiary_index - col][n])
                    );
                }
            }
        }
    }
}

void sync_adjust(sync_t *st, int sample_adj)
{
    int i;
    for (i = 0; i < MAX_PARTITIONS * PARTITION_WIDTH + 1; i++)
    {
        st->costas_phase[LB_START + i] -= sample_adj * (LB_START + i - (FFT_FM / 2)) * 2 * M_PI / FFT_FM;
        st->costas_phase[UB_END - i] -= sample_adj * (UB_END - i - (FFT_FM / 2)) * 2 * M_PI / FFT_FM;
    }
}

void sync_push(sync_t *st, float complex *fftout)
{
    unsigned int i;

    if (st->input->radio->mode == NRSC5_MODE_FM)
    {
        for (i = 0; i < MAX_PARTITIONS * PARTITION_WIDTH + 1; i++)
        {
            st->buffer[LB_START + i][st->idx] = fftout[LB_START + i];
            st->buffer[UB_END - i][st->idx] = fftout[UB_END - i];
        }
    }
    else
    {
        for (i = CENTER_AM - MAX_INDEX_AM; i <= CENTER_AM + MAX_INDEX_AM; i++)
        {
            st->buffer[i][st->idx] = fftout[i];
        }
    }

    if (++st->idx == BLKSZ)
    {
        st->idx = 0;

        if (st->input->radio->mode == NRSC5_MODE_FM)
            sync_process_fm(st);
        else
            sync_process_am(st);
    }
}

void sync_reset(sync_t *st)
{
    unsigned int i;
    for (i = 0; i < FFT_FM; i++)
    {
        st->costas_freq[i] = 0;
        st->costas_phase[i] = 0;
    }

    st->idx = 0;
    st->psmi = 1;
    st->cfo_wait = 0;
    st->offset_history = 0;
    st->mer_cnt = 0;
    st->error_lb = 0;
    st->error_ub = 0;
}

void sync_init(sync_t *st, input_t *input)
{
    float loop_bw = 0.05, damping = 0.70710678;
    float denom = 1 + (2 * damping * loop_bw) + (loop_bw * loop_bw);
    st->alpha = (4 * damping * loop_bw) / denom;
    st->beta = (4 * loop_bw * loop_bw) / denom;

    st->input = input;
    sync_reset(st);
}
