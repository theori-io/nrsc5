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

int decode_int(uint8_t *bits, int *off, int length)
{
    int i, result = 0;
    for (i = 0; i < length; i++)
    {
        result <<= 1;
        result |= bits[(*off)++];
    }
    return result;
}

char decode_char(uint8_t *bits, int *off)
{
    return chars[decode_int(bits, off, 5)];
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

        if (off > 60) break;
        msg_id = decode_int(bits, &off, 4);

        switch (msg_id)
        {
        case 0:
            if (off > 64 - 32) break;
            for (j = 0; j < 2; j++)
            {
                country_code[j] = decode_char(bits, &off);
            }
            off += 3;
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
                short_name[j] = decode_char(bits, &off);
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
            off += 58;
            break;
        case 3:
            off += 32;
            break;
        case 4:
            off += 27;
            break;
        case 5:
            off += 58;
            break;
        case 6:
            off += 27;
            break;
        case 7:
            off += 22;
            break;
        case 8:
            off += 58;
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
}
