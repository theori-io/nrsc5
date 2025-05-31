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
#include <time.h>

#include "here_images.h"
#include "private.h"

static void process_packet(here_images_t *st)
{
    if (st->payload_len < 28)
    {
        log_warn("HERE Image frame too short");
        return;
    }

    int image_type = (st->buffer[0] >> 4);
    int seq = (st->buffer[0] & 0x0f);

    if ((image_type != NRSC5_HERE_IMAGE_TRAFFIC) && (image_type != NRSC5_HERE_IMAGE_WEATHER))
    {
        log_warn("Unknown HERE Image type: %d", image_type);
        return;
    }

    int n1 = (st->buffer[2] << 8) | st->buffer[3];
    int n2 = (st->buffer[4] << 8) | st->buffer[5];
    unsigned int timestamp = ((unsigned int)st->buffer[9] << 24) | (st->buffer[10] << 16)
                           | (st->buffer[11] << 8) | st->buffer[12];

    int lat1 = ((st->buffer[14] & 0x7f) << 18) | (st->buffer[15] << 10) | (st->buffer[16] << 2) | (st->buffer[17] >> 6);
    if (st->buffer[14] & 0x80)
        lat1 = -lat1;

    int lon1 = ((st->buffer[17] & 0x1f) << 20) | (st->buffer[18] << 12) | (st->buffer[19] << 4) | (st->buffer[20] >> 4);
    if (st->buffer[17] & 0x20)
        lon1 = -lon1;

    int lat2 = ((st->buffer[20] & 0x07) << 22) | (st->buffer[21] << 14) | (st->buffer[22] << 6) | (st->buffer[23] >> 2);
    if (st->buffer[20] & 0x08)
        lat2 = -lat2;

    int lon2 = ((st->buffer[23] & 0x01) << 24) | (st->buffer[24] << 16) | (st->buffer[25] << 8) | st->buffer[26];
    if (st->buffer[23] & 0x02)
        lon2 = -lon2;

    int filename_len = st->buffer[27];

    if (st->payload_len < 34 + filename_len)
    {
        log_warn("HERE Image frame too short");
        return;
    }

    int file_len = (st->buffer[32 + filename_len] << 8) | st->buffer[33 + filename_len];

    if (st->payload_len < 34 + filename_len + file_len)
    {
        log_warn("HERE Image frame too short");
        return;
    }

    st->buffer[28 + filename_len] = '\0';

    nrsc5_report_here_image(st->radio, image_type, seq, n1, n2, timestamp,
                            lat1 / 100000.f, lon1 / 100000.f, lat2 / 100000.f, lon2 / 100000.f,
                            (char *)&st->buffer[28], file_len, &st->buffer[34 + filename_len]);
}

void here_images_push(here_images_t *st, uint16_t seq, unsigned int len, uint8_t *buf)
{
    if (seq != st->expected_seq)
    {
        memset(st->buffer, 0, sizeof(st->buffer));
        st->payload_len = -1;
        st->sync_state = 0;
    }

    for (unsigned int offset = 0; offset < len; offset++)
    {
        st->sync_state <<= 8;
        st->sync_state |= buf[offset];

        if (st->payload_len == -1) // waiting for sync
        {
            if (((st->sync_state >> 16) & 0xffffffff) == 0xfff7fff7)
            {
                st->payload_len = st->sync_state & 0xffff;
                st->buffer_idx = 0;
            }
        }
        else
        {
            st->buffer[st->buffer_idx++] = buf[offset];
            if (st->buffer_idx == (st->payload_len + 2))
            {
                process_packet(st);
                st->payload_len = -1;
            }
        }
    }

    st->expected_seq = (seq + 1) & 0xffff;
}

void here_images_reset(here_images_t *st)
{
    st->expected_seq = -1;
}

void here_images_init(here_images_t *st, nrsc5_t *radio)
{
    st->radio = radio;
}
