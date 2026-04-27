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
#define PIDS_TYPE_SIS 0
#define PIDS_TYPE_LLDS 1

#define SIS_MSG_ID_STATION_ID 0
#define SIS_MSG_ID_STATION_NAME_SHORT 1
#define SIS_MSG_ID_STATION_NAME_LONG  2
#define SIS_MSG_ID_STATION_LOCATION  4
#define SIS_MSG_ID_STATION_MESSAGE   5
#define SIS_MSG_ID_SERVICE_INFORMATION 6
#define SIS_MSG_ID_PARAMETER_MESSAGE 7
#define SIS_MSG_ID_UNIVERSAL_SHORT_STATION_NAME 8
#define SIS_MSG_ID_EMERGENCY_ALERTS_MESSAGE 9
#define SIS_MSG_ID_ADV_SERVICE_INFORMATION 10

#define SIS_SERVICE_CATEGORY_AUDIO 0
#define SIS_SERVICE_CATEGORY_DATA 1

#define SIS_EA_LOCATION_FORMAT_SAME 0
#define SIS_EA_LOCATION_FORMAT_FIPS 1
#define SIS_EA_LOCATION_FORMAT_ZIP  2


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

static unsigned int decode_int(const uint8_t *bits, int *off, unsigned int length)
{
    unsigned int i, result = 0;
    for (i = 0; i < length; i++)
    {
        result <<= 1;
        result |= bits[(*off)++];
    }
    return result;
}

static unsigned int decode_int_reverse(const uint8_t *bits, int *off, unsigned int length)
{
    unsigned int i, result = 0;
    for (i = 0; i < length; i++)
        result |= (bits[(*off)++] << i);
    return result;
}

static int decode_signed_int(const uint8_t *bits, int *off, const unsigned int length)
{
    int result = (int) decode_int(bits, off, length);
    return (result & (1 << (length - 1))) ? result - (1 << length) : result;
}

static char decode_char5(const uint8_t *bits, int *off)
{
    return chars[decode_int(bits, off, 5)];
}

static char decode_char7(const uint8_t *bits, int *off)
{
    return (char) decode_int(bits, off, 7);
}

static int decode_locations(const uint8_t *bits, const int len, int locations[MAX_ALERT_LOCATIONS], const int location_format, const int num_locations)
{
    int off = 0;
    int previous_location = 0;
    int full_len, compressed_len;

    switch (location_format)
    {
    case SIS_EA_LOCATION_FORMAT_SAME:
        full_len = 20;
        compressed_len = 14;
        break;
    case SIS_EA_LOCATION_FORMAT_FIPS:
    case SIS_EA_LOCATION_FORMAT_ZIP:
        full_len = 17;
        compressed_len = 10;
        break;
    default:
        log_warn("Invalid location format: %d", location_format);
        return -1;
    }

    for (int i = 0; i < num_locations; i++)
    {
        if (off + 1 > len)
        {
            log_warn("Invalid location data");
            return -1;
        }

        if ((i == 0) || bits[off++])
        {
            // Full-length location
            if (off + full_len > len)
            {
                log_warn("Invalid location data");
                return -1;
            }
            locations[i] = (int)decode_int_reverse(bits, &off, full_len);
        }
        else
        {
            // Compressed location
            if (off + compressed_len > len)
            {
                log_warn("Invalid location data");
                return -1;
            }
            int new_digits = (int)decode_int_reverse(bits, &off, compressed_len);
            int old_digits = (previous_location % 100000) - (previous_location % 1000);
            locations[i] = ((new_digits / 1000) * 100000) + (new_digits % 1000) + old_digits;
        }

        previous_location = locations[i];
    }

    return 0;
}

static void decode_control_data(const char *control_data, const int len, int *category1, int *category2, int locations[MAX_ALERT_LOCATIONS], int *location_format, int *num_locations)
{
    uint8_t bits[MAX_ALERT_CNT_LEN * 8];
    int off = 0;

    for (int i = 0; i < len; i++)
        for (int j = 0; j < 8; j++)
            bits[i*8 + j] = ((unsigned char)control_data[i] >> j) & 1;

    off += 8; // unknown
    off += 12; // CNT CRC
    off += 8; // unknown
    *category1 = (int)decode_int_reverse(bits, &off, 5);
    *category2 = (int)decode_int_reverse(bits, &off, 5);
    off += 9; // unknown
    *location_format = (int)decode_int_reverse(bits, &off, 3);
    *num_locations = (int)decode_int_reverse(bits, &off, 5);
    off += 1; // unknown

    if (decode_locations(bits + off, len*8 - off, locations, *location_format, *num_locations) < 0)
        *num_locations = 0;
}

static char *utf8_encode(const encoding_t encoding, const char *buf, const int len)
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
    int locations[MAX_ALERT_LOCATIONS];

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
        decode_control_data(st->alert, st->alert_cnt_len, &category1, &category2, locations, &location_format, &num_locations);
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

static int sis_decode_station_id(pids_t *st, const uint8_t* bits)
{
    int updated = 0;
    char country_code[3] = {0};
    int fcc_facility_id;
    int j, off = 0;

    for (j = 0; j < 2; j++)
    {
        country_code[j] = decode_char5(bits, &off);
    }
    off += 3; // reserved
    fcc_facility_id = (int)decode_int(bits, &off, 19);

    if ((strcmp(country_code, st->country_code) != 0) || (fcc_facility_id != st->fcc_facility_id))
    {
        strcpy(st->country_code, country_code);
        st->fcc_facility_id = fcc_facility_id;
        nrsc5_report_station_id(st->input->radio, st->country_code, st->fcc_facility_id);
        updated = 1;
    }

    return updated;
}

static int sis_decode_station_name_short(pids_t *st, const uint8_t* bits)
{
    char short_name[8] = {0};
    int off = 0, updated = 0, j;

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
        nrsc5_report_station_name(st->input->radio, st->short_name);
        updated = 1;
    }

    return updated;
}

static int sis_decode_station_long_name(pids_t *st, const uint8_t* bits)
{
    unsigned int j;
    int updated = 0, off = 0;
    int tmp = 55;

    const unsigned last_frame = decode_int(bits, &off, 3);
    const unsigned current_frame = decode_int(bits, &off, 3);
    const int seq = (int)decode_int(bits, &tmp, 3);

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

            if (!st->slogan_displayed)
            {
                char *slogan = utf8_encode(ENCODING_ISO_8859_1, st->long_name, strlen(st->long_name));
                nrsc5_report_station_slogan(st->input->radio, slogan);
                free(slogan);
            }

            updated = 1;
        }
    }
    return updated;
}

static int sis_decode_station_location(pids_t *st, const uint8_t* bits)
{
    int updated = 0, off = 0;

    if (bits[off++])
    {
        const float latitude = (float)decode_signed_int(bits, &off, 22) / 8192.0f;
        const unsigned int altitude_high = decode_int(bits, &off, 4) << 8;
        if ((latitude != st->latitude) || (altitude_high != (st->altitude & 0xf00)))
        {
            st->latitude = latitude;
            st->altitude = (st->altitude & 0x0f0) | altitude_high;
            if (!isnan(st->longitude))
            {
                nrsc5_report_station_location(st->input->radio, st->latitude, st->longitude, st->altitude);
                updated = 1;
            }
        }
    }
    else
    {
        const float longitude = (float)decode_signed_int(bits, &off, 22) / 8192.0f;
        const unsigned altitude_low = decode_int(bits, &off, 4) << 4;
        if ((longitude != st->longitude) || (altitude_low != (st->altitude & 0x0f0)))
        {
            st->longitude = longitude;
            st->altitude = (st->altitude & 0xf00) | altitude_low;
            if (!isnan(st->latitude))
            {
                nrsc5_report_station_location(st->input->radio, st->latitude, st->longitude, st->altitude);
                updated = 1;
            }
        }
    }

    return updated;
}

static int sis_decode_station_message(pids_t *st, const uint8_t* bits)
{
    int off = 0, updated = 0, j;
    const int current_frame = (int)decode_int(bits, &off, 5);
    const int seq = (int)decode_int(bits, &off, 2);

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
        st->message_len = (int)decode_int(bits, &off, 8);
        st->message_checksum = decode_int(bits, &off, 7);
        for (j = 0; j < 4; j++)
            st->message[j] = (char)decode_int(bits, &off, 8);
    }
    else
    {
        off += 3; // reserved
        for (j = 0; j < 6; j++)
            st->message[current_frame * 6 - 2 + j] = (char)decode_int(bits, &off, 8);
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

                char *message = utf8_encode(st->message_encoding, st->message, st->message_len);
                nrsc5_report_station_message(st->input->radio, message);
                free(message);

                updated = 1;
            }
            else
            {
                log_warn("Invalid message checksum: %d != %d", st->message_checksum, checksum);
            }
        }
    }

    return updated;
}

static int sis_decode_service_information(pids_t *st, const uint8_t* bits)
{
    int updated = 0, off = 0, j;
    int prog_num;
    asd_t audio_service;
    dsd_t data_service;

    const unsigned int category = decode_int(bits, &off, 2);

    switch (category)
    {
    case SIS_SERVICE_CATEGORY_AUDIO:
        audio_service.access = (int)decode_int(bits, &off, 1);
        prog_num = (int)decode_int(bits, &off, 6);
        audio_service.type = (int)decode_int(bits, &off, 8);
        off += 5; // reserved
        audio_service.sound_exp = (int)decode_int(bits, &off, 5);

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
            nrsc5_report_asd(st->input->radio, prog_num, audio_service.access, audio_service.type, audio_service.sound_exp);
            updated = 1;
        }
        break;
    case SIS_SERVICE_CATEGORY_DATA:
        data_service.access = (int)decode_int(bits, &off, 1);
        data_service.type = (int)decode_int(bits, &off, 9);
        off += 3; // reserved
        data_service.mime_type = (int)decode_int(bits, &off, 12);

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
                nrsc5_report_dsd(st->input->radio, data_service.access, data_service.type, data_service.mime_type);
                updated = 1;
                break;
            }
        }
        break;
    default:
        log_warn("Unknown service category identifier: %d", category);
    }

    return updated;
}

static void sis_decode_parameter(pids_t *st, const uint8_t* bits)
{
    int off = 0;

    const unsigned int index = decode_int(bits, &off, 6);
    const int parameter = (int)decode_int(bits, &off, 16);

    if (index >= NUM_PARAMETERS)
    {
        log_warn("Invalid parameter index: %d", index);
        return;
    }

    if (st->parameters[index] != parameter)
    {
        st->parameters[index] = parameter;
        switch (index)
        {
        case 0:
        case 1:
        case 2:
            if (st->parameters[0] >= 0 && st->parameters[1] >= 0 && st->parameters[2] >= 0)
            {
                const int pending_offset = st->parameters[0] >> 8;
                const int current_offset = st->parameters[0] & 0xff;
                const unsigned int pending_alfn = (unsigned int)st->parameters[2] << 16 | st->parameters[1];

                nrsc5_report_leap_second_offset(st->input->radio, pending_offset, current_offset, pending_alfn);
            }
            break;
        case 3:
            {
                const int dst_regional = st->parameters[3] & 0x1;
                const int dst_local = (st->parameters[3] >> 1) & 0x1;
                const int dst_schedule = (st->parameters[3] >> 2) & 0x7;
                int tzo = (st->parameters[3] >> 5) & 0x7ff;

                if (tzo >= 1024)
                    tzo -= 2048;

                nrsc5_report_local_time(st->input->radio, tzo, dst_regional, dst_local, dst_schedule);
            }
            break;
        case 4:
        case 5:
        case 6:
        case 7:
            if (st->parameters[4] >= 0 && st->parameters[5] >= 0 && st->parameters[6] >= 0 && st->parameters[7] >= 0)
            {
                const char manufacturer_id[3] = {
                    (char)((st->parameters[4] >> 8) & 0x7f), (char)(st->parameters[4] & 0x7f),
                    '\0'
                };
                const int core_version[NRSC5_DEVICE_VERSION_LENGTH] = {
                    (st->parameters[5] >> 11) & 0x1f, (st->parameters[5] >> 6) & 0x1f, (st->parameters[5] >> 1) & 0x1f,
                    (st->parameters[7] >> 11) & 0x1f
                };
                const int manufacturer_version[NRSC5_DEVICE_VERSION_LENGTH] = {
                    (st->parameters[6] >> 11) & 0x1f, (st->parameters[6] >> 6) & 0x1f, (st->parameters[6] >> 1) & 0x1f,
                    (st->parameters[7] >> 6) & 0x1f,
                };

                const int core_status = (st->parameters[7] >> 3) & 0x7;
                const int manufacturer_status = st->parameters[7] & 0x7;
                const int importer_connected = (st->parameters[4] >> 7) & 0x1;

                nrsc5_report_exciter_info(st->input->radio, manufacturer_id, core_version, manufacturer_version,
                    core_status, manufacturer_status, importer_connected);
            }
            break;
        case 8:
        case 9:
        case 10:
        case 11:
            if (st->parameters[8] >= 0 && st->parameters[9] >= 0 && st->parameters[10] >= 0 && st->parameters[11] >= 0)
            {
                const char manufacturer_id[3] = {
                    (char)((st->parameters[8] >> 8) & 0x7f), (char)(st->parameters[8] & 0x7f),
                    '\0'
                };
                const int core_version[NRSC5_DEVICE_VERSION_LENGTH] = {
                    (st->parameters[9] >> 11) & 0x1f, (st->parameters[9] >> 6) & 0x1f, (st->parameters[9] >> 1) & 0x1f,
                    (st->parameters[11] >> 11) & 0x1f,
                };
                const int manufacturer_version[NRSC5_DEVICE_VERSION_LENGTH] = {
                    (st->parameters[10] >> 11) & 0x1f, (st->parameters[10] >> 6) & 0x1f, (st->parameters[10] >> 1) & 0x1f,
                    (st->parameters[11] >> 6) & 0x1f
                };
                const int core_status = (st->parameters[11] >> 3) & 0x7;
                const int manufacturer_status = st->parameters[11] & 0x7;

                nrsc5_report_importer_info(st->input->radio, manufacturer_id, core_version, manufacturer_version,
                    core_status, manufacturer_status);
            }
            break;
        case 12:
            log_debug("Importer configuration number: %d", parameter);
            break;
        default:
            log_warn("Unknown SIS parameter index: %d", index);
            break;
        }
    }
}

static int sis_decode_universal_short_station_name(pids_t *st, const uint8_t *bits)
{
    int j, off = 0;
    int updated = 0;

    const unsigned int current_frame = decode_int(bits, &off, 4);
    if (bits[off++] == 0)
    {
        if (current_frame >= MAX_UNIVERSAL_SHORT_NAME_FRAMES)
        {
            log_error("unexpected frame number in Universal Short Station Name: %d", current_frame);
            off += 53;
            return 0;
        }

        if (current_frame == 0)
        {
            st->universal_short_name_encoding = decode_int(bits, &off, 3);
            st->universal_short_name_append = bits[off++];
            st->universal_short_name_len = bits[off++] + 1;
            for (j = 0; j < 6; j++)
                st->universal_short_name[j] = (char)decode_int(bits, &off, 8);
        }
        else
        {
            off += 5; // reserved
            for (j = 0; j < 6; j++)
                st->universal_short_name[current_frame * 6 + j] = (char)decode_int(bits, &off, 8);
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

                char *name = utf8_encode(st->universal_short_name_encoding,
                                         st->universal_short_name_final,
                                         strlen(st->universal_short_name_final));
                nrsc5_report_station_name(st->input->radio, name);
                free(name);

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
            st->slogan_len = (int)decode_int(bits, &off, 7);
            for (j = 0; j < 5; j++)
                st->slogan[j] = (char)decode_int(bits, &off, 8);
        }
        else
        {
            off += 5; // reserved
            for (j = 0; j < 6; j++)
                st->slogan[current_frame * 6 - 1 + j] = (char)decode_int(bits, &off, 8);
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

                if (!st->long_name_displayed)
                {
                    char *slogan = utf8_encode(st->slogan_encoding, st->slogan, st->slogan_len);
                    nrsc5_report_station_slogan(st->input->radio, slogan);
                    free(slogan);
                }

                updated = 1;
            }
        }
    }

    return updated;
}

static int sis_decode_emergency_alerts(pids_t *st, const uint8_t *bits)
{
    int off = 0, updated = 0, j;

    const unsigned int current_frame = decode_int(bits, &off, 6);
    const int seq = (int)decode_int(bits, &off, 2);
    off += 2; // reserved

    st->alert_timeout = 0;

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
        st->alert_len = (int)decode_int(bits, &off, 9);
        st->alert_crc = (int)decode_int(bits, &off, 7);
        st->alert_cnt_len = 1 + (2 * (int)decode_int(bits, &off, 5));
        for (j = 0; j < 3; j++)
            st->alert[j] = (char)decode_int(bits, &off, 8);
    }
    else
    {
        for (j = 0; j < 6; j++)
            st->alert[current_frame * 6 - 3 + j] = (char)decode_int(bits, &off, 8);
    }
    st->alert_have_frame[current_frame] = 1;

    if (st->alert_len >= 0 && !st->alert_displayed)
    {
        int complete = 1;
        for (j = 0; j < (st->alert_len + 8) / 6; j++)
            complete &= st->alert_have_frame[j];

        if (complete)
        {
            const int expected_alert_crc = crc7(st->alert, st->alert_len);
            if (st->alert_crc != expected_alert_crc)
            {
                log_warn("Invalid alert CRC: 0x%02x != 0x%02x", st->alert_crc, expected_alert_crc);
                return 0;
            }

            if ((st->alert_cnt_len < 7) || (st->alert_len < st->alert_cnt_len))
            {
                log_warn("Invalid alert CNT length");
                return 0;
            }

            const int actual_cnt_crc = (((unsigned char)st->alert[2] & 0x0f) << 8) | (unsigned char)st->alert[1];
            const int expected_cnt_crc = control_data_crc(st->alert, st->alert_cnt_len);
            if (actual_cnt_crc == expected_cnt_crc)
            {
                st->alert_displayed = 1;

                int category1 = -1;
                int category2 = -1;
                int location_format = -1;
                int num_locations = -1;
                int locations[MAX_ALERT_LOCATIONS];
                char *message = utf8_encode(st->alert_encoding, st->alert + st->alert_cnt_len, st->alert_len - st->alert_cnt_len);
                decode_control_data(st->alert, st->alert_cnt_len, &category1, &category2, locations, &location_format, &num_locations);
                nrsc5_report_emergency_alert(st->input->radio, message, (uint8_t *)st->alert, st->alert_cnt_len, category1, category2, location_format, num_locations, locations);
                free(message);

                updated = 1;
            }
            else
            {
                log_warn("Invalid CNT CRC: 0x%03x != 0x%03x", actual_cnt_crc, expected_cnt_crc);
            }
        }
    }

    return updated;
}

static void sis_decode(pids_t *st, const uint8_t *bits, const unsigned int bc)
{
    int off = 0;
    int updated = 0;

    const int payloads = bits[0] + 1;
    off += 1;

    if (st->alert_displayed)
        st->alert_timeout++;

    for (int i = 0; i < payloads; i++)
    {
        if (off > 59) break;
        const unsigned int msg_id = decode_int(bits, &off, 4);
        const int payload_size = payload_sizes[msg_id];

        if (payload_size == -1)
        {
            log_error("unexpected msg_id: %d", msg_id);
            break;
        }

        if (off > 63 - payload_size)
        {
            log_error("not enough room for SIS payload, msg_id: %d", msg_id);
            break;
        }

        switch (msg_id)
        {
        case SIS_MSG_ID_STATION_ID:
            if (sis_decode_station_id(st, bits + off))
                updated = 1;
            off += 32;
            break;
        case SIS_MSG_ID_STATION_NAME_SHORT:
            if (sis_decode_station_name_short(st, bits + off))
                updated = 1;
            off += 22;
            break;
        case SIS_MSG_ID_STATION_NAME_LONG:
            if (sis_decode_station_long_name(st, bits + off))
                updated = 1;
            off += 58;
            break;
        case 3:
            // reserved
            off += 32;
            break;
        case SIS_MSG_ID_STATION_LOCATION:
            if (sis_decode_station_location(st, bits + off))
                updated = 1;
            off += 27;
            break;
        case SIS_MSG_ID_STATION_MESSAGE:
            if (sis_decode_station_message(st, bits + off))
                updated = 1;
            off += 58;
            break;
        case SIS_MSG_ID_SERVICE_INFORMATION:
        case SIS_MSG_ID_ADV_SERVICE_INFORMATION:
            if (sis_decode_service_information(st, bits + off))
                updated = 1;
            off += 27;
            break;
        case SIS_MSG_ID_PARAMETER_MESSAGE:
            sis_decode_parameter(st, bits + off);
            off += 22;
            break;
        case SIS_MSG_ID_UNIVERSAL_SHORT_STATION_NAME:
            if (sis_decode_universal_short_station_name(st, bits + off))
                updated = 1;
            off += 58;
            break;
        case SIS_MSG_ID_EMERGENCY_ALERTS_MESSAGE:
            if (sis_decode_emergency_alerts(st, bits + off))
                updated = 1;
            off += 58;
            break;
        default:
            log_warn("Unknown SIS MSG ID: %d", msg_id);
            break;
        }
    }

    const uint8_t time_locked = bits[64];
    const uint8_t adv_alfn = bits[65] << 1 | bits[66];

    alfn_t* alfn = &st->alfn[st->alfn_frame];

    alfn->value |= (uint32_t)adv_alfn << (2 * bc);
    alfn->received |= (1 << bc);

    st->alfn_time_locked = time_locked;

    if (st->alert_displayed && (st->alert_timeout >= ALERT_TIMEOUT_LIMIT))
    {
        reset_alert(st);
        nrsc5_report_emergency_alert(st->input->radio, NULL, NULL, -1, -1, -1, -1, -1, NULL);
        updated = 1;
    }

    if (updated)
        report(st);
}

void pids_complete_fm(pids_t* st)
{
    alfn_t* alfn = &st->alfn[0];

    if (alfn->received == 0xFFFF)
        nrsc5_report_alfn(st->input->radio, alfn->value, st->alfn_time_locked);
    alfn->value = 0;
    alfn->received = 0;
}

static void pids_report_alfn_am(const pids_t* st)
{
    // Use latest ALFN lower bits.
    int lower_idx = st->alfn_frame;
    if (st->alfn_upper_idx == st->alfn_frame)
        lower_idx = (ALFN_AM_FRAMES + st->alfn_frame - 1) % ALFN_AM_FRAMES;

    const alfn_t* upper_frame = &st->alfn[st->alfn_upper_idx];
    const alfn_t* lower_frame = &st->alfn[lower_idx];

    if (upper_frame->received != 0xFF ||
        lower_frame->received != 0xFF)
        return;

    const uint16_t upper = upper_frame->value & 0xFFFF;
    uint16_t lower = lower_frame->value & 0xFFFF;

    // If current frame was upper, lower wasn't updated. Assume lower was increased.
    if (st->alfn_upper_idx == st->alfn_frame)
        lower++;

    const uint32_t alfn = (int32_t)upper << 16 | (int32_t)lower;
    nrsc5_report_alfn(st->input->radio, alfn, st->alfn_time_locked);
}

static int find_mod4(const int lower[4])
{
    for (int i = 0; i < 4; i++)
    {
        if (lower[i % 4] == 1 &&
            lower[(i + 1) % 4] == 2 &&
            lower[(i + 2) % 4] == 3)
        {
            return (i + 3) % 4;
        }
    }
    return -1;
}

void pids_complete_am(pids_t* st)
{
    if (st->alfn[st->alfn_frame].received == 0xFF)
    {
        if (st->alfn_frame == 3 && st->alfn_upper_idx == -1)
        {
            const int lower[ALFN_AM_FRAMES] = {
                (int)(st->alfn[0].value & 0x3),
                (int)(st->alfn[1].value & 0x3),
                (int)(st->alfn[2].value & 0x3),
                (int)(st->alfn[3].value & 0x3),
            };
            int finished = 1;
            for (int i = 0; i < ALFN_AM_FRAMES; i++)
            {
                if ((st->alfn[i].received & 0x1) == 0)
                {
                    finished = 0;
                    break;
                }
            }
            if (finished)
                st->alfn_upper_idx = find_mod4(lower);
        }

        if (st->alfn_upper_idx != -1)
            pids_report_alfn_am(st);
    }

    st->alfn_frame = (st->alfn_frame + 1) % 4;
    // Clear for new frame
    st->alfn[st->alfn_frame].value = 0;
    st->alfn[st->alfn_frame].received = 0;
}

void pids_frame_push(pids_t *st, const uint8_t *bits, const unsigned int bc)
{
    uint8_t pids[PIDS_FRAME_LEN];

    for (int i = 0; i < PIDS_FRAME_LEN; i++)
    {
        pids[i] = bits[((i>>3)<<3) + 7 - (i & 7)];
    }

    if (check_crc12(pids))
    {
        const int type = pids[0];

        if (type == PIDS_TYPE_SIS)
            sis_decode(st, pids + 1, bc);
        else if (type == PIDS_TYPE_LLDS)
            log_debug("Ignoring LLDS frame");
    }
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

    st->alfn_frame = 0;
    st->alfn_frame = 0;
    st->alfn_upper_idx = -1;
    for (i = 0; i < ALFN_AM_FRAMES; i++)
    {
        st->alfn[i].value = 0;
        st->alfn[i].received = 0;
    }

    st->input = input;
}
