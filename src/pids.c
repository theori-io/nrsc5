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
        int category, prog_num;
        asd_t audio_service;
        dsd_t data_service;
        int index, parameter, tzo;

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
            if (off > 64 - 27) break;
            category = decode_int(bits, &off, 2);
            switch (category)
            {
            case 0:
                audio_service.access = decode_int(bits, &off, 1);
                prog_num = decode_int(bits, &off, 6);
                audio_service.type = decode_int(bits, &off, 8);
                off += 5; // reserved
                audio_service.sound_exp = decode_int(bits, &off, 5);

                if (prog_num >= MAX_AUDIO_SERVICES)
                {
                    log_warn("Invalid program number: %d", prog_num);
                    break;
                }

                if (st->audio_services[prog_num].access != audio_service.access
                    || st->audio_services[prog_num].type != audio_service.type
                    || st->audio_services[prog_num].sound_exp != audio_service.sound_exp)
                {
                    st->audio_services[prog_num] = audio_service;
                    log_debug("Audio program %d: %s, type %d, sound experience %d",
                        prog_num, audio_service.access ? "restricted" : "public",
                        audio_service.type, audio_service.sound_exp);
                }
                break;
            case 1:
                data_service.access = decode_int(bits, &off, 1);
                data_service.type = decode_int(bits, &off, 9);
                off += 3; // reserved
                data_service.mime_type = decode_int(bits, &off, 12);

                for (j = 0; j < MAX_DATA_SERVICES; j++)
                {
                    if (st->data_services[j].access == data_service.access
                        && st->data_services[j].type == data_service.type
                        && st->data_services[j].mime_type == data_service.mime_type)
                    {
                        break;
                    }
                    else if (st->data_services[j].type == -1)
                    {
                        st->data_services[j] = data_service;
                        log_debug("Data service: %s, type %d, MIME type %03x",
                            data_service.access ? "restricted" : "public",
                            data_service.type, data_service.mime_type);
                        break;
                    }
                }
                break;
            default:
                log_warn("Unknown service category identifier: %d", category);
            }
            break;
        case 7:
            if (off > 64 - 22) break;
            index = decode_int(bits, &off, 6);
            parameter = decode_int(bits, &off, 16);
            if (index >= NUM_PARAMETERS)
            {
                log_warn("Invalid parameter index: %d", index);
                break;
            }
            if (st->parameters[index] != parameter)
            {
                st->parameters[index] = parameter;
                switch (index)
                {
                case 0:
                    log_debug("Pending leap second offset: %d, current leap second offset: %d",
                        parameter >> 8, parameter & 0xff);
                    break;
                case 1:
                case 2:
                    if (st->parameters[1] >= 0 && st->parameters[2] >= 0)
                        log_debug("ALFN of pending leap second adjustment: %d", st->parameters[2] << 16 | st->parameters[1]);
                    break;
                case 3:
                    tzo = (parameter >> 5) & 0x7ff;
                    if (tzo > 1024) tzo -= 2048;
                    log_debug("Local time zone offset: %d minutes, DST sched. %d, local DST? %s, regional DST? %s",
                        tzo, (parameter >> 2) & 0x7, parameter & 0x2 ? "yes" : "no", parameter & 0x1 ? "yes" : "no");
                    break;
                case 4:
                case 5:
                case 6:
                case 7:
                    if (st->parameters[4] >= 0 && st->parameters[5] >= 0 && st->parameters[6] >= 0 && st->parameters[7] >= 0)
                    {
                        log_debug("Exciter manuf. \"%c%c\", core version %d.%d.%d.%d-%d, manuf. version %d.%d.%d.%d-%d",
                            (st->parameters[4] >> 8) & 0x7f, st->parameters[4] & 0x7f,
                            (st->parameters[5] >> 11) & 0x1f, (st->parameters[5] >> 6) & 0x1f, (st->parameters[5] >> 1) & 0x1f,
                            (st->parameters[7] >> 11) & 0x1f, (st->parameters[7] >> 3) & 0x7,
                            (st->parameters[6] >> 11) & 0x1f, (st->parameters[6] >> 6) & 0x1f, (st->parameters[6] >> 1) & 0x1f,
                            (st->parameters[7] >> 6) & 0x1f, st->parameters[7] & 0x7
                        );
                    }
                    break;
                case 8:
                case 9:
                case 10:
                case 11:
                    if (st->parameters[8] >= 0 && st->parameters[9] >= 0 && st->parameters[10] >= 0 && st->parameters[11] >= 0)
                    {
                        log_debug("Importer manuf. \"%c%c\", core version %d.%d.%d.%d-%d, manuf. version %d.%d.%d.%d-%d",
                            (st->parameters[8] >> 8) & 0x7f, st->parameters[8] & 0x7f,
                            (st->parameters[9] >> 11) & 0x1f, (st->parameters[9] >> 6) & 0x1f, (st->parameters[9] >> 1) & 0x1f,
                            (st->parameters[11] >> 11) & 0x1f, (st->parameters[11] >> 3) & 0x7,
                            (st->parameters[10] >> 11) & 0x1f, (st->parameters[10] >> 6) & 0x1f, (st->parameters[10] >> 1) & 0x1f,
                            (st->parameters[11] >> 6) & 0x1f, st->parameters[11] & 0x7
                        );
                    }
                    break;
                }
            }
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
                            log_warn("Unsupported encoding: %d", st->slogan_encoding);
                        st->slogan_displayed = 1;
                    }
                }
            }
            break;
        case 9:
            if (off > 64 - 58) break;
            current_frame = decode_int(bits, &off, 6);
            seq = decode_int(bits, &off, 2);
            off += 2; // reserved

            if (current_frame == 0)
            {
                if (seq != st->alert_seq)
                {
                    memset(st->alert, 0, sizeof(st->alert));
                    memset(st->alert_have_frame, 0, sizeof(st->alert_have_frame));
                    st->alert_seq = seq;
                    st->alert_displayed = 0;
                }
                st->alert_encoding = decode_int(bits, &off, 3);
                st->alert_len = decode_int(bits, &off, 9);
                off += 7; // CRC-7 integrity check
                st->alert_cnt_len = decode_int(bits, &off, 5);
                for (j = 0; j < 3; j++)
                    st->alert[j] = decode_int(bits, &off, 8);
            }
            else
            {
                for (j = 0; j < 6; j++)
                    st->alert[current_frame * 6 - 3 + j] = decode_int(bits, &off, 8);
            }
            st->alert_have_frame[current_frame] = 1;

            if (st->alert_len >= 0 && !st->alert_displayed)
            {
                int complete = 1;
                for (j = 0; j < (st->alert_len + 8) / 6; j++)
                    complete &= st->alert_have_frame[j];

                if (complete)
                {
                    if (st->alert_encoding == 0)
                        log_debug("Alert: %s", st->alert + 1 + (2 * st->alert_cnt_len));
                    else
                        log_warn("Unsupported encoding: %d", st->alert_encoding);
                    st->alert_displayed = 1;
                }
            }
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
    int i;

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

    for (i = 0; i < MAX_AUDIO_SERVICES; i++)
    {
        st->audio_services[i].access = -1;
        st->audio_services[i].type = -1;
        st->audio_services[i].sound_exp = -1;
    }
    for (i = 0; i < MAX_DATA_SERVICES; i++)
    {
        st->data_services[i].access = -1;
        st->data_services[i].type = -1;
        st->data_services[i].mime_type = -1;
    }

    for (i = 0; i < NUM_PARAMETERS; i++)
        st->parameters[i] = -1;

    memset(st->slogan, 0, sizeof(st->slogan));
    memset(st->slogan_have_frame, 0, sizeof(st->slogan_have_frame));
    st->slogan_len = -1;
    st->slogan_displayed = 0;

    memset(st->alert, 0, sizeof(st->alert));
    memset(st->alert_have_frame, 0, sizeof(st->alert_have_frame));
    st->alert_seq = -1;
    st->alert_displayed = 0;
}
