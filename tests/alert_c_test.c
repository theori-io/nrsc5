#include <string.h>

#include <nrsc5.h>

#define CHECK(condition) do { if (!(condition)) return 1; } while (0)

static int check_description(unsigned int event, unsigned int quantifier,
                             const char *expected)
{
    char description[256];

    nrsc5_alert_c_event_description(event, quantifier, description, sizeof(description));
    return strcmp(description, expected) == 0;
}

int main(void)
{
    const char *name;

    nrsc5_alert_c_event_name(1, &name);
    CHECK(strcmp(name, "traffic problem") == 0);
    nrsc5_alert_c_event_name(70, &name);
    CHECK(strcmp(name, "traffic congestion, average speed of 10 km/h") == 0);
    nrsc5_alert_c_event_name(124, &name);
    CHECK(strcmp(name, "traffic flowing freely") == 0);
    nrsc5_alert_c_event_name(201, &name);
    CHECK(strcmp(name, "accident") == 0);
    nrsc5_alert_c_event_name(2047, &name);
    CHECK(strcmp(name, "(null message) {completely silent message, see protocol, sect. 3.5.4}") == 0);
    nrsc5_alert_c_event_name(0, &name);
    CHECK(strcmp(name, "Unknown") == 0);
    nrsc5_alert_c_event_name(2048, &name);
    CHECK(strcmp(name, "Unknown") == 0);

    CHECK(check_description(124, 0, "traffic flowing freely"));
    CHECK(check_description(124, 255, "traffic flowing freely"));
    CHECK(check_description(201, 1, "1 accident"));
    CHECK(check_description(201, 3, "3 accidents"));
    CHECK(check_description(204, 2, "accident involving 2 heavy lorries"));
    CHECK(check_description(406, 1, "1st entry slip road closed"));
    CHECK(check_description(406, 2, "2nd entry slip road closed"));
    CHECK(check_description(1921, 15, "150 parking spaces available"));
    CHECK(check_description(1318, 5, "visibility reduced to less than 50 metres"));
    CHECK(check_description(1117, 20, "100 % probability of overcast weather"));
    CHECK(check_description(124, 20, "traffic flowing freely with average speeds of up to 100 km/h"));
    CHECK(check_description(124, 33, "traffic flowing freely with average speeds of up to 5 km/h"));
    CHECK(check_description(91, 12, "delays of up to 2 hours for cars"));
    CHECK(check_description(1083, 61, "current temperature 10 degrees Celsius"));
    CHECK(check_description(39, 7, "reopening of bridge expected 01:00"));
    CHECK(check_description(403, 120, "closed for heavy vehicles over 20.0 tonnes"));
    CHECK(check_description(1851, 120, "temporary width limit 20.0 metres"));
    CHECK(check_description(1101, 25, "heavy snowfall of up to 25 millimetres"));
    CHECK(check_description(1908, 186, "switch your car radio to 106.1 MHz"));
    CHECK(check_description(1913, 65, "switch your car radio to 972 kHz"));
    CHECK(check_description(2048, 1, "Unknown"));
    return 0;
}
