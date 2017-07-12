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

#include "conv.h"
#include "decode.h"
#include "input.h"

// calculate channel bit error rate by re-encoding and comparing to the input
static float calc_cber(int8_t *coded, uint8_t *decoded)
{
    uint8_t r = 0;
    unsigned int i, j, errors = 0;

    // tail biting
    for (i = 0; i < 6; i++)
        r = (r >> 1) | (decoded[FRAME_LEN - 6 + i] << 6);

    for (i = 0, j = 0; i < FRAME_LEN; i++)
    {
        // shift in new bit
        r = (r >> 1) | (decoded[i] << 6);

        if ((coded[j++] > 0 ? 1 : 0) != (__builtin_popcount(r & 0133) & 1))
            errors++;

        if ((coded[j++] > 0 ? 1 : 0) != (__builtin_popcount(r & 0171) & 1))
            errors++;

        if ((j % 6) == 5)
            j++;
        else if ((coded[j++] > 0 ? 1 : 0) != (__builtin_popcount(r & 0165) & 1))
            errors++;
    }

    return errors / (5.0 / 2.0 * FRAME_LEN);;
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

static void dump_ber(float cber)
{
    static float min = 1, max = 0, sum = 0, count = 0;
    sum += cber;
    count += 1;
    if (cber < min) min = cber;
    if (cber > max) max = cber;
    log_info("BER: %f, avg: %f, min: %f, max: %f", cber, sum / count, min, max);
}

void decode_process(decode_t *st)
{
    const int J = 20, B = 16, C = 36;
    const int8_t v[] = {
        10, 2, 18, 6, 14, 8, 16, 0, 12, 4,
        11, 3, 19, 7, 15, 9, 17, 1, 13, 5
    };
    unsigned int i, out = 0;
    for (i = 0; i < 365440; i++)
    {
        int partition = v[i % J];
        int block = ((i / J) + (partition * 7)) % B;
        int k = i / (J * B);
        int row = (k * 11) % 32;
        int column = (k * 11 + k / (32*9)) % C;
        st->viterbi[out++] = st->buffer[(block * 32 + row) * 720 + partition * C + column];
        if ((out % 6) == 5) // depuncture, [1, 1, 1, 1, 1, 0]
            st->viterbi[out++] = 0;
    }

    nrsc5_conv_decode(st->viterbi, st->scrambler);
    dump_ber(calc_cber(st->viterbi, st->scrambler));
    descramble(st->scrambler, 146176);
    frame_push(&st->input->frame, st->scrambler);
}

void decode_reset(decode_t *st)
{
    st->idx = 0;
}

void decode_init(decode_t *st, struct input_t *input)
{
    st->input = input;
    st->buffer = malloc(720 * BLKSZ * 16);
    st->viterbi = malloc(FRAME_LEN * 3);
    st->scrambler = malloc(FRAME_LEN);

    decode_reset(st);
}
