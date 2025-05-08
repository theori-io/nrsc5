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
#include "private.h"
#include "unicode.h"

#define ALERT_TIMEOUT_LIMIT 16

static char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ ?-*$ ";
static int payload_sizes[] = {
    32, 22, 58, 32, 27, 58, 27, 22,
    58, 58, 27, -1, -1, -1, -1, -1
};

static uint16_t crc12(uint8_t *bits)
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

static int check_crc12(uint8_t *bits)
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

static int crc7(const char *alert, int len)
{
    const unsigned char poly = 0x09;
    unsigned char reg = 0x42;
    int byte_index, bit_index;

    for (byte_index = len - 1; byte_index >= 0; byte_index--)
    {
        for (bit_index = 6; bit_index >= 0; bit_index--)
        {
            unsigned char bit = ((unsigned char)alert[byte_index] >> bit_index) & 1;
            if ((bit_index == 0) && (byte_index > 0))
                bit ^= ((unsigned char)alert[byte_index - 1] >> 7);

            reg <<= 1;
            reg ^= bit;
            if (reg & 0x80)
                reg ^= (0x80 | poly);
        }
    }

    for (bit_index = 6; bit_index >= 0; bit_index--)
    {
        reg <<= 1;
        if (reg & 0x80)
            reg ^= (0x80 | poly);
    }

    return reg;
}

static int control_data_crc(const char *control_data, int len)
{
    unsigned short poly = 0xD010;
    unsigned short reg = 0x7E1B;
    int lowbit;

    int byte_index, bit_index;

    for (byte_index = len - 1; byte_index >= 1; byte_index--)
    {
        for (bit_index = 0; bit_index < 8; bit_index++)
        {
            unsigned short bit =
                ((unsigned char)control_data[byte_index] >> bit_index) & 1;
            if ((byte_index == 1) || (byte_index == 2 && bit_index < 4))
                bit = 0; // Skip CRC bits
            
            lowbit = reg & 1;
            reg >>= 1;
            reg ^= (bit << 15);
            if (lowbit)
                reg ^= poly;
        }
    }

    for (bit_index = 0; bit_index < 16; bit_index++)
    {
        lowbit = reg & 1;
        reg >>= 1;
        if (lowbit)
            reg ^= poly;
    }

    return reg & 0x0fff;
}

static unsigned int decode_int(uint8_t *bits, int *off, unsigned int length)
{
    unsigned int i, result = 0;
    for (i = 0; i < length; i++)
    {
        result <<= 1;
        result |= bits[(*off)++];
    }
    return result;
}

static unsigned int decode_int_reverse(uint8_t *bits, int *off, unsigned int length)
{
    unsigned int i, result = 0;
    for (i = 0; i < length; i++)
        result |= (bits[(*off)++] << i);
    return result;
}

static int decode_signed_int(uint8_t *bits, int *off, unsigned int length)
{
    int result = (int) decode_int(bits, off, length);
    return (result & (1 << (length - 1))) ? result - (1 << length) : result;
}

static char decode_char5(uint8_t *bits, int *off)
{
    return chars[decode_int(bits, off, 5)];
}

static char decode_char7(uint8_t *bits, int *off)
{
    return (char) decode_int(bits, off, 7);
}

static void decode_control_data(const char *control_data, int len, int *category1, int *category2, int *location_format, int *num_locations, int **locations_out)
{
    int i, j;
    uint8_t bits[MAX_ALERT_CNT_LEN * 8];
    int off = 0;
    int locations[MAX_ALERT_LOCATIONS];
    int previous_location = 0;
    int full_len, compressed_len;
    
    for (i = 0; i < len; i++)
        for (j = 0; j < 8; j++)
            bits[i*8 + j] = ((unsigned char)control_data[i] >> j) & 1;

    off += 8; // unknown
    off += 12; // CNT CRC
    off += 8; // unknown
    *category1 = decode_int_reverse(bits, &off, 5);
    *category2 = decode_int_reverse(bits, &off, 5);
    off += 9; // unknown
    *location_format = decode_int_reverse(bits, &off, 3);
    *num_locations = decode_int_reverse(bits, &off, 5);
    off += 1; // unknown

    switch (*location_format)
    {
    case 0: // SAME
        full_len = 20;
        compressed_len = 14;
        break;
    case 1: // FIPS
    case 2: // ZIP
        full_len = 17;
        compressed_len = 10;
        break;
    default:
        log_warn("Invalid location format: %d", *location_format);
        return;
    }

    for (int i = 0; i < *num_locations; i++)
    {
        if (off + 1 >= len*8)
        {
            log_warn("Invalid location data");
            return;
        }

        if ((i == 0) || bits[off++])
        {
            // Full-length location
            if (off + full_len >= len*8)
            {
                log_warn("Invalid location data");
                return;
            }
            locations[i] = decode_int_reverse(bits, &off, full_len);
        }
        else
        {
            // Compressed location
            if (off + compressed_len >= len*8)
            {
                log_warn("Invalid location data");
                return;
            }
            int new_digits = decode_int_reverse(bits, &off, compressed_len);
            int old_digits = (previous_location % 100000) - (previous_location % 1000);
            locations[i] = ((new_digits / 1000) * 100000) + (new_digits % 1000) + old_digits;
        }

        previous_location = locations[i];
    }

    *locations_out = malloc(sizeof(int) * (*num_locations));
    memcpy(*locations_out, locations, sizeof(int) * (*num_locations));
}

static char *utf8_encode(encoding_t encoding, char *buf, int len)
{
    if (encoding == ENCODING_ISO_8859_1)
        return iso_8859_1_to_utf_8((uint8_t *) buf, len);
    else if (encoding == ENCODING_UCS_2)
        return ucs_2_to_utf_8((uint8_t *) buf, len);
    else
        log_warn("Invalid encoding: %d", encoding);

    return NULL;
}

static void report(pids_t *st)
{
    int i;
    const char *country_code = NULL;
    char *name = NULL;
    char *slogan = NULL;
    char *message = NULL;
    char *alert = NULL;
    float latitude = NAN;
    float longitude = NAN;
    int altitude = 0;
    nrsc5_sis_asd_t *audio_services = NULL;
    nrsc5_sis_dsd_t *data_services = NULL;
    int category1 = -1;
    int category2 = -1;
    int location_format = -1;
    int num_locations = -1;
    int *locations = NULL;

    if (st->country_code[0] != 0)
        country_code = st->country_code;

    if (st->universal_short_name_displayed)
        name = utf8_encode(st->universal_short_name_encoding,
                           st->universal_short_name_final,
                           strlen(st->universal_short_name_final));
    else if (st->short_name[0] != 0)
        name = strdup(st->short_name);

    if (st->slogan_displayed)
        slogan = utf8_encode(st->slogan_encoding, st->slogan, st->slogan_len);
    else if (st->long_name_displayed)
        slogan = utf8_encode(ENCODING_ISO_8859_1, st->long_name, strlen(st->long_name));

    if (st->message_displayed)
        message = utf8_encode(st->message_encoding, st->message, st->message_len);

    if (st->alert_displayed)
    {
        alert = utf8_encode(st->alert_encoding, st->alert + st->alert_cnt_len, st->alert_len - st->alert_cnt_len);
        decode_control_data(st->alert, st->alert_cnt_len, &category1, &category2, &location_format, &num_locations, &locations);
    }

    if (!isnan(st->latitude) && !isnan(st->longitude))
    {
        latitude = st->latitude;
        longitude = st->longitude;
        altitude = st->altitude;
    }

    for (i = MAX_AUDIO_SERVICES - 1; i >= 0; i--)
    {
        if (st->audio_services[i].type != -1)
        {
            nrsc5_sis_asd_t *asd = malloc(sizeof(nrsc5_sis_asd_t));
            asd->next = audio_services;
            asd->program = i;
            asd->access = st->audio_services[i].access;
            asd->type = st->audio_services[i].type;
            asd->sound_exp = st->audio_services[i].sound_exp;
            audio_services = asd;
        }
    }

    for (i = MAX_DATA_SERVICES - 1; i >= 0; i--)
    {
        if (st->data_services[i].type != -1)
        {
            nrsc5_sis_dsd_t *dsd = malloc(sizeof(nrsc5_sis_dsd_t));
            dsd->next = data_services;
            dsd->access = st->data_services[i].access;
            dsd->type = st->data_services[i].type;
            dsd->mime_type = st->data_services[i].mime_type;
            data_services = dsd;
        }
    }

    nrsc5_report_sis(st->input->radio, country_code, st->fcc_facility_id, name, slogan, message, alert,
                     (uint8_t *)st->alert, st->alert_cnt_len, category1, category2, location_format, num_locations, locations,
                     latitude, longitude, altitude, audio_services, data_services);

    free(name);
    free(slogan);
    free(message);
    free(alert);
    free(locations);

    while (audio_services)
    {
        nrsc5_sis_asd_t *asd = audio_services;
        audio_services = asd->next;
        free(asd);
    }

    while (data_services)
    {
        nrsc5_sis_dsd_t *dsd = data_services;
        data_services = dsd->next;
        free(dsd);
    }
}

static void reset_alert(pids_t *st)
{
    memset(st->alert, 0, sizeof(st->alert));
    memset(st->alert_have_frame, 0, sizeof(st->alert_have_frame));
    st->alert_seq = -1;
    st->alert_displayed = 0;
    st->alert_timeout = 0;
}

static void decode_sis(pids_t *st, uint8_t *bits)
{
    int payloads, off, i;
    int updated = 0;

    if (bits[0] != 0) return;
    payloads = bits[1] + 1;
    off = 2;

    if (st->alert_displayed)
        st->alert_timeout++;

    for (i = 0; i < payloads; i++)
    {
        int msg_id, payload_size, j;
        char country_code[3] = {0};
        int fcc_facility_id;
        char short_name[8] = {0};
        int seq;
        int current_frame;
        int last_frame;
        float latitude, longitude;
        int altitude_high, altitude_low;
        int category, prog_num;
        asd_t audio_service;
        dsd_t data_service;
        int index, parameter, tzo;

        if (off > 60) break;
        msg_id = decode_int(bits, &off, 4);
        payload_size = payload_sizes[msg_id];

        if (payload_size == -1)
        {
            log_error("unexpected msg_id: %d", msg_id);
            break;
        }

        if (off > 64 - payload_size)
        {
            log_error("not enough room for SIS payload, msg_id: %d", msg_id);
            break;
        }

        switch (msg_id)
        {
        case 0:
            for (j = 0; j < 2; j++)
            {
                country_code[j] = decode_char5(bits, &off);
            }
            off += 3; // reserved
            fcc_facility_id = decode_int(bits, &off, 19);

            if ((strcmp(country_code, st->country_code) != 0) || (fcc_facility_id != st->fcc_facility_id))
            {
                strcpy(st->country_code, country_code);
                st->fcc_facility_id = fcc_facility_id;
                updated = 1;
            }
            break;
        case 1:
            for (j = 0; j < 4; j++)
            {
                short_name[j] = decode_char5(bits, &off);
            }
            if (bits[off] == 0 && bits[off+1] == 1)
                strcat(short_name, "-FM");
            off += 2;

            if (strcmp(short_name, st->short_name) != 0)
            {
                strcpy(st->short_name, short_name);
                updated = 1;
            }
            break;
        case 2:
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
                    st->long_name_displayed = 1;
                    updated = 1;
                }
            }

            off += 3;
            break;
        case 3:
            off += 32;
            break;
        case 4:
            if (bits[off++])
            {
                latitude = decode_signed_int(bits, &off, 22) / 8192.0;
                altitude_high = decode_int(bits, &off, 4) << 8;
                if (latitude != st->latitude)
                {
                    st->latitude = latitude;
                    st->altitude = (st->altitude & 0x0f0) | altitude_high;
                    if (!isnan(st->longitude))
                        updated = 1;
                }
            }
            else
            {
                longitude = decode_signed_int(bits, &off, 22) / 8192.0;
                altitude_low = decode_int(bits, &off, 4) << 4;
                if (longitude != st->longitude)
                {
                    st->longitude = longitude;
                    st->altitude = (st->altitude & 0xf00) | altitude_low;
                    if (!isnan(st->latitude))
                        updated = 1;
                }
            }
            break;
        case 5:
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
                st->message_checksum = decode_int(bits, &off, 7);
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
                    unsigned int checksum = 0;
                    for (j = 0; j < st->message_len; j++)
                        checksum += (unsigned char) st->message[j];
                    checksum = (((checksum >> 8) & 0x7f) + (checksum & 0xff)) & 0x7f;

                    if (checksum == st->message_checksum)
                    {
                        st->message_displayed = 1;
                        updated = 1;
                    }
                    else
                    {
                        log_warn("Invalid message checksum: %d != %d", st->message_checksum, checksum);
                    }
                }
            }
            break;
        case 6:
        case 10:
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
                    updated = 1;
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
                        updated = 1;
                        break;
                    }
                }
                break;
            default:
                log_warn("Unknown service category identifier: %d", category);
            }
            break;
        case 7:
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
                case 12:
                    log_debug("Importer configuration number: %d", parameter);
                    break;
                }
            }
            break;
        case 8:
            current_frame = decode_int(bits, &off, 4);
            if (bits[off++] == 0)
            {
                if (current_frame >= MAX_UNIVERSAL_SHORT_NAME_FRAMES)
                {
                    log_error("unexpected frame number in Universal Short Station Name: %d", current_frame);
                    off += 53;
                    break;
                }

                if (current_frame == 0)
                {
                    st->universal_short_name_encoding = decode_int(bits, &off, 3);
                    st->universal_short_name_append = bits[off++];
                    st->universal_short_name_len = bits[off++] + 1;
                    for (j = 0; j < 6; j++)
                        st->universal_short_name[j] = decode_int(bits, &off, 8);
                }
                else
                {
                    off += 5; // reserved
                    for (j = 0; j < 6; j++)
                        st->universal_short_name[current_frame * 6 + j] = decode_int(bits, &off, 8);
                }
                st->universal_short_name_have_frame[current_frame] = 1;

                if (st->universal_short_name_len >= 0 && !st->universal_short_name_displayed)
                {
                    int complete = 1;
                    for (j = 0; j < st->universal_short_name_len; j++)
                        complete &= st->universal_short_name_have_frame[j];

                    if (complete)
                    {
                        strcpy(st->universal_short_name_final, st->universal_short_name);
                        if (st->universal_short_name_append)
                            strcat(st->universal_short_name_final, "-FM");
                        st->universal_short_name_displayed = 1;
                        updated = 1;
                    }
                }
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
                        st->slogan_displayed = 1;
                        updated = 1;
                    }
                }
            }
            break;
        case 9:
            st->alert_timeout = 0;
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
                st->alert_crc = decode_int(bits, &off, 7);
                st->alert_cnt_len = 1 + (2 * decode_int(bits, &off, 5));
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
                    int expected_alert_crc = crc7(st->alert, st->alert_len);
                    if (st->alert_crc == expected_alert_crc)
                    {
                        if ((st->alert_cnt_len >= 7) && (st->alert_cnt_len <= st->alert_len))
                        {
                            int actual_cnt_crc = (((unsigned char)st->alert[2] & 0x0f) << 8) | (unsigned char)st->alert[1];
                            int expected_cnt_crc = control_data_crc(st->alert, st->alert_cnt_len);
                            if (actual_cnt_crc == expected_cnt_crc)
                            {
                                st->alert_displayed = 1;
                                updated = 1;
                            }
                            else
                            {
                                log_warn("Invalid CNT CRC: 0x%03x != 0x%03x", actual_cnt_crc, expected_cnt_crc);
                            }
                        }
                        else
                        {
                            log_warn("Invalid alert CNT length");
                        }
                    }
                    else
                    {
                        log_warn("Invalid alert CRC: 0x%02x != 0x%02x", st->alert_crc, expected_alert_crc);
                    }
                }
            }
            break;
        }
    }

    if (st->alert_displayed && (st->alert_timeout >= ALERT_TIMEOUT_LIMIT))
    {
        reset_alert(st);
        updated = 1;
    }

    if (updated == 1)
        report(st);
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

void pids_init(pids_t *st, input_t *input)
{
    int i;

    memset(st->country_code, 0, sizeof(st->country_code));
    st->fcc_facility_id = -1;

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

    memset(st->universal_short_name, 0, sizeof(st->universal_short_name));
    memset(st->universal_short_name_final, 0, sizeof(st->universal_short_name_final));
    memset(st->universal_short_name_have_frame, 0, sizeof(st->universal_short_name_have_frame));
    st->universal_short_name_append = -1;
    st->universal_short_name_len = -1;
    st->universal_short_name_displayed = 0;

    memset(st->slogan, 0, sizeof(st->slogan));
    memset(st->slogan_have_frame, 0, sizeof(st->slogan_have_frame));
    st->slogan_len = -1;
    st->slogan_displayed = 0;

    reset_alert(st);

    st->input = input;
}
