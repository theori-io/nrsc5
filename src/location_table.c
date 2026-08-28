/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <xlocale.h>
#endif

#include <nrsc5.h>

typedef struct
{
    uint8_t country;
    uint8_t ltn;
    uint16_t location;
    uint16_t positive;
    uint16_t negative;
    double latitude;
    double longitude;
    char *name;
} location_table_entry_t;

struct nrsc5_location_table_t
{
    location_table_entry_t *entries;
    size_t count;
};

#ifdef _WIN32
typedef _locale_t numeric_locale_t;

static numeric_locale_t create_numeric_locale(void)
{
    return _create_locale(LC_NUMERIC, "C");
}

static void destroy_numeric_locale(numeric_locale_t locale)
{
    _free_locale(locale);
}

static double parse_locale_double(const char *text, char **end, numeric_locale_t locale)
{
    return _strtod_l(text, end, locale);
}
#else
typedef locale_t numeric_locale_t;

static numeric_locale_t create_numeric_locale(void)
{
    return newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

static void destroy_numeric_locale(numeric_locale_t locale)
{
    freelocale(locale);
}

static double parse_locale_double(const char *text, char **end, numeric_locale_t locale)
{
    return strtod_l(text, end, locale);
}
#endif

static int compare_entries(const void *left, const void *right)
{
    const location_table_entry_t *a = left;
    const location_table_entry_t *b = right;

    if (a->country != b->country)
        return a->country < b->country ? -1 : 1;
    if (a->ltn != b->ltn)
        return a->ltn < b->ltn ? -1 : 1;
    if (a->location != b->location)
        return a->location < b->location ? -1 : 1;
    return 0;
}

static int parse_uint(const char *text, unsigned long maximum, unsigned long *value)
{
    char *end;
    unsigned long result;

    errno = 0;
    result = strtoul(text, &end, 10);
    if (errno != 0 || text == end || *end != 0 || result > maximum)
        return 0;
    *value = result;
    return 1;
}

static int parse_double(const char *text, double minimum, double maximum,
                        numeric_locale_t locale, double *value)
{
    char *end;
    double result;

    errno = 0;
    result = parse_locale_double(text, &end, locale);
    if (errno != 0 || text == end || *end != 0 || !isfinite(result)
        || result < minimum || result > maximum)
        return 0;
    *value = result;
    return 1;
}

enum location_table_field
{
    FIELD_COUNTRY,
    FIELD_LTN,
    FIELD_LOCATION,
    FIELD_POSITIVE,
    FIELD_NEGATIVE,
    FIELD_LATITUDE,
    FIELD_LONGITUDE,
    FIELD_NAME,
    FIELD_COUNT
};

enum
{
    NAVTEQ_COUNTRY_CODE_MAX = (1U << 4) - 1,
    NAVTEQ_LOCATION_TABLE_NUMBER_MAX = (1U << 6) - 1,
    LOCATION_TABLE_LINE_SIZE = 4096,
    LOCATION_TABLE_INITIAL_CAPACITY = 1024,
};

static int parse_entry(char *line, numeric_locale_t locale,
                       location_table_entry_t *entry)
{
    char *fields[FIELD_COUNT];
    char *separator;
    unsigned long country, ltn, location, positive, negative;
    unsigned int i;

    fields[FIELD_COUNTRY] = line;
    for (i = 1; i < FIELD_COUNT; i++)
    {
        separator = strchr(fields[i - 1], '\t');
        if (separator == NULL)
            return 0;
        *separator = 0;
        fields[i] = separator + 1;
    }
    if (strchr(fields[FIELD_NAME], '\t') != NULL
        || !parse_uint(fields[FIELD_COUNTRY], NAVTEQ_COUNTRY_CODE_MAX, &country)
        || !parse_uint(fields[FIELD_LTN], NAVTEQ_LOCATION_TABLE_NUMBER_MAX, &ltn)
        || !parse_uint(fields[FIELD_LOCATION], UINT16_MAX, &location)
        || !parse_uint(fields[FIELD_POSITIVE], UINT16_MAX, &positive)
        || !parse_uint(fields[FIELD_NEGATIVE], UINT16_MAX, &negative)
        || !parse_double(fields[FIELD_LATITUDE], -90.0, 90.0,
                         locale, &entry->latitude)
        || !parse_double(fields[FIELD_LONGITUDE], -180.0, 180.0,
                         locale, &entry->longitude))
        return 0;

    entry->country = country;
    entry->ltn = ltn;
    entry->location = location;
    entry->positive = positive;
    entry->negative = negative;
    entry->name = strdup(fields[FIELD_NAME]);
    return entry->name != NULL;
}

int nrsc5_location_table_open(nrsc5_location_table_t **result, const char *path)
{
    static const char header[]
        = "country\tltn\tlocation\tpositive\tnegative\tlatitude\tlongitude\tname";
    nrsc5_location_table_t *table = NULL;
    FILE *file = NULL;
    numeric_locale_t locale = (numeric_locale_t)0;
    char line[LOCATION_TABLE_LINE_SIZE];
    size_t capacity = 0;

    if (result == NULL || path == NULL)
    {
        errno = EINVAL;
        return 1;
    }
    *result = NULL;
    file = fopen(path, "rb");
    if (file == NULL)
        return 1;
    table = calloc(1, sizeof(*table));
    if (table == NULL)
        goto fail;
    locale = create_numeric_locale();
    if (locale == (numeric_locale_t)0)
        goto fail;

    if (fgets(line, sizeof(line), file) == NULL)
        goto invalid;
    line[strcspn(line, "\r\n")] = 0;
    if (strcmp(line, header) != 0)
        goto invalid;

    while (fgets(line, sizeof(line), file) != NULL)
    {
        size_t length = strlen(line);
        location_table_entry_t *entries;

        if (length == 0 || (line[length - 1] != '\n' && !feof(file)))
            goto invalid;
        if (line[length - 1] == '\n')
            line[--length] = 0;
        if (length > 0 && line[length - 1] == '\r')
            line[--length] = 0;
        if (strchr(line, '\r') != NULL || strchr(line, '\n') != NULL)
            goto invalid;
        if (table->count == capacity)
        {
            if (capacity > SIZE_MAX / 2 / sizeof(*table->entries))
            {
                errno = ENOMEM;
                goto fail;
            }
            capacity = capacity == 0 ? LOCATION_TABLE_INITIAL_CAPACITY : capacity * 2;
            entries = realloc(table->entries, capacity * sizeof(*entries));
            if (entries == NULL)
                goto fail;
            table->entries = entries;
        }
        memset(&table->entries[table->count], 0, sizeof(*table->entries));
        if (!parse_entry(line, locale, &table->entries[table->count]))
            goto invalid;
        table->count++;
    }
    if (ferror(file))
        goto fail;
    fclose(file);
    file = NULL;
    destroy_numeric_locale(locale);
    locale = (numeric_locale_t)0;
    if (table->count == 0)
        goto invalid;

    qsort(table->entries, table->count, sizeof(*table->entries), compare_entries);
    for (size_t i = 1; i < table->count; i++)
    {
        if (compare_entries(&table->entries[i - 1], &table->entries[i]) == 0)
            goto invalid;
    }
    *result = table;
    return 0;

invalid:
    errno = EINVAL;
fail:
    if (locale != (numeric_locale_t)0)
        destroy_numeric_locale(locale);
    if (file != NULL)
        fclose(file);
    nrsc5_location_table_close(table);
    return 1;
}

void nrsc5_location_table_close(nrsc5_location_table_t *table)
{
    if (table == NULL)
        return;
    for (size_t i = 0; i < table->count; i++)
        free(table->entries[i].name);
    free(table->entries);
    free(table);
}

int nrsc5_location_table_lookup(nrsc5_location_table_t *table, uint8_t country,
                                uint8_t ltn, uint16_t location,
                                nrsc5_location_t *result)
{
    location_table_entry_t key = {country, ltn, location, 0, 0, 0, 0, NULL};
    location_table_entry_t *entry;

    if (table == NULL || result == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    entry = bsearch(&key, table->entries, table->count,
                    sizeof(*table->entries), compare_entries);
    if (entry == NULL)
        return 0;

    memset(result, 0, sizeof(*result));
    result->location = entry->location;
    result->positive = entry->positive;
    result->negative = entry->negative;
    result->latitude = entry->latitude;
    result->longitude = entry->longitude;
    strncpy(result->name, entry->name, sizeof(result->name) - 1);
    return 1;
}
