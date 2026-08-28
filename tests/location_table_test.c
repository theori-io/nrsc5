#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <nrsc5.h>

#define CHECK(condition) do { if (!(condition)) return 1; } while (0)

int main(void)
{
    nrsc5_location_table_t *table = (nrsc5_location_table_t *)1;
    nrsc5_location_t location;
    char original_locale[128];
    const char *locale;

    locale = setlocale(LC_NUMERIC, NULL);
    CHECK(locale != NULL && strlen(locale) < sizeof(original_locale));
    strcpy(original_locale, locale);
    // Use a comma-decimal locale when available; the file format remains C-locale.
    setlocale(LC_NUMERIC, "de_DE.UTF-8");

    CHECK(nrsc5_location_table_open(&table, INVALID_LOCATION_TABLE_PATH) == 1);
    CHECK(table == NULL);
    CHECK(nrsc5_location_table_open(NULL, LOCATION_TABLE_PATH) == 1);
    CHECK(nrsc5_location_table_open(&table, LOCATION_TABLE_PATH) == 0);
    CHECK(nrsc5_location_table_lookup(table, 1, 20, 4344, &location) == 1);
    CHECK(strcmp(location.name, "King Blvd/Exit 14") == 0);
    CHECK(fabs(location.latitude - 40.74785113334656) < 0.000000001);
    CHECK(fabs(location.longitude - -74.17136192321777) < 0.000000001);
    CHECK(location.positive == 4345);
    CHECK(location.negative == 4343);
    CHECK(nrsc5_location_table_lookup(table, 1, 20, 5146, &location) == 1);
    CHECK(strcmp(location.name, "Beginning of Freeway") == 0);
    CHECK(nrsc5_location_table_lookup(table, 1, 20, 51464, &location) == 1);
    CHECK(strcmp(location.name, "John F Kennedy Dr S") == 0);
    CHECK(nrsc5_location_table_lookup(table, 1, 20, UINT16_MAX, &location) == 0);
    nrsc5_location_table_close(table);
    CHECK(setlocale(LC_NUMERIC, original_locale) != NULL);
    return 0;
}
