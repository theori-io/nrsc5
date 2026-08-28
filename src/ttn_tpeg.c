#include "ttn_tpeg.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <zlib.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} cursor_t;

typedef struct component_t {
    uint8_t id;
    const uint8_t *attributes;
    size_t attributes_size;
    struct component_t *children;
    size_t child_count;
} component_t;

static const char *const weather_conditions[84] = {
    "Unknown", "Sunny", "Clear", "Mostly Sunny", "Mostly Clear",
    "Partly Cloudy", "Partly Cloudy", "Mostly Cloudy", "Mostly Cloudy",
    "Cloudy", "Cloudy", "Fog", "Fog", "Clouds and Fog", "Clouds and Fog",
    "Dust", "Dust", "Smoke", "Smoke", "Windy", "Windy", "Frigid", "Frigid",
    "Hot", "Hot", "Rain", "Rain", "Showers", "Showers", "Scattered Showers",
    "Scattered Showers", "Thunderstorms", "Thunderstorms", "Isolated T-Storms",
    "Isolated T-Storms", "Scattered T-Storms", "Scattered T-Storms", "Freezing Rain",
    "Freezing Rain", "Rain and Snow", "Rain and Snow", "Snow", "Snow", "Sleet",
    "Sleet", "Rain and Sleet", "Rain and Sleet", "Wintry Mix", "Wintry Mix",
    "Blizzard", "Blizzard", "Tornado", "Tornado", "Hurricane", "Hurricane",
    "Tropical Storm", "Tropical Storm", "Haze", "Haze", "Blowing Sand",
    "Blowing Sand", "Blowing Snow", "Blowing Snow", "Drizzle", "Drizzle",
    "Heavy Rain", "Heavy Rain", "Strong T-Storms", "Strong T-Storms",
    "Freezing Drizzle", "Freezing Drizzle", "Mixed Rain/Hail", "Mixed Rain/Hail",
    "Hail", "Hail", "Flurries", "Flurries", "Scattered Snow Showers",
    "Scattered Snow Showers", "Snow Showers", "Snow Showers", "Heavy Snow", "Heavy Snow",
    "Not Available"
};

const char *ttn_weather_condition_name(unsigned int condition)
{
    return weather_conditions[condition > 83 ? 83 : condition];
}

static int take(cursor_t *cursor, size_t size, const uint8_t **result)
{
    if (size > cursor->size - cursor->offset)
        return 0;
    *result = cursor->data + cursor->offset;
    cursor->offset += size;
    return 1;
}

static int uint_mb(cursor_t *cursor, uint32_t *result)
{
    uint32_t value = 0;
    const uint8_t *p;

    for (int i = 0; i < 5; i++)
    {
        if (!take(cursor, 1, &p))
            return 0;
        if (value > (UINT32_MAX >> 7))
            return 0;
        value = (value << 7) | (p[0] & 0x7f);
        if (!(p[0] & 0x80))
        {
            *result = value;
            return 1;
        }
    }
    return 0;
}

static int sint_mb(cursor_t *cursor, int32_t *result)
{
    uint32_t value = 0;
    const uint8_t *p;
    int negative;

    if (cursor->offset >= cursor->size)
        return 0;
    negative = (cursor->data[cursor->offset] & 0x40) != 0;
    for (int i = 0; i < 5; i++)
    {
        if (!take(cursor, 1, &p))
            return 0;
        value = (value << 7) | (p[0] & 0x7f);
        if (!(p[0] & 0x80))
        {
            if (negative && i < 4)
                value |= UINT32_MAX << ((i + 1) * 7);
            *result = (int32_t)value;
            return 1;
        }
    }
    return 0;
}

static int byte(cursor_t *cursor, uint8_t *result)
{
    const uint8_t *p;
    if (!take(cursor, 1, &p))
        return 0;
    *result = p[0];
    return 1;
}

static int uint32_be(cursor_t *cursor, uint32_t *result)
{
    const uint8_t *p;
    if (!take(cursor, 4, &p))
        return 0;
    *result = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
            | ((uint32_t)p[2] << 8) | p[3];
    return 1;
}

static int uint16_be(cursor_t *cursor, uint32_t *result)
{
    const uint8_t *p;
    if (!take(cursor, 2, &p))
        return 0;
    *result = ((uint32_t)p[0] << 8) | p[1];
    return 1;
}

static int selector(cursor_t *cursor, uint8_t *flags)
{
    uint8_t value;
    *flags = 0;
    for (int i = 0; i < 5; i++)
    {
        if (!byte(cursor, &value))
            return 0;
        for (int bit = 0; bit < 7; bit++)
            if (value & (1 << (6 - bit)))
                *flags |= (uint8_t)(1 << (i * 7 + bit));
        if (!(value & 0x80))
            return 1;
    }
    return 0;
}

static int component_parse(cursor_t *cursor, component_t *component)
{
    uint32_t length;
    const uint8_t *data;
    cursor_t body;
    uint32_t attributes_size;

    if (!byte(cursor, &component->id) || !uint_mb(cursor, &length)
        || !take(cursor, length, &data))
        return 0;
    body = (cursor_t){ data, length, 0 };
    if (!uint_mb(&body, &attributes_size)
        || !take(&body, attributes_size, &component->attributes))
        return 0;
    component->attributes_size = attributes_size;
    while (body.offset < body.size)
    {
        component_t *children = realloc(component->children,
                                        (component->child_count + 1) * sizeof(*children));
        if (!children)
            return 0;
        component->children = children;
        memset(&children[component->child_count], 0, sizeof(*children));
        if (!component_parse(&body, &children[component->child_count]))
            return 0;
        component->child_count++;
    }
    return 1;
}

static void component_free(component_t *component)
{
    for (size_t i = 0; i < component->child_count; i++)
        component_free(&component->children[i]);
    free(component->children);
}

static int short_string(cursor_t *cursor, char **result)
{
    uint32_t size;
    const uint8_t *data;
    char *string;

    if (!uint_mb(cursor, &size) || !take(cursor, size, &data))
        return 0;
    string = malloc((size_t)size + 1);
    if (!string)
        return 0;
    memcpy(string, data, size);
    string[size] = 0;
    *result = string;
    return 1;
}

static int coordinate(cursor_t *cursor, double *result)
{
    const uint8_t *data;
    int32_t value;
    if (!take(cursor, 3, &data))
        return 0;
    value = (int32_t)((data[0] << 16) | (data[1] << 8) | data[2]);
    if (value & 0x800000)
        value -= 0x1000000;
    *result = value * (360.0 / 16777216.0);
    return 1;
}

static int city_info(cursor_t *cursor, nrsc5_ttn_city_t *city)
{
    uint8_t flags;
    uint32_t provider;

    city->provider_city_id = 0;
    if (!short_string(cursor, (char **)&city->name)
        || !coordinate(cursor, &city->longitude)
        || !coordinate(cursor, &city->latitude)
        || !selector(cursor, &flags))
        return 0;
    if ((flags & 2) && (!uint_mb(cursor, &provider)))
        return 0;
    if (flags & 2)
        city->provider_city_id = provider;
    if (flags & 4)
    {
        /* The extension is not present in recovered payloads. */
        char *extension;
        if (!short_string(cursor, &extension))
            return 0;
        free(extension);
    }
    return 1;
}

static int decode_city_database(const component_t *root,
                                nrsc5_ttn_city_database_t *database)
{
    cursor_t cursor = { root->attributes, root->attributes_size, 0 };
    uint32_t value;
    nrsc5_ttn_city_t *cities;

    if (!uint_mb(&cursor, &value)) return 0;
    if (!uint_mb(&cursor, &value) || !uint32_be(&cursor, &database->timestamp)) return 0;
    if (!uint_mb(&cursor, &database->database_version)) return 0;
    if (!uint_mb(&cursor, &value) || !uint_mb(&cursor, &value)
        || !uint_mb(&cursor, &value)) return 0;
    database->count = 0;
    cities = calloc(root->child_count, sizeof(*cities));
    if (root->child_count && !cities) return 0;
    database->cities = cities;
    for (size_t i = 0; i < root->child_count; i++)
    {
        cursor_t attributes;
        if (root->children[i].id != 16) return 0;
        attributes = (cursor_t){ root->children[i].attributes,
                                 root->children[i].attributes_size, 0 };
        if (!uint_mb(&attributes, &cities[database->count].city_id)
            || !city_info(&attributes, &cities[database->count]))
            return 0;
        database->count++;
    }
    return 1;
}

static int decode_short_forecast(const component_t *component,
                                 nrsc5_ttn_short_forecast_t *forecast)
{
    cursor_t cursor = { component->attributes, component->attributes_size, 0 };
    uint8_t selector_flags;
    int32_t temperature;

    if (!byte(&cursor, (uint8_t *)&forecast->offset_hours)
        || !byte(&cursor, (uint8_t *)&forecast->weather_condition)
        || !byte(&cursor, (uint8_t *)&forecast->wind_direction)
        || !uint_mb(&cursor, &forecast->wind_speed)
        || !sint_mb(&cursor, &temperature)) return 0;
    forecast->temperature = temperature;
    if (!sint_mb(&cursor, &temperature)) return 0;
    forecast->feels_like_temperature = temperature;
    if (!byte(&cursor, (uint8_t *)&forecast->humidity)
        || !selector(&cursor, &selector_flags)) return 0;
    forecast->chance_of_precipitation = 0;
    if ((selector_flags & 1) && !byte(&cursor, (uint8_t *)&forecast->chance_of_precipitation))
        return 0;
    return 1;
}

static int decode_long_forecast(const component_t *component,
                                nrsc5_ttn_long_forecast_t *forecast)
{
    cursor_t cursor = { component->attributes, component->attributes_size, 0 };
    uint8_t selector_flags;
    int32_t value;

    if (!byte(&cursor, (uint8_t *)&forecast->offset_days)
        || !byte(&cursor, (uint8_t *)&forecast->weather_condition)
        || !byte(&cursor, (uint8_t *)&forecast->wind_direction)
        || !uint_mb(&cursor, &forecast->wind_speed)
        || !sint_mb(&cursor, &value)) return 0;
    forecast->high_temperature = value;
    if (!sint_mb(&cursor, &value)) return 0;
    forecast->low_temperature = value;
    if (!byte(&cursor, (uint8_t *)&forecast->chance_of_precipitation)
        || !selector(&cursor, &selector_flags)) return 0;
    forecast->external_feels_like_temperature = 0;
    forecast->maximum_humidity = 0;
    if ((selector_flags & 1) && !sint_mb(&cursor, &value)) return 0;
    if (selector_flags & 1)
        forecast->external_feels_like_temperature = value;
    if ((selector_flags & 2) && !byte(&cursor, (uint8_t *)&forecast->maximum_humidity))
        return 0;
    return 1;
}

static int decode_weather(const component_t *root, unsigned int *timestamp,
                          unsigned int *count, nrsc5_ttn_weather_city_t **result)
{
    cursor_t cursor = { root->attributes, root->attributes_size, 0 };
    uint32_t value, group_count;
    nrsc5_ttn_weather_city_t *cities;

    if (!uint_mb(&cursor, &value) || !uint_mb(&cursor, &value)
        || !uint32_be(&cursor, timestamp) || !uint_mb(&cursor, &value)
        || !uint_mb(&cursor, &value) || !uint_mb(&cursor, &group_count)) return 0;
    for (uint32_t i = 0; i < group_count; i++)
        if (!uint_mb(&cursor, &value) || !uint_mb(&cursor, &value) || !uint_mb(&cursor, &value))
            return 0;
    cities = calloc(root->child_count, sizeof(*cities));
    if (root->child_count && !cities) return 0;
    *count = 0;
    for (size_t i = 0; i < root->child_count; i++)
    {
        const component_t *forecast_component = &root->children[i];
        cursor_t attributes;
        uint8_t selector_flags;
        if (forecast_component->id != 21) return 0;
        attributes = (cursor_t){ forecast_component->attributes,
                                 forecast_component->attributes_size, 0 };
        uint32_t city_id, database_version;
        if (!byte(&attributes, (uint8_t *)&cities[*count].scope)
            || !uint_mb(&attributes, &city_id)
            || !uint_mb(&attributes, &database_version)
            || !byte(&attributes, (uint8_t *)&cities[*count].current_day)
            || !selector(&attributes, &selector_flags)) return 0;
        cities[*count].city.city_id = city_id;
        cities[*count].city_database_version = database_version;
        cities[*count].city.provider_city_id = 0;
        cities[*count].city.name = NULL;
        if (selector_flags & 2 && !city_info(&attributes, &cities[*count].city)) return 0;
        cities[*count].short_forecasts = calloc(forecast_component->child_count,
                                                sizeof(*cities[*count].short_forecasts));
        cities[*count].long_forecasts = calloc(forecast_component->child_count,
                                               sizeof(*cities[*count].long_forecasts));
        if ((forecast_component->child_count && !cities[*count].short_forecasts)
            || (forecast_component->child_count && !cities[*count].long_forecasts)) return 0;
        for (size_t j = 0; j < forecast_component->child_count; j++)
        {
            const component_t *child = &forecast_component->children[j];
            if (child->id == 22)
            {
                if (!decode_short_forecast(child, (nrsc5_ttn_short_forecast_t *)
                                           &cities[*count].short_forecasts[cities[*count].short_forecast_count++])) return 0;
            }
            else if (child->id == 23)
            {
                if (!decode_long_forecast(child, (nrsc5_ttn_long_forecast_t *)
                                          &cities[*count].long_forecasts[cities[*count].long_forecast_count++])) return 0;
            }
        }
        (*count)++;
    }
    *result = cities;
    return 1;
}

static int decode_service_network(const component_t *root,
                                  nrsc5_ttn_service_network_t *result)
{
    cursor_t cursor = { root->attributes, root->attributes_size, 0 };
    uint32_t id, count;
    uint8_t flags;

    memset(result, 0, sizeof(*result));
    if (!uint_mb(&cursor, &id) || !uint32_be(&cursor, &result->timestamp)
        || !selector(&cursor, &flags))
        return 0;
    result->service_component_id_raw = id;
    result->service_component_id = id & 0xff;
    if ((flags & 1) && !short_string(&cursor, (char **)&result->service_provider_name))
        return 0;
    if (!(flags & 2))
        return cursor.offset == cursor.size;
    if (!uint_mb(&cursor, &count))
        return 0;
    result->bearers = calloc(count, sizeof(*result->bearers));
    if (count && !result->bearers)
        return 0;
    result->bearer_count = count;
    for (uint32_t i = 0; i < count; i++)
    {
        nrsc5_ttn_bearer_t *bearer = (nrsc5_ttn_bearer_t *)&result->bearers[i];
        if (!uint32_be(&cursor, &bearer->station_id) || !selector(&cursor, &flags))
            return 0;
        if ((flags & 1) && !uint_mb(&cursor, &count)) return 0;
        if (flags & 1)
        {
            bearer->fm_links = calloc(count, sizeof(*bearer->fm_links));
            if (count && !bearer->fm_links) return 0;
            bearer->fm_link_count = count;
            for (uint32_t j = 0; j < count; j++)
                if (!uint32_be(&cursor, &((nrsc5_ttn_bearer_link_t *)bearer->fm_links)[j].station_id)
                    || !byte(&cursor, (uint8_t *)&((nrsc5_ttn_bearer_link_t *)bearer->fm_links)[j].frequency_selector))
                    return 0;
        }
        if ((flags & 2) && !uint_mb(&cursor, &count)) return 0;
        if (flags & 2)
        {
            bearer->am_links = calloc(count, sizeof(*bearer->am_links));
            if (count && !bearer->am_links) return 0;
            bearer->am_link_count = count;
            for (uint32_t j = 0; j < count; j++)
                if (!uint32_be(&cursor, &((nrsc5_ttn_bearer_link_t *)bearer->am_links)[j].station_id)
                    || !byte(&cursor, (uint8_t *)&((nrsc5_ttn_bearer_link_t *)bearer->am_links)[j].frequency_selector))
                    return 0;
        }
    }
    return cursor.offset == cursor.size;
}

static void free_service_network(nrsc5_ttn_service_network_t *info)
{
    for (unsigned int i = 0; i < info->bearer_count; i++)
    {
        free((void *)info->bearers[i].fm_links);
        free((void *)info->bearers[i].am_links);
    }
    free((void *)info->bearers);
    free((void *)info->service_provider_name);
}

static void free_city_database(nrsc5_ttn_city_database_t *database)
{
    for (unsigned int i = 0; i < database->count; i++)
        free((void *)database->cities[i].name);
    free((void *)database->cities);
}

static void free_weather(unsigned int count, nrsc5_ttn_weather_city_t *cities)
{
    for (unsigned int i = 0; i < count; i++)
    {
        free((void *)cities[i].city.name);
        free((void *)cities[i].short_forecasts);
        free((void *)cities[i].long_forecasts);
    }
    free(cities);
}

static int decode_tmc_location(const component_t *component,
                               nrsc5_ttn_tec_t *event,
                               nrsc5_location_table_t *locations)
{
    cursor_t cursor = { component->attributes, component->attributes_size, 0 };
    uint32_t value;
    uint8_t flags;

    if (!uint16_be(&cursor, &value)
        || !byte(&cursor, &event->country_code)
        || !byte(&cursor, &event->location_table_number)
        || !selector(&cursor, &flags))
        return 0;
    event->location = (uint16_t)value;
    event->direction_positive = (flags & 1) != 0;
    event->both_directions = (flags & 2) != 0;
    event->extent = 0;
    if ((flags & 4) && !uint_mb(&cursor, &event->extent))
        return 0;
    /* Extended country/table version fields are deliberately ignored here. */
    if (flags & 8) { if (!byte(&cursor, (uint8_t *)&value)) return 0; }
    if (flags & 16) { if (!uint_mb(&cursor, &value)) return 0; }
    if ((flags & 32) && component->child_count == 0) return 0;
    if (locations)
    {
        int found = nrsc5_location_table_lookup(locations, event->country_code,
                                                 event->location_table_number,
                                                 event->location,
                                                 &event->resolved_location);
        event->location_resolved = found == 1;
    }
    return 1;
}

static int decode_tec_message(const component_t *root, nrsc5_ttn_tec_t *event,
                              nrsc5_location_table_t *locations)
{
    memset(event, 0, sizeof(*event));
    for (size_t i = 0; i < root->child_count; i++)
    {
        const component_t *child = &root->children[i];
        cursor_t cursor = { child->attributes, child->attributes_size, 0 };
        uint8_t flags;
        uint32_t value;

        if (child->id == 1)
        {
            if (!uint_mb(&cursor, &value) || !byte(&cursor, (uint8_t *)&event->version)
                || !uint32_be(&cursor, &event->expiry_time) || !selector(&cursor, &flags))
                return 0;
            event->message_id = value;
            event->cancel = (flags & 1) != 0;
        }
        else if (child->id == 3)
        {
            if (!uint_mb(&cursor, &event->effect_code) || !selector(&cursor, &flags))
                return 0;
            for (size_t j = 0; j < child->child_count; j++)
                if (child->children[j].id == 4)
                {
                    cursor_t cause = { child->children[j].attributes,
                                       child->children[j].attributes_size, 0 };
                    if (!uint_mb(&cause, &event->cause_code)
                        || !uint_mb(&cause, &value)) return 0;
                    event->warning_level = value;
                    break;
                }
        }
        else if (child->id == 2)
            for (size_t j = 0; j < child->child_count; j++)
                if (child->children[j].id == 2)
                    decode_tmc_location(&child->children[j], event, locations);
    }
    return event->message_id != 0;
}

static int decode_tec_components(const uint8_t *data, size_t size,
                                 nrsc5_location_table_t *locations,
                                 ttn_tpeg1_tec_cb callback, void *opaque)
{
    cursor_t cursor = { data, size, 0 };
    int seen = 0;
    while (cursor.offset + 5 <= cursor.size)
    {
        uint32_t length;
        uint32_t header_crc;
        const uint8_t *body;
        component_t root = {0};
        size_t frame_end;

        if (cursor.data[cursor.offset] == 0xff && cursor.data[cursor.offset + 1] == 0x0f)
            break;
        if (!byte(&cursor, &root.id) || !uint16_be(&cursor, &length)
            || !uint16_be(&cursor, &header_crc)
            || !take(&cursor, length, &body))
            break;
        (void)header_crc;
        frame_end = cursor.offset;
        /* A component-frame header is followed by priority/count and CRC. */
        cursor_t frame = { body, length, 0 };
        if (length < 3) continue;
        frame.offset = 2;
        while (frame.offset + 2 < frame.size)
        {
            component_t candidate = {0};
            size_t start = frame.offset;
            if (component_parse(&frame, &candidate) && candidate.id == 0)
            {
                nrsc5_ttn_tec_t event = {0};
                /* Unknown optional TEC fields must not hide the message. */
                if (decode_tec_message(&candidate, &event, locations))
                {
                    if (callback) callback(&event, opaque);
                    seen++;
                }
            }
            component_free(&candidate);
            if (frame.offset <= start) break;
            frame.offset = start + 1;
        }
        cursor.offset = frame_end;
    }
    return seen;
}

int ttn_tpeg1_decode(const uint8_t *payload, size_t payload_size,
                     nrsc5_location_table_t *locations,
                     ttn_tpeg1_tec_cb tec, void *opaque)
{
    uint8_t *inflated = NULL;
    size_t inflated_size = 0;
    const uint8_t *data = payload;
    size_t size = payload_size;
    int result;

    if (!payload || payload_size < 4) return 1;
    if (payload_size >= 4 && memcmp(payload, "TTN\xff", 4) == 0)
    {
        if (ttn_tpeg2_inflate(payload, payload_size, &inflated, &inflated_size) != 0)
            return 1;
        data = inflated;
        size = inflated_size;
    }
    else if (payload_size >= 4)
    {
        /* TPEG-1 service data starts with SID[3] and ServEncID. */
        data += 4;
        size -= 4;
    }
    result = decode_tec_components(data, size, locations, tec, opaque);
    free(inflated);
    return result > 0 ? 0 : 1;
}

int ttn_tpeg2_decode(const uint8_t *payload, size_t payload_size,
                     ttn_tpeg2_city_database_cb city_database,
                     ttn_tpeg2_weather_cb weather,
                     ttn_tpeg2_service_network_cb service_network, void *opaque)
{
    cursor_t cursor;
    component_t root = {0};
    uint8_t *inflated = NULL;
    size_t inflated_size = 0;
    int result = 1;

    if (ttn_tpeg2_inflate(payload, payload_size, &inflated, &inflated_size) != 0)
        return 1;
    cursor = (cursor_t){ inflated, inflated_size, 0 };
    if (!component_parse(&cursor, &root) || cursor.offset != cursor.size)
        goto done;
    if (root.id == 0)
    {
        nrsc5_ttn_service_network_t info;
        if (!decode_service_network(&root, &info))
            goto done;
        if (service_network)
            service_network(&info, opaque);
        free_service_network(&info);
        result = 0;
    }
    else if (root.id == 6)
    {
        nrsc5_ttn_city_database_t database = {0};
        if (!decode_city_database(&root, &database))
        {
            free_city_database(&database);
            goto done;
        }
        if (city_database)
            city_database(&database, opaque);
        free_city_database(&database);
        result = 0;
    }
    else if (root.id == 2)
    {
        unsigned int timestamp, count = 0;
        nrsc5_ttn_weather_city_t *cities = NULL;
        if (!decode_weather(&root, &timestamp, &count, &cities))
        {
            free_weather(count, cities);
            goto done;
        }
        if (weather)
            weather(timestamp, count, cities, opaque);
        free_weather(count, cities);
        result = 0;
    }
done:
    component_free(&root);
    free(inflated);
    return result;
}

static const uint8_t ttn_dictionary_data[] = {
    0x54,0x4f,0x57,0x4e,0x4f,0x49,0x4c,0x48,0x57,0x59,0x41,0x56,0x45,0x53,0x54,0x41,
    0x54,0x49,0x4f,0x4e,0x53,0x54,0x41,0x54,0x45,0x20,0x52,0x4f,0x55,0x54,0x45,0x04,
    0x06,0x0f,0x0e,0x40,0x01,0x01,0x40,0x16,0x16,0x09,0x08,0x00,0x60,0x03,0x02,0x02,
    0x2d,0x32,0x1c,0x4e,0x6f,0x72,0x74,0x68,0x53,0x6f,0x75,0x74,0x68,0x45,0x61,0x73,
    0x74,0x57,0x65,0x73,0x74,0x53,0x74,0x2e,0x41,0x74,0x6c,0x61,0x6e,0x74,0x69,0x63,
    0x54,0x65,0x78,0x61,0x73,0x4e,0x65,0x77,0x20,0x59,0x6f,0x72,0x6b,0x43,0x6f,0x6e,
    0x66,0x65,0x72,0x65,0x6e,0x63,0x65,0x01,0x00,0x18,0x11,0x10,0x01,0x81,0x54,0x72,
    0x75,0x73,0x74,0x20,0x46,0x75,0x6e,0x64,0x49,0x6e,0x63,0x2e,0x43,0x6f,0x72,0x70,
    0x49,0x6e,0x64,0x65,0x78,0x47,0x6c,0x6f,0x62,0x61,0x6c,0x43,0x61,0x70,0x69,0x74,
    0x61,0x6c,0x46,0x69,0x6e,0x61,0x6e,0x63,0x69,0x61,0x6c,0x49,0x6e,0x63,0x6f,0x6d,
    0x65,0x53,0x65,0x72,0x76,0x69,0x63,0x65,0x73,0x49,0x6e,0x74,0x65,0x72,0x6e,0x61,
    0x74,0x69,0x6f,0x6e,0x61,0x6c,0x46,0x69,0x72,0x73,0x74,0x02,0x00,0x00,0x00,0x05,
    0x03,0x00,0x00,0x00,0x07,0x04,0x00,0x00,0x00,0x08,0x02,0x0a,0x00,0x02,0x07,0x06,
    0x60,0x01,0x6c,0x00,0x01,0x4e,0xaf,0x01,0x04,0x60,0x01,0x03,0x60,0xaf,0x4e,0xaf,
    0x00,0x02,0x07,0x02,0x0a,0x00,0x10,0xc6,0x00,0x4e,0xaf,0x0d,0x4d,0x00,0x01,0x00,
    0x07,0x09
};

typedef struct
{
    uint32_t adler32;
    const uint8_t *data;
    size_t size;
} ttn_dictionary_t;

static const ttn_dictionary_t ttn_dictionaries[] = {
    { 0xc3f64537, ttn_dictionary_data, sizeof(ttn_dictionary_data) },
};

static const ttn_dictionary_t *find_dictionary(uint32_t adler32)
{
    for (size_t i = 0; i < sizeof(ttn_dictionaries) / sizeof(ttn_dictionaries[0]); i++)
        if (ttn_dictionaries[i].adler32 == adler32)
            return &ttn_dictionaries[i];
    return NULL;
}

int ttn_tpeg2_inflate(const uint8_t *payload, size_t payload_size,
                      uint8_t **output, size_t *output_size)
{
    z_stream stream = {0};
    size_t capacity = payload_size < 4096 ? 4096 : payload_size * 2;
    uint8_t *buffer;
    int result;
    const ttn_dictionary_t *dictionary = NULL;

    size_t input_offset = 0;

    if (!payload || !output || !output_size || payload_size < 2)
        return 1;
    while (input_offset + 1 < payload_size)
    {
        uint16_t header = ((uint16_t)payload[input_offset] << 8)
                        | payload[input_offset + 1];
        if ((payload[input_offset] & 0x0f) == 8 && (payload[input_offset] >> 4) <= 7
            && header % 31 == 0)
            break;
        input_offset++;
    }
    if (input_offset + 1 >= payload_size)
        return 1;
    if (payload[input_offset + 1] & 0x20)
    {
        uint32_t adler32;
        if (payload_size - input_offset < 6)
            return 1;
        adler32 = ((uint32_t)payload[input_offset + 2] << 24)
                | ((uint32_t)payload[input_offset + 3] << 16)
                | ((uint32_t)payload[input_offset + 4] << 8)
                | payload[input_offset + 5];
        dictionary = find_dictionary(adler32);
        if (!dictionary)
            return 1;
    }
    buffer = malloc(capacity);
    if (!buffer)
        return 1;
    stream.next_in = (Bytef *)(payload + input_offset);
    stream.avail_in = (uInt)(payload_size - input_offset);
    result = inflateInit2(&stream, 15);
    if (result != Z_OK)
        goto error;
    stream.next_out = buffer;
    stream.avail_out = (uInt)capacity;
    for (;;) {
        result = inflate(&stream, Z_NO_FLUSH);
        if (result == Z_NEED_DICT)
            result = dictionary
                   ? inflateSetDictionary(&stream, dictionary->data, dictionary->size)
                   : Z_DATA_ERROR;
        if (result == Z_STREAM_END)
            break;
        if (result != Z_OK)
            goto error_end;
        if (stream.avail_out != 0)
            continue;
        {
            size_t used = stream.total_out;
        uint8_t *grown;
        capacity *= 2;
        grown = realloc(buffer, capacity);
        if (!grown)
            goto error_end;
        buffer = grown;
        stream.next_out = buffer + used;
        stream.avail_out = (uInt)(capacity - used);
        }
    }
    *output = buffer;
    *output_size = stream.total_out;
    inflateEnd(&stream);
    return 0;

error_end:
    inflateEnd(&stream);
error:
    free(buffer);
    return 1;
}
