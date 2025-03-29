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
#include "private.h"

/*
 * GNU Radio Filter Design Tool
 * FIR, Low Pass, Kaiser Window
 * Sample rate: 1488375
 * End of pass band: 372094
 * Start of stop band: 530000
 * Stop band attenuation: 40
 */
static float decim_taps[] = {
    0.6062333583831787,
    -0.13481467962265015,
    0.032919470220804214,
    -0.00410953676328063
};

void input_pdu_push(input_t *st, uint8_t *pdu, unsigned int len, unsigned int program, unsigned int stream_id)
{
    output_push(st->output, pdu, len, program, stream_id);
}

int input_shift(input_t *st, unsigned int cnt)
{
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

    if (cnt + st->avail > INPUT_BUF_LEN)
    {
        log_error("input buffer overflow!");
        return -1;
    }

    return 0;
}

void input_push(input_t *st)
{
    while (st->avail - st->used >= (st->radio->mode == NRSC5_MODE_FM ? FFTCP_FM : FFTCP_AM))
    {
        st->used += acquire_push(&st->acq, &st->buffer[st->used], st->avail - st->used);
        acquire_process(&st->acq);
    }
}

void input_push_cu8(input_t *st, const uint8_t *buf, uint32_t len)
{
    unsigned int i;
    assert(len % 4 == 0);

    nrsc5_report_iq(st->radio, buf, len);

    if (input_shift(st, len / 4) != 0)
        return;

    for (i = 0; i < len; i += 4)
    {
        cint16_t x[2];

        x[0].r = U8_Q15(buf[i]);
        x[0].i = U8_Q15(buf[i + 1]);
        x[1].r = U8_Q15(buf[i + 2]);
        x[1].i = U8_Q15(buf[i + 3]);

        if (st->radio->mode == NRSC5_MODE_FM)
        {
            halfband_q15_execute(st->decim[0], x, &st->buffer[st->avail++]);
        }
        else
        {
            x[0].r >>= 4;
            x[0].i >>= 4;
            x[1].r >>= 4;
            x[1].i >>= 4;

            halfband_q15_execute(st->decim[0], x, &st->stages[0][st->offset & 1]);
            if ((st->offset & 0x1) == 0x1) {
                halfband_q15_execute(st->decim[1], st->stages[0], &st->stages[1][(st->offset >> 1) & 1]);
            }
            if ((st->offset & 0x3) == 0x3) {
                halfband_q15_execute(st->decim[2], st->stages[1], &st->stages[2][(st->offset >> 2) & 1]);
            }
            if ((st->offset & 0x7) == 0x7) {
                halfband_q15_execute(st->decim[3], st->stages[2], &st->stages[3][(st->offset >> 3) & 1]);
            }
            if ((st->offset & 0xf) == 0xf) {
                halfband_q15_execute(st->decim[4], st->stages[3], &st->buffer[st->avail++]);
            }
            st->offset++;
        }
    }

    input_push(st);
}

void input_push_cs16(input_t *st, const int16_t *buf, uint32_t len)
{
    assert(len % 2 == 0);

    if (input_shift(st, len / 2) != 0)
        return;

    memcpy(&st->buffer[st->avail], buf, len * sizeof(int16_t));
    st->avail += len / 2;

    input_push(st);
}

void input_reset(input_t *st)
{
    st->avail = 0;
    st->used = 0;
    st->offset = 0;

    input_set_sync_state(st, SYNC_STATE_NONE);
    for (int i = 0; i < AM_DECIM_STAGES; i++)
        firdecim_q15_reset(st->decim[i]);
    acquire_reset(&st->acq);
    decode_reset(&st->decode);
    frame_reset(&st->frame);
    sync_reset(&st->sync);
}

void input_init(input_t *st, nrsc5_t *radio, output_t *output)
{
    st->radio = radio;
    st->output = output;
    st->sync_state = SYNC_STATE_NONE;

    for (int i = 0; i < AM_DECIM_STAGES; i++)
        st->decim[i] = firdecim_q15_create(decim_taps, sizeof(decim_taps) / sizeof(decim_taps[0]));

    acquire_init(&st->acq, st);
    decode_init(&st->decode, st);
    frame_init(&st->frame, st);
    sync_init(&st->sync, st);

    input_reset(st);
}

void input_set_mode(input_t *st)
{
    acquire_set_mode(&st->acq, st->radio->mode);
    input_reset(st);
}

void input_free(input_t *st)
{
    acquire_free(&st->acq);
    frame_free(&st->frame);

    for (int i = 0; i < AM_DECIM_STAGES; i++)
        firdecim_q15_free(st->decim[i]);
}

void input_set_sync_state(input_t *st, unsigned int new_state)
{
    if (st->sync_state == new_state)
        return;

    if (st->sync_state == SYNC_STATE_FINE)
        nrsc5_report_lost_sync(st->radio);
    if (new_state == SYNC_STATE_FINE)
    {
        nrsc5_report_sync(st->radio);
        log_debug("Primary service mode: %d", st->sync.psmi);
    }

    st->sync_state = new_state;
}

void input_aas_push(input_t *st, uint8_t *psd, unsigned int len)
{
    output_aas_push(st->output, psd, len);
}
