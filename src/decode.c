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

#include <string.h>

#include "conv.h"
#include "decode.h"
#include "input.h"
#include "pids.h"
#include "private.h"

#define PM_V_SIZE 20

/* 1012s.pdf figure 10-4 */
static const int bl_delay[] = { 2, 1, 5 };
static const int ml_delay[] = { 11, 6, 7 };
static const int bu_delay[] = { 10, 8, 9 };
static const int mu_delay[] = { 4, 3, 0 };
static const int el_delay[] = { 0, 1 };
static const int eu_delay[] = { 2, 3, 5, 4 };

static const int8_t PM_V[PM_V_SIZE] = {
    10, 2, 18, 6, 14, 8, 16, 0, 12, 4,
    11, 3, 19, 7, 15, 9, 17, 1, 13, 5
};

static const struct lte_conv_code conv_code_k7 = {
    .n = 3,
    .k = 7,
    .len = P1_FRAME_LEN_FM,
    .gen = { 0133, 0171, 0165 },
    .term = CONV_TERM_TAIL_BITING,
};

static const struct lte_conv_code conv_code_e1 = {
    .n = 3,
    .k = 9,
    .len = P3_FRAME_LEN_MA3,
    .gen = { 0561, 0657, 0711 },
    .term = CONV_TERM_TAIL_BITING,
};

static const struct lte_conv_code conv_code_e2_e3 = {
    .n = 3,
    .k = 9,
    .len = P3_FRAME_LEN_MA1,
    .gen = { 0561, 0753, 0711 },
    .term = CONV_TERM_TAIL_BITING,
};

/* 1012s.pdf figure 10-5 */
static const int pids_il_delay[] = { 0, 1, 12, 13, 6, 5, 18, 17, 11, 7, 23, 19 };
static const int pids_iu_delay[] = { 2, 4, 14, 16, 3, 8, 15, 20, 9, 10, 21, 22 };

static int bit_map(const unsigned char matrix[PARTITION_WIDTH_AM * BLKSZ * 8], const int b, const int k, const int p)
{
    const int col = (9*k) % 25;
    const int row = (11*col + 16*(k/25) + 11*(k/50)) % 32;
    return (matrix[PARTITION_WIDTH_AM * (b*BLKSZ + row) + col] >> p) & 1;
}

static void interleaver_ma1(decode_t *st)
{
    int b, k, p;
    for (int n = 0; n < 18000; n++)
    {
        b = n/2250;
        k = (n + n/750 + 1) % 750;
        p = n % 3;
        st->bl[n] = bit_map(st->buffer_pl, b, k, p);

        b = (3*n + 3) % 8;
        k = (n + n/3000 + 3) % 750;
        p = 3 + (n % 3);
        st->ml[DIVERSITY_DELAY_AM + n] = bit_map(st->buffer_pl, b, k, p);

        b = n/2250;
        k = (n + n/750) % 750;
        p = n % 3;
        st->bu[n] = bit_map(st->buffer_pu, b, k, p);

        b = (3*n) % 8;
        k = (n + n/3000 + 2) % 750;
        p = 3 + (n % 3);
        st->mu[DIVERSITY_DELAY_AM + n] = bit_map(st->buffer_pu, b, k, p);
    }

    if (st->input->sync.psmi != SERVICE_MODE_MA3)
    {
        for (int n = 0; n < 12000; n++)
        {
            b = (3*n + n/3000) % 8;
            k = (n + (n/6000)) % 750;
            p = n % 2;
            st->el[n] = bit_map(st->buffer_t, b, k, p);
        }
        for (int n = 0; n < 24000; n++)
        {
            b = (3*n + n/3000 + 2*(n/12000)) % 8;
            k = (n + (n/6000)) % 750;
            p = n % 4;
            st->eu[n] = bit_map(st->buffer_s, b, k, p);
        }
    }
    else
    {
        for (int n = 0; n < 18000; n++)
        {
            b = (3*n + 3) % 8;
            k = (n + n/3000 + 3) % 750;
            p = n % 3;
            st->ebl[n] = bit_map(st->buffer_t, b, k, p);

            b = (3*n + 3) % 8;
            k = (n + n/3000 + 3) % 750;
            p = 3 + (n % 3);
            st->eml[DIVERSITY_DELAY_AM + n] = bit_map(st->buffer_t, b, k, p);

            b = (3*n) % 8;
            k = (n + n/3000 + 2) % 750;
            p = n % 3;
            st->ebu[n] = bit_map(st->buffer_s, b, k, p);

            b = (3*n) % 8;
            k = (n + n/3000 + 2) % 750;
            p = 3 + (n % 3);
            st->emu[DIVERSITY_DELAY_AM + n] = bit_map(st->buffer_s, b, k, p);
        }
    }

    for (int i = 0; i < 6000; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            st->p1_am[i*12 + bl_delay[j]] = st->bl[i*3 + j];
            st->p1_am[i*12 + ml_delay[j]] = st->ml[i*3 + j];
            st->p1_am[i*12 + bu_delay[j]] = st->bu[i*3 + j];
            st->p1_am[i*12 + mu_delay[j]] = st->mu[i*3 + j];
        }
        if (st->input->sync.psmi != SERVICE_MODE_MA3)
        {
            for (int j = 0; j < 2; j++)
            {
                st->p3_am[i*6 + el_delay[j]] = st->el[i*2 + j];
            }
            for (int j = 0; j < 4; j++)
            {
                st->p3_am[i*6 + eu_delay[j]] = st->eu[i*4 + j];
            }
        }
        else
        {
            for (int j = 0; j < 3; j++)
            {
                st->p3_am[i*12 + bl_delay[j]] = st->ebl[i*3 + j];
                st->p3_am[i*12 + ml_delay[j]] = st->eml[i*3 + j];
                st->p3_am[i*12 + bu_delay[j]] = st->ebu[i*3 + j];
                st->p3_am[i*12 + mu_delay[j]] = st->emu[i*3 + j];
            }
        }
    }

    memmove(st->ml, st->ml + 18000, DIVERSITY_DELAY_AM);
    memmove(st->mu, st->mu + 18000, DIVERSITY_DELAY_AM);
    if (st->input->sync.psmi == SERVICE_MODE_MA3)
    {
        memmove(st->eml, st->eml + 18000, DIVERSITY_DELAY_AM);
        memmove(st->emu, st->emu + 18000, DIVERSITY_DELAY_AM);
    }

    int offset = 0;
    for (int i = 0; i < 8 * P1_FRAME_LEN_AM * 3; i++)
    {
        switch (i % 15)
        {
        case 1:
        case 4:
        case 7:
            st->viterbi_p1_am[i] = 0;
            break;
        default:
            st->viterbi_p1_am[i] = st->p1_am[offset++] ? 1 : -1;
        }
    }

    offset = 0;
    if (st->input->sync.psmi != SERVICE_MODE_MA3)
    {
        for (int i = 0; i < P3_FRAME_LEN_MA1 * 3; i++)
        {
            switch (i % 6)
            {
            case 1:
            case 4:
            case 5:
                st->viterbi_p3_am[i] = 0;
                break;
            default:
                st->viterbi_p3_am[i] = st->p3_am[offset++] ? 1 : -1;
            }
        }
    }
    else
    {
        for (int i = 0; i < P3_FRAME_LEN_MA3 * 3; i++)
        {
            switch (i % 15)
            {
            case 1:
            case 4:
            case 7:
                st->viterbi_p3_am[i] = 0;
                break;
            default:
                st->viterbi_p3_am[i] = st->p3_am[offset++] ? 1 : -1;
            }
        }
    }
}

// calculate number of bit errors by re-encoding and comparing to the input
static int bit_errors(int8_t *coded, uint8_t *decoded, const unsigned int k, unsigned int frame_len,
                      const unsigned int gens[3],
                      const uint8_t *puncture, const int puncture_len)
{
    uint16_t r = 0;
    unsigned int i, j, errors = 0;

    // tail biting
    for (i = 0; i < (k-1); i++)
        r = (r >> 1) | (decoded[frame_len - (k-1) + i] << (k-1));

    for (i = 0, j = 0; i < frame_len; i++, j += 3)
    {
        // shift in new bit
        r = (r >> 1) | (decoded[i] << (k-1));

        if (puncture[j % puncture_len] && ((coded[j] > 0) != __builtin_parity(r & gens[0])))
            errors++;
        if (puncture[(j+1) % puncture_len] && ((coded[j+1] > 0) != __builtin_parity(r & gens[1])))
            errors++;
        if (puncture[(j+2) % puncture_len] && ((coded[j+2] > 0) != __builtin_parity(r & gens[2])))
            errors++;
    }

    return errors;
}

static int bit_errors_2_5_fm(int8_t *coded, uint8_t *decoded, const int len)
{
    const uint8_t puncture[] = {1, 1, 1, 1, 1, 0};
    return bit_errors(coded, decoded, conv_code_k7.k, len, conv_code_k7.gen, puncture, 6);
}

static int bit_errors_e1(int8_t *coded, uint8_t *decoded, const int len)
{
    const uint8_t puncture[] = {1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1};
    return bit_errors(coded, decoded, conv_code_e1.k, len, conv_code_e1.gen, puncture, 15);
}

static int bit_errors_e2(int8_t *coded, uint8_t *decoded, const int len)
{
    const uint8_t puncture[] = {1, 0, 1, 1, 0, 0};
    return bit_errors(coded, decoded, conv_code_e2_e3.k, len, conv_code_e2_e3.gen, puncture, 6);
}

static void descramble(uint8_t *buf, unsigned int length)
{
    const unsigned int width = 11;
    unsigned int i, val = 0x3ff;
    for (i = 0; i < length; i += 8)
    {
        unsigned int j;
        for (j = 0; j < 8; ++j)
        {
            int bit = ((val >> 9) ^ val) & 1;
            val |= bit << width;
            val >>= 1;
            buf[i + j] ^= bit;
        }
    }
}

static void interleaver_i(const int8_t* in,
                          int8_t* viterbi,
                          const int J,
                          const int B,
                          const int C,
                          const int M,
                          const int8_t* V,
                          const unsigned int length_v,
                          const unsigned int N)
{
    unsigned int out = 0;
    for (unsigned int i = 0; i < N; i++)
    {
        const int8_t partition = V[((i + 2 * (M / 4)) / M) % length_v];
        unsigned int block;
        if (M == 1)
            block = ((i / J) + (partition * 7)) % B;
        else
            block = (i + (i / (J * B))) % B;
        const unsigned int k = i / (J * B);
        const unsigned int row = (k * 11) % 32;
        const unsigned int column = (k * 11 + k / (32 * 9)) % C;
        viterbi[out++] = in[(block * 32 + row) * (J * C) + partition * C + column];
        if ((out % 6) == 5) // depuncture, [1, 1, 1, 1, 1, 0]
            viterbi[out++] = 0;
    }
}

static void interleaver_ii(const int8_t* in, int8_t* viterbi, const unsigned int bc,
                           const int J, const int B, const int C,
                           const int8_t* V, const unsigned int length_v,
                           const int b, const int I0)
{
    int out = 0;

    for (unsigned int i = bc * b; i < (bc+1) * b; i++)
    {
        const int8_t partition = V[i % length_v];
        const unsigned int block = i / b;
        const unsigned int k = ((i / J) % (b / J)) + (I0 / (J * B));
        const unsigned int row = (k * 11) % 32;
        const unsigned int column = (k * 11 + k / (32 * 9)) % C;
        viterbi[out++] = in[(block * 32 + row) * (J * C) + partition * C + column];
        if ((out % 6) == 5) // depuncture, [1, 1, 1, 1, 1, 0]
            viterbi[out++] = 0;
    };
}

void interleaver_iv(interleaver_iv_t* interleaver, int8_t* viterbi, const unsigned int frame_len)
{
    const unsigned int J = (frame_len == P3_FRAME_LEN_MP3_MP11) ? 4 : 2;
    const unsigned int B = 32;
    const unsigned int C = 36;
    const unsigned int M = (frame_len == P3_FRAME_LEN_MP3_MP11) ? 2 : 4;
    const unsigned int N = (frame_len == P3_FRAME_LEN_MP3_MP11) ? 147456 : 73728;
    const unsigned int bk_bits = 32 * C;
    const unsigned int bk_adj = 32 * C - 1;

    if (interleaver->i == N)
    {
        interleaver->i = 0;
        memset(interleaver->pt, 0, sizeof(unsigned int) * 4);
        interleaver->ready = 1;
    }

    unsigned int out = 0;
    for (unsigned int i = 0; i < frame_len * 2; i++)
    {
        const unsigned int partition = ((interleaver->i + 2 * (M / 4)) / M) % J;
        const unsigned int pti = interleaver->pt[partition]++;
        const unsigned int block = (pti + (partition * 7) - (bk_adj * (pti / bk_bits))) % B;
        const unsigned int row = ((11 * pti) % bk_bits) / C;
        const unsigned int column = (pti * 11) % C;
        viterbi[out++] = interleaver->internal[(block * 32 + row) * (J * C) + partition * C + column];
        if ((out % 6) == 1 || (out % 6) == 4) // depuncture, [1, 0, 1, 1, 0, 1]
            viterbi[out++] = 0;

        interleaver->internal[interleaver->i] = interleaver->buffer[i];
        interleaver->i++;
    }
}

void decode_push_pm(decode_t *st, const int8_t* sbit, const unsigned int bc)
{
    memcpy(st->buffer_pm + PM_BLOCK_SIZE * bc, sbit, PM_BLOCK_SIZE * sizeof(int8_t));
    decode_process_pids(st, bc);

    if (bc == 0)
        st->started_pm = 1;

    if (st->started_pm)
    {
        if (bc == 15)
            decode_process_p1(st);
    }
}

void decode_push_px1(decode_t *st, const int8_t* sbit, const unsigned int len, const unsigned int bc)
{
    if (bc % 2 == 0)
        st->interleaver_px1.started = 1;

    if (st->interleaver_px1.started)
    {
        memcpy(st->interleaver_px1.buffer + len * (bc % 2), sbit,len * sizeof(int8_t));

        if (bc % 2 == 1)
        {
            interleaver_iv(&st->interleaver_px1, st->viterbi_p3, len);

            if (st->interleaver_px1.ready)
            {
                nrsc5_conv_decode_p3_p4(st->viterbi_p3, st->scrambler_p3, len);
                descramble(st->scrambler_p3, len);
                frame_push(&st->input->frame, st->scrambler_p3, len, P3_LOGICAL_CHANNEL);
            }
        }
    }
}

void decode_push_px2(decode_t* st, const int8_t* sbit, const unsigned int len, const unsigned int bc)
{
    if (bc % 2 == 0)
        st->interleaver_px2.started = 1;

    if (st->interleaver_px2.started)
    {
        memcpy(st->interleaver_px2.buffer + len * (bc % 2), sbit, len * sizeof(int8_t));

        if (bc % 2 == 1)
        {
            interleaver_iv(&st->interleaver_px2, st->viterbi_p4, len);

            if (st->interleaver_px2.ready)
            {
                nrsc5_conv_decode_p3_p4(st->viterbi_p4, st->scrambler_p4, len);
                descramble(st->scrambler_p4, len);
                frame_push(&st->input->frame, st->scrambler_p4, len, P4_LOGICAL_CHANNEL);
            }
        }
    }
}

void decode_push_pl_pu_s_t(decode_t* st,
    const uint8_t* sym_pl, const uint8_t* sym_pu, const uint8_t* sym_s,
    const uint8_t* sym_t, const unsigned int bc)
{
    memcpy(st->buffer_pl + (bc * BLKSZ * PARTITION_WIDTH_AM), sym_pl, BLKSZ * PARTITION_WIDTH_AM);
    memcpy(st->buffer_pu + (bc * BLKSZ * PARTITION_WIDTH_AM), sym_pu, BLKSZ * PARTITION_WIDTH_AM);
    memcpy(st->buffer_s + (bc * BLKSZ * PARTITION_WIDTH_AM), sym_s, BLKSZ * PARTITION_WIDTH_AM);
    memcpy(st->buffer_t + (bc * BLKSZ * PARTITION_WIDTH_AM), sym_t, BLKSZ * PARTITION_WIDTH_AM);

    decode_process_p1_p3_am(st, bc);
}

void decode_process_p1(decode_t *st)
{
    const int J = 20, B = 16, C = 36, M = 1;
    interleaver_i(st->buffer_pm, st->viterbi_p1,
        J, B, C, M, PM_V, PM_V_SIZE, P1_FRAME_LEN_ENCODED_FM);

    nrsc5_conv_decode_p1(st->viterbi_p1, st->scrambler_p1);
    nrsc5_report_ber(st->input->radio, (float) bit_errors_2_5_fm(st->viterbi_p1, st->scrambler_p1, P1_FRAME_LEN_FM) / P1_FRAME_LEN_ENCODED_FM);
    descramble(st->scrambler_p1, P1_FRAME_LEN_FM);
    frame_push(&st->input->frame, st->scrambler_p1, P1_FRAME_LEN_FM, P1_LOGICAL_CHANNEL);
}

void decode_process_pids(decode_t *st, const unsigned int bc)
{
    const int J = 20, B = 16, C = 36;
    interleaver_ii(st->buffer_pm, st->viterbi_pids, (int)bc, J, B, C, PM_V, PM_V_SIZE, PIDS_FRAME_LEN_ENCODED_FM,
        P1_FRAME_LEN_ENCODED_FM);

    nrsc5_conv_decode_pids(st->viterbi_pids, st->scrambler_pids);
    descramble(st->scrambler_pids, PIDS_FRAME_LEN);
    pids_frame_push(&st->pids, st->scrambler_pids, bc);

    if (bc == 15)
        pids_complete_fm(&st->pids);
}

void decode_process_pids_am(decode_t *st, const uint8_t* sbit, const unsigned int bc)
{
    uint8_t il[120], iu[120];

    /* 1012s.pdf section 10.4 */
    for (int n = 0; n < 120; n++) {
        int k, p, row;

        p = n % 4;

        k = (n + (n/60) + 11) % 30;
        row = (11 * (k + (k/15)) + 3) % 32;
        il[n] = (sbit[row*2] >> p) & 1;

        k = (n + (n/60)) % 30;
        row = (11 * (k + (k/15)) + 3) % 32;
        iu[n] = (sbit[row*2 + 1] >> p) & 1;
    }

    /* 1012s.pdf figure 10-5 */
    const int pids1_disabled = (st->input->sync.psmi == 1) && st->input->sync.rdbi;
    for (int i = 0; i < 10; i++) {
      for (int j = 0; j < 12; j++) {
        st->viterbi_pids[i*24 + pids_il_delay[j]] = pids1_disabled ? 0 : (il[i*12 + j] ? 1 : -1);
        st->viterbi_pids[i*24 + pids_iu_delay[j]] = iu[i*12 + j] ? 1 : -1;
      }
    }

    nrsc5_conv_decode_e2_e3(st->viterbi_pids, st->scrambler_pids, PIDS_FRAME_LEN);
    descramble(st->scrambler_pids, PIDS_FRAME_LEN);
    pids_frame_push(&st->pids, st->scrambler_pids, bc);

    if (bc == 7)
        pids_complete_am(&st->pids);
}

void decode_process_p1_p3_am(decode_t *st, const unsigned int bc)
{
    if (bc == 0)
        st->am_errors = 0;

    if (st->am_diversity_wait == 0)
    {
        nrsc5_conv_decode_e1(st->viterbi_p1_am + (bc * P1_FRAME_LEN_AM * 3), st->scrambler_p1_am, P1_FRAME_LEN_AM);
        st->am_errors += bit_errors_e1(st->viterbi_p1_am + (bc * P1_FRAME_LEN_AM * 3), st->scrambler_p1_am, P1_FRAME_LEN_AM);
        descramble(st->scrambler_p1_am, P1_FRAME_LEN_AM);
        frame_push(&st->input->frame, st->scrambler_p1_am, P1_FRAME_LEN_AM, P1_LOGICAL_CHANNEL);

        if (bc == 7)
        {
            unsigned int total_frame_length = 8 * P1_FRAME_LEN_ENCODED_AM;

            if (!st->input->sync.rdbi)
            {
                if (st->input->sync.psmi != SERVICE_MODE_MA3)
                {
                    total_frame_length += P3_FRAME_LEN_ENCODED_MA1;
                    nrsc5_conv_decode_e2_e3(st->viterbi_p3_am, st->scrambler_p3_am, P3_FRAME_LEN_MA1);
                    st->am_errors += bit_errors_e2(st->viterbi_p3_am, st->scrambler_p3_am, P3_FRAME_LEN_MA1);
                    descramble(st->scrambler_p3_am, P3_FRAME_LEN_MA1);
                    frame_push(&st->input->frame, st->scrambler_p3_am, P3_FRAME_LEN_MA1, P3_LOGICAL_CHANNEL);
                }
                else
                {
                    total_frame_length += P3_FRAME_LEN_ENCODED_MA3;
                    nrsc5_conv_decode_e1(st->viterbi_p3_am, st->scrambler_p3_am, P3_FRAME_LEN_MA3);
                    st->am_errors += bit_errors_e1(st->viterbi_p3_am, st->scrambler_p3_am, P3_FRAME_LEN_MA3);
                    descramble(st->scrambler_p3_am, P3_FRAME_LEN_MA3);
                    frame_push(&st->input->frame, st->scrambler_p3_am, P3_FRAME_LEN_MA3, P3_LOGICAL_CHANNEL);
                }
            }

            nrsc5_report_ber(st->input->radio, (float) st->am_errors / (float) total_frame_length);
        }
    }

    if (bc == 7)
    {
        interleaver_ma1(st);

        if (st->am_diversity_wait > 0)
            st->am_diversity_wait--;
    }
}

static void interleaver_iv_reset(interleaver_iv_t *interleaver)
{
    interleaver->i = 0;
    memset(interleaver->pt, 0, sizeof(unsigned int) * 4);
    interleaver->started = 0;
    interleaver->ready = 0;
}

void decode_reset(decode_t *st)
{
    st->idx_pm = 0;
    st->started_pm = 0;
    st->am_errors = 0;
    st->am_diversity_wait = 4;
    interleaver_iv_reset(&st->interleaver_px1);
    interleaver_iv_reset(&st->interleaver_px2);
    pids_init(&st->pids, st->input);
}

void decode_init(decode_t *st, input_t *input)
{
    st->input = input;
    decode_reset(st);
}
