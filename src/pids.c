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

#include "defines.h"
#include "pids.h"

static char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ ?-*$ ";

uint16_t crc12(uint8_t *bits)
{
    uint16_t poly = 0xD010;
    uint16_t reg = 0x0000;
    int i, lowbit;

    for (i = 67; i >= 0; i--)
    {
        lowbit = reg & 1;
        reg >>= 1;
        reg ^= ((uint16_t)bits[i] << 15);
        if (lowbit) reg ^= poly;
    }
    for (i = 0; i < 16; i++)
    {
        lowbit = reg & 1;
        reg >>= 1;
        if (lowbit) reg ^= poly;
    }
    reg ^= 0x955;
    return reg & 0xfff;
}

int check_crc12(uint8_t *bits)
{
    uint16_t expected_crc = 0;
    int i;

    for (i = 68; i < 80; i++)
    {
        expected_crc <<= 1;
        expected_crc |= bits[i];
    }
    return expected_crc == crc12(bits);
}

unsigned int decode_int(uint8_t *bits, int *off, int length)
{
    int i, result = 0;
    for (i = 0; i < length; i++)
    {
        result <<= 1;
        result |= bits[(*off)++];
    }
    return result;
}

char decode_char5(uint8_t *bits, int *off)
{
    return chars[decode_int(bits, off, 5)];
}

char decode_char7(uint8_t *bits, int *off)
{
    return (char) decode_int(bits, off, 7);
}

void decode_sis(pids_t *st, uint8_t *bits)
{
    int payloads, off, i;

    if (bits[0] != 0) return;
    payloads = bits[1] + 1;
    off = 2;

    for (i = 0; i < payloads; i++)
    {
        int msg_id, j;
        char country_code[3] = {0};
        int fcc_facility_id;
        char short_name[8] = {0};
        int seq;
        int current_frame;
        int last_frame;
        float latitude, longitude;

        if (off > 60) break;
        msg_id = decode_int(bits, &off, 4);

        switch (msg_id)
        {
        case 0:
            if (off > 64 - 32) break;
            for (j = 0; j < 2; j++)
            {
                country_code[j] = decode_char5(bits, &off);
            }
            off += 3; // reserved
            fcc_facility_id = decode_int(bits, &off, 19);

            if ((strcmp(country_code, st->country_code) != 0) || (fcc_facility_id != st->fcc_facility_id))
            {
                log_debug("Country: %s, FCC facility ID: %d", country_code, fcc_facility_id);
                strcpy(st->country_code, country_code);
                st->fcc_facility_id = fcc_facility_id;
            }
            break;
        case 1:
            if (off > 64 - 22) break;
            for (j = 0; j < 4; j++)
            {
                short_name[j] = decode_char5(bits, &off);
            }
            if (bits[off] == 0 && bits[off+1] == 1)
                strcat(short_name, "-FM");
            off += 2;

            if (strcmp(short_name, st->short_name) != 0)
            {
                log_debug("Station Name: %s", short_name);
                strcpy(st->short_name, short_name);
            }
            break;
        case 2:
            if (off > 64 - 58) break;
            off += 55;
            seq = decode_int(bits, &off, 3);
            off -= 58;

            last_frame = decode_int(bits, &off, 3);
            current_frame = decode_int(bits, &off, 3);

            if ((current_frame == 0) && (seq != st->long_name_seq))
            {
                memset(st->long_name, 0, sizeof(st->long_name));
                memset(st->long_name_have_frame, 0, sizeof(st->long_name_have_frame));
                st->long_name_seq = seq;
                st->long_name_displayed = 0;
            }

            for (j = 0; j < 7; j++)
                st->long_name[current_frame * 7 + j] = decode_char7(bits, &off);
            st->long_name_have_frame[current_frame] = 1;

            if ((st->long_name_seq >= 0) && !st->long_name_displayed)
            {
                int complete = 1;
                for (j = 0; j < last_frame + 1; j++)
                    complete &= st->long_name_have_frame[j];

                if (complete)
                {
                    log_debug("Long station name: %s", st->long_name);
                    st->long_name_displayed = 1;
                }
            }

            off += 3;
            break;
        case 3:
            off += 32;
            break;
        case 4:
            if (off > 64 - 27) break;
            if (bits[off++])
            {
                latitude = (bits[off++] ? -1.0 : 1.0) / 8192;
                latitude *= decode_int(bits, &off, 21);
                st->altitude = (st->altitude & 0x0f0) | (decode_int(bits, &off, 4) << 8);
                if ((latitude != st->latitude) && !isnan(st->longitude))
                    log_debug("Station location: %f, %f, %dm", latitude, st->longitude, st->altitude);
                st->latitude = latitude;
            }
            else
            {
                longitude = (bits[off++] ? -1.0 : 1.0) / 8192;
                longitude *= decode_int(bits, &off, 21);
                st->altitude = (st->altitude & 0xf00) | (decode_int(bits, &off, 4) << 4);
                if ((longitude != st->longitude) && !isnan(st->latitude))
                    log_debug("Station location: %f %f, %dm", st->latitude, longitude, st->altitude);
                st->longitude = longitude;
            }
            break;
        case 5:
            if (off > 64 - 58) break;
            current_frame = decode_int(bits, &off, 5);
            seq = decode_int(bits, &off, 2);

            if (current_frame == 0)
            {
                if (seq != st->message_seq)
                {
                    memset(st->message, 0, sizeof(st->message));
                    memset(st->message_have_frame, 0, sizeof(st->message_have_frame));
                    st->message_seq = seq;
                    st->message_displayed = 0;
                }
                st->message_priority = bits[off++];
                st->message_encoding = decode_int(bits, &off, 3);
                st->message_len = decode_int(bits, &off, 8);
                off += 7; // checksum
                for (j = 0; j < 4; j++)
                    st->message[j] = decode_int(bits, &off, 8);
            }
            else
            {
                off += 3; // reserved
                for (j = 0; j < 6; j++)
                    st->message[current_frame * 6 - 2 + j] = decode_int(bits, &off, 8);
            }
            st->message_have_frame[current_frame] = 1;

            if ((st->message_seq >= 0) && !st->message_displayed)
            {
                int complete = 1;
                for (j = 0; j < (st->message_len + 7) / 6; j++)
                    complete &= st->message_have_frame[j];

                if (complete)
                {
                    if (st->message_encoding == 0)
                        log_debug("Message (priority %d): %s", st->message_priority, st->message);
                    else
                        log_debug("Unsupported encoding: %d", st->message_encoding);
                    st->message_displayed = 1;
                }
            }
            break;
        case 6:
            off += 27;
            break;
        case 7:
            off += 22;
            break;
        case 8:
            if (off > 64 - 58) break;
            current_frame = decode_int(bits, &off, 4);
            if (bits[off++] == 0)
            {
                // Fixme: implement Universal Short Station Name
                off += 53;
            }
            else
            {
                if (current_frame == 0)
                {
                    st->slogan_encoding = decode_int(bits, &off, 3);
                    off += 3; // reserved
                    st->slogan_len = decode_int(bits, &off, 7);
                    for (j = 0; j < 5; j++)
                        st->slogan[j] = decode_int(bits, &off, 8);
                }
                else
                {
                    off += 5; // reserved
                    for (j = 0; j < 6; j++)
                        st->slogan[current_frame * 6 - 1 + j] = decode_int(bits, &off, 8);
                }
                st->slogan_have_frame[current_frame] = 1;

                if (st->slogan_len >= 0 && !st->slogan_displayed)
                {
                    int complete = 1;
                    for (j = 0; j < (st->slogan_len + 6) / 6; j++)
                        complete &= st->slogan_have_frame[j];

                    if (complete)
                    {
                        if (st->slogan_encoding == 0)
                            log_debug("Slogan: %s", st->slogan);
                        else
                            log_debug("Unsupported encoding: %d", st->slogan_encoding);
                        st->slogan_displayed = 1;
                    }
                }
            }
            break;
        case 9:
            off += 58;
            break;
        default:
            log_error("unexpected msg_id: %d", msg_id);
        }
    }
}

void pids_frame_push(pids_t *st, uint8_t *bits)
{
    int i;
    uint8_t reversed[PIDS_FRAME_LEN];

    for (i = 0; i < PIDS_FRAME_LEN; i++)
    {
        reversed[i] = bits[((i>>3)<<3) + 7 - (i & 7)];
    }
    if (check_crc12(reversed))
        decode_sis(st, reversed);
}

void pids_init(pids_t *st)
{
    memset(st->country_code, 0, sizeof(st->country_code));
    st->fcc_facility_id = 0;

    memset(st->short_name, 0, sizeof(st->short_name));
    
    st->long_name_seq = -1;
    st->long_name_displayed = 0;

    st->latitude = NAN;
    st->longitude = NAN;
    st->altitude = 0;

    st->message_seq = -1;
    st->message_displayed = 0;

    memset(st->slogan, 0, sizeof(st->slogan));
    memset(st->slogan_have_frame, 0, sizeof(st->slogan_have_frame));
    st->slogan_len = -1;
    st->slogan_displayed = 0;
}
