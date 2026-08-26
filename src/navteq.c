/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "navteq.h"

/*
 * NAVTEQ Traffic frame, derived from 0x2D42AC3E payloads. Fields are numbered
 * from the most significant bit:
 *
 *   bits   width  field
 *    0-2     3    record type
 *    3-6     4    country code
 *    7-9     3    reserved
 *   10-15    6    location table number
 *   16-31   16    location code
 *      32    1    direction
 *   33-37    5    extent
 *      38    1    directionality (stored inverted)
 *      39    1    diversion advised
 *      40    1    reserved (must be zero)
 *      41    1    duration type
 *   42-44    3    ALERT-C control code
 *   45-55   11    ALERT-C event code
 *   56-63    8    encoded ALERT-C quantifier (0/255 means absent)
 */
typedef struct
{
    unsigned int first_bit;
    unsigned int width;
} bit_field_t;

static const struct
{
    bit_field_t message_type;
    bit_field_t is_terminal;
    bit_field_t generation;
} digital_traffic_header_layout = {
    .message_type = { 0, 4 },
    .is_terminal = { 4, 1 },
    .generation = { 5, 3 },
};

static const struct
{
    bit_field_t record_type;
    bit_field_t country_code;
    bit_field_t reserved;
    bit_field_t location_table_number;
    bit_field_t location;
    bit_field_t direction;
    bit_field_t extent;
    bit_field_t inverted_directionality;
    bit_field_t diversion;
    bit_field_t reserved_flag;
    bit_field_t duration_type;
    bit_field_t control_code;
    bit_field_t event;
    bit_field_t quantifier;
} digital_traffic_entry_layout = {
    .record_type = { 0, 3 },
    .country_code = { 3, 4 },
    .reserved = { 7, 3 },
    .location_table_number = { 10, 6 },
    .location = { 16, 16 },
    .direction = { 32, 1 },
    .extent = { 33, 5 },
    .inverted_directionality = { 38, 1 },
    .diversion = { 39, 1 },
    .reserved_flag = { 40, 1 },
    .duration_type = { 41, 1 },
    .control_code = { 42, 3 },
    .event = { 45, 11 },
    .quantifier = { 56, 8 },
};

enum
{
    TRAFFIC_MESSAGE_TYPE = 7,
    TRAFFIC_PRIMARY_RECORD_TYPE = 1,
    TRAFFIC_SECONDARY_RECORD_TYPE = 6,
};

enum
{
    TRAFFIC_HEADER_SIZE = 4,
    TRAFFIC_ENTRY_SIZE = 8,
    TRAFFIC_GROUP_SIZE = 2 * TRAFFIC_ENTRY_SIZE,
};

typedef struct
{
    uint8_t generation;
    int is_terminal;
    unsigned int payload_size;
} digital_traffic_header_t;

static uint32_t read_bit_field(const uint8_t *buf, bit_field_t field)
{
    uint32_t value = 0;

    while (field.width--)
    {
        value = (value << 1)
              | ((buf[field.first_bit / 8] >> (7 - field.first_bit % 8)) & 1);
        field.first_bit++;
    }
    return value;
}

static bool decode_digital_traffic_header(const uint8_t *buf, unsigned int len,
                                          digital_traffic_header_t *header)
{
    if (len < TRAFFIC_HEADER_SIZE
        || read_bit_field(buf, digital_traffic_header_layout.message_type)
           != TRAFFIC_MESSAGE_TYPE)
        return false;

    header->generation = read_bit_field(buf, digital_traffic_header_layout.generation);
    header->is_terminal = read_bit_field(buf, digital_traffic_header_layout.is_terminal);
    header->payload_size = ((unsigned int)buf[2] << 8) | buf[3];
    return header->payload_size == len - TRAFFIC_HEADER_SIZE
        && header->payload_size != 0
        && header->payload_size % TRAFFIC_GROUP_SIZE == 0;
}

static bool digital_traffic_record_types_are_valid(const uint8_t *payload,
                                                   unsigned int payload_size)
{
    unsigned int offset;

    for (offset = 0; offset < payload_size; offset += TRAFFIC_GROUP_SIZE)
    {
        const uint8_t *primary = payload + offset;
        const uint8_t *secondary = primary + TRAFFIC_ENTRY_SIZE;

        if (read_bit_field(primary, digital_traffic_entry_layout.record_type)
                != TRAFFIC_PRIMARY_RECORD_TYPE
            || read_bit_field(secondary, digital_traffic_entry_layout.record_type)
                != TRAFFIC_SECONDARY_RECORD_TYPE
            || read_bit_field(primary, digital_traffic_entry_layout.reserved_flag) != 0
            || read_bit_field(secondary, digital_traffic_entry_layout.reserved_flag) != 0)
            return false;
    }
    return true;
}

static void decode_digital_traffic_entry(const uint8_t *buf,
                                         nrsc5_navteq_digital_traffic_entry_t *entry)
{
    entry->record_type = read_bit_field(buf, digital_traffic_entry_layout.record_type);
    entry->country_code = read_bit_field(buf, digital_traffic_entry_layout.country_code);
    entry->reserved = read_bit_field(buf, digital_traffic_entry_layout.reserved);
    entry->location_table_number = read_bit_field(
        buf, digital_traffic_entry_layout.location_table_number);
    entry->location = read_bit_field(buf, digital_traffic_entry_layout.location);
    entry->direction = read_bit_field(buf, digital_traffic_entry_layout.direction);
    entry->extent = read_bit_field(buf, digital_traffic_entry_layout.extent);
    entry->bidirectional = !read_bit_field(
        buf, digital_traffic_entry_layout.inverted_directionality);
    entry->diversion = read_bit_field(buf, digital_traffic_entry_layout.diversion);
    entry->duration_type = read_bit_field(buf, digital_traffic_entry_layout.duration_type);
    entry->control_code = read_bit_field(buf, digital_traffic_entry_layout.control_code);
    entry->event = read_bit_field(buf, digital_traffic_entry_layout.event);
    entry->quantifier = read_bit_field(buf, digital_traffic_entry_layout.quantifier);
}

bool navteq_decode_digital_traffic(const uint8_t *buf, unsigned int len,
                                   uint8_t *generation, int *is_terminal,
                                   nrsc5_navteq_digital_traffic_entry_t *entries,
                                   unsigned int *count)
{
    digital_traffic_header_t header;
    const uint8_t *payload;
    unsigned int entry_count, capacity, index;

    if (!decode_digital_traffic_header(buf, len, &header))
        return false;
    payload = buf + TRAFFIC_HEADER_SIZE;
    if (!digital_traffic_record_types_are_valid(payload, header.payload_size))
        return false;

    entry_count = header.payload_size / TRAFFIC_ENTRY_SIZE;
    capacity = *count;
    *count = entry_count;
    if (entries != NULL)
    {
        if (capacity < entry_count)
            return false;
        for (index = 0; index < entry_count; index++)
            decode_digital_traffic_entry(payload + index * TRAFFIC_ENTRY_SIZE,
                                         &entries[index]);
    }

    *generation = header.generation;
    *is_terminal = header.is_terminal;
    return true;
}

typedef struct
{
    uint8_t marker;
    uint8_t subtype;
    uint8_t index_and_flags;
    uint8_t frequency_code;
    uint8_t discriminator;
    uint8_t trailing_data[3];
} alternate_frequency_wire_t;

enum
{
    ALTERNATE_FREQUENCY_MARKER = 0x23,
    ALTERNATE_FREQUENCY_SUBTYPE = 0xe0,
    ALTERNATE_FREQUENCY_DISCRIMINATOR = 0x14,
    ALTERNATE_FREQUENCY_BASE_HZ = 87500000,
    ALTERNATE_FREQUENCY_STEP_HZ = 100000,
};

static const bit_field_t alternate_frequency_index = { 0, 4 };

_Static_assert(sizeof(alternate_frequency_wire_t) == 8,
               "NAVTEQ alternate-frequency record must be eight bytes");

static bool alternate_frequency_is_valid(const alternate_frequency_wire_t *wire)
{
    return wire->marker == ALTERNATE_FREQUENCY_MARKER
        && wire->subtype == ALTERNATE_FREQUENCY_SUBTYPE
        && wire->discriminator == ALTERNATE_FREQUENCY_DISCRIMINATOR
        && read_bit_field(&wire->index_and_flags, alternate_frequency_index) != 0;
}

static void decode_alternate_frequency(const alternate_frequency_wire_t *wire,
                                       nrsc5_navteq_alternate_frequency_entry_t *entry)
{
    entry->index = read_bit_field(&wire->index_and_flags, alternate_frequency_index);
    entry->frequency_hz = ALTERNATE_FREQUENCY_BASE_HZ
                        + (uint32_t)wire->frequency_code * ALTERNATE_FREQUENCY_STEP_HZ;
    memcpy(entry->data, wire, sizeof(entry->data));
}

bool navteq_decode_alternate_frequencies(const uint8_t *buf, unsigned int len,
                                         nrsc5_navteq_alternate_frequency_entry_t *entries,
                                         unsigned int *count)
{
    const unsigned int record_size = sizeof(alternate_frequency_wire_t);
    unsigned int entry_count, capacity, index;

    if (len == 0 || len % record_size != 0)
        return false;
    entry_count = len / record_size;
    for (index = 0; index < entry_count; index++)
    {
        alternate_frequency_wire_t wire;

        memcpy(&wire, buf + index * record_size, record_size);
        if (!alternate_frequency_is_valid(&wire))
            return false;
    }

    capacity = *count;
    *count = entry_count;
    if (entries == NULL)
        return true;
    if (capacity < entry_count)
        return false;

    for (index = 0; index < entry_count; index++)
    {
        alternate_frequency_wire_t wire;

        memcpy(&wire, buf + index * record_size, record_size);
        decode_alternate_frequency(&wire, &entries[index]);
    }
    return true;
}
