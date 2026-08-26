/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>

#include <nrsc5.h>

typedef struct
{
    const char *name;
    const char *quantified_name;
    uint8_t quantifier_type;
} alert_c_event_t;

// Quantifier types and code-to-value mappings follow ISO 14819-2:2003,
// section 3.1.2, Table 1: https://www.iso.org/standard/29536.html
enum
{
    ALERT_C_QUANTIFIER_SMALL_NUMBER = 0,
    ALERT_C_QUANTIFIER_NUMBER = 1,
    ALERT_C_QUANTIFIER_LESS_THAN_METRES = 2,
    ALERT_C_QUANTIFIER_PERCENT = 3,
    ALERT_C_QUANTIFIER_UP_TO_KMH = 4,
    ALERT_C_QUANTIFIER_UP_TO_TIME = 5,
    ALERT_C_QUANTIFIER_DEGREES_CELSIUS = 6,
    ALERT_C_QUANTIFIER_TIME = 7,
    ALERT_C_QUANTIFIER_TONNES = 8,
    ALERT_C_QUANTIFIER_METRES = 9,
    ALERT_C_QUANTIFIER_UP_TO_MILLIMETRES = 10,
    ALERT_C_QUANTIFIER_MHZ = 11,
    ALERT_C_QUANTIFIER_KHZ = 12,
};

// Generated from the OpenStreetMap TMC/Event Code List.
static const alert_c_event_t events[2048] = {
#include "alert_c_events.inc"
};

static const alert_c_event_t unknown_event = { "Unknown", NULL, 0 };

static const alert_c_event_t *find_event(unsigned int event)
{
    return event < sizeof(events) / sizeof(events[0]) && events[event].name
         ? &events[event] : &unknown_event;
}

static unsigned int normalize_small_quantifier(unsigned int value)
{
    // Types 0-5 occupy five bits. In that encoding zero represents 32.
    value &= 0x1f;
    return value == 0 ? 32 : value;
}

static unsigned int decode_small_number(unsigned int value)
{
    static const unsigned int tail[] = { 30, 32, 34, 36 };

    // The final four codes skip odd values to extend the five-bit range.
    return value > 28 ? tail[value - 29] : value;
}

static int format_quantifier(uint8_t type, unsigned int value, char *text, size_t size,
                             int *is_singular)
{
    unsigned int number, tenths;

    *is_singular = 0;
    if (type <= ALERT_C_QUANTIFIER_UP_TO_TIME)
        value = normalize_small_quantifier(value);

    switch (type)
    {
    case ALERT_C_QUANTIFIER_SMALL_NUMBER:
        value = decode_small_number(value);
        *is_singular = value == 1;
        snprintf(text, size, "%u", value);
        break;
    case ALERT_C_QUANTIFIER_NUMBER:
        // Use units through 4, tens through 100, then steps of 50 through 1000.
        if (value <= 4)
            number = value;
        else if (value <= 14)
            number = (value - 4) * 10;
        else
            number = (value - 12) * 50;
        *is_singular = number == 1;
        snprintf(text, size, "%u", number);
        break;
    case ALERT_C_QUANTIFIER_LESS_THAN_METRES:
        snprintf(text, size, "less than %u metres", value * 10);
        break;
    case ALERT_C_QUANTIFIER_PERCENT:
        snprintf(text, size, "%u %%", value == 32 ? 0 : value * 5);
        break;
    case ALERT_C_QUANTIFIER_UP_TO_KMH:
        snprintf(text, size, "of up to %u km/h", value * 5);
        break;
    case ALERT_C_QUANTIFIER_UP_TO_TIME:
        // Codes progress from five-minute intervals to one- and six-hour intervals.
        if (value <= 10)
            snprintf(text, size, "of up to %u minutes", value * 5);
        else if (value <= 22)
            snprintf(text, size, "of up to %u hours", value - 10);
        else
            snprintf(text, size, "of up to %u hours", (value - 20) * 6);
        break;
    case ALERT_C_QUANTIFIER_DEGREES_CELSIUS:
        // Code 51 is 0 degrees Celsius.
        snprintf(text, size, "%d degrees Celsius", (int)value - 51);
        break;
    case ALERT_C_QUANTIFIER_TIME:
        // Code 1 is midnight; subsequent codes advance in ten-minute intervals.
        value = (value - 1) * 10;
        snprintf(text, size, "%02u:%02u", value / 60, value % 60);
        break;
    case ALERT_C_QUANTIFIER_TONNES:
    case ALERT_C_QUANTIFIER_METRES:
        // Values use 0.1-unit steps through 10.0, then 0.5-unit steps.
        tenths = value <= 100 ? value : 100 + (value - 100) * 5;
        snprintf(text, size, "%u.%u %s", tenths / 10, tenths % 10,
                 type == ALERT_C_QUANTIFIER_TONNES ? "tonnes" : "metres");
        break;
    case ALERT_C_QUANTIFIER_UP_TO_MILLIMETRES:
        snprintf(text, size, "of up to %u millimetres", value);
        break;
    case ALERT_C_QUANTIFIER_MHZ:
        // FM code 1 represents 87.6 MHz, increasing in 0.1 MHz steps.
        tenths = 875 + value;
        snprintf(text, size, "%u.%u MHz", tenths / 10, tenths % 10);
        break;
    case ALERT_C_QUANTIFIER_KHZ:
        // LF codes 1-15 start at 153 kHz; MF codes start at 531 kHz.
        value = value <= 15 ? 144 + 9 * value : 522 + 9 * (value - 15);
        snprintf(text, size, "%u kHz", value);
        break;
    default:
        return 0;
    }
    return 1;
}

static void format_ordinal(unsigned int value, char *text, size_t size, int *is_singular)
{
    const char *suffix = "th";

    value = decode_small_number(normalize_small_quantifier(value));
    // English ordinals 11, 12, and 13 use "th" despite their final digit.
    if (value % 100 < 11 || value % 100 > 13)
    {
        if (value % 10 == 1)
            suffix = "st";
        else if (value % 10 == 2)
            suffix = "nd";
        else if (value % 10 == 3)
            suffix = "rd";
    }
    *is_singular = value == 1;
    snprintf(text, size, "%u%s", value, suffix);
}

static void resolve_inflections(const char *source, int is_singular,
                                char *destination, size_t size)
{
    const char *replacement;
    size_t marker_size;

    if (size == 0)
        return;
    // The source OSM event list uses forms such as accident(s) and lorr(y/ies):
    // https://wiki.openstreetmap.org/wiki/TMC/Event_Code_List
    while (*source && size > 1)
    {
        replacement = NULL;
        marker_size = 0;
        if (strncmp(source, "(y/ies)", 7) == 0)
        {
            replacement = is_singular ? "y" : "ies";
            marker_size = 7;
        }
        else if (strncmp(source, "(es)", 4) == 0)
        {
            replacement = is_singular ? "" : "es";
            marker_size = 4;
        }
        else if (strncmp(source, "(s)", 3) == 0)
        {
            replacement = is_singular ? "" : "s";
            marker_size = 3;
        }
        if (replacement != NULL)
        {
            while (*replacement && size > 1)
            {
                *destination++ = *replacement++;
                size--;
            }
            source += marker_size;
        }
        else
        {
            *destination++ = *source++;
            size--;
        }
    }
    *destination = 0;
}

void nrsc5_alert_c_event_name(unsigned int event, const char **name)
{
    *name = find_event(event)->name;
}

void nrsc5_alert_c_event_description(unsigned int event, unsigned int quantifier,
                                     char *description, size_t description_size)
{
    const alert_c_event_t *entry = find_event(event);
    const char *marker;
    char quantified[512];
    char value[64];
    int is_singular;
    size_t marker_size;

    if (description_size == 0)
        return;
    if (quantifier == 0 || quantifier >= UINT8_MAX || entry->quantified_name == NULL)
    {
        snprintf(description, description_size, "%s", entry->name);
        return;
    }

    // Ordinal templates use a distinct marker so 1, 2, 3 become 1st, 2nd, 3rd.
    marker = strstr(entry->quantified_name, "(Qth)");
    if (marker != NULL)
    {
        format_ordinal(quantifier, value, sizeof(value), &is_singular);
        marker_size = 5;
    }
    else
    {
        marker = strstr(entry->quantified_name, "(Q)");
        if (marker == NULL
            || !format_quantifier(entry->quantifier_type, quantifier, value, sizeof(value),
                                  &is_singular))
        {
            snprintf(description, description_size, "%s", entry->name);
            return;
        }
        marker_size = 3;
    }
    // Replace the one quantifier marker, then resolve singular/plural markers.
    snprintf(quantified, sizeof(quantified), "%.*s%s%s",
             (int)(marker - entry->quantified_name), entry->quantified_name,
             value, marker + marker_size);
    resolve_inflections(quantified, is_singular, description, description_size);
}
