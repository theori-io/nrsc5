#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "navteq.h"

#define CHECK(condition) do { if (!(condition)) return false; } while (0)

static bool test_digital_traffic(void)
{
    static const uint8_t packet[] = {
        /* Type 7, generation 1, non-terminal, 16-byte payload. */
        0x71, 0x01, 0x00, 0x10,
        /* Primary: country 1, LTN 20, location 0xc90d, extent 2, event 124. */
        0x22, 0x14, 0xc9, 0x0d, 0x0a, 0x00, 0x7c, 0xff,
        /* Secondary: country 1, LTN 20, location 0xc908, extent 3, event 124. */
        0xc2, 0x14, 0xc9, 0x08, 0x0e, 0x00, 0x7c, 0xff,
    };
    nrsc5_navteq_digital_traffic_entry_t entries[2];
    unsigned int count = 0;
    uint8_t generation = 0;
    int is_terminal = -1;

    CHECK(navteq_decode_digital_traffic(packet, sizeof(packet), &generation,
                                        &is_terminal, NULL, &count));
    CHECK(count == 2);
    CHECK(generation == 1);
    CHECK(!is_terminal);

    CHECK(navteq_decode_digital_traffic(packet, sizeof(packet), &generation,
                                        &is_terminal, entries, &count));
    CHECK(entries[0].record_type == 1);
    CHECK(entries[0].country_code == 1);
    CHECK(entries[0].reserved == 0);
    CHECK(entries[0].location_table_number == 20);
    CHECK(entries[0].location == 0xc90d);
    CHECK(entries[0].direction == 0);
    CHECK(entries[0].extent == 2);
    CHECK(entries[0].bidirectional == 0);
    CHECK(entries[0].diversion == 0);
    CHECK(entries[0].duration_type == 0);
    CHECK(entries[0].control_code == 0);
    CHECK(entries[0].event == 124);
    CHECK(entries[0].quantifier == 0xff);
    CHECK(entries[1].record_type == 6);
    CHECK(entries[1].location == 0xc908);
    CHECK(entries[1].extent == 3);

    uint8_t quantified_packet[sizeof(packet)];
    memcpy(quantified_packet, packet, sizeof(packet));
    quantified_packet[8] = (quantified_packet[8] & ~0x02) | 0x01;
    quantified_packet[9] = 0x58;
    quantified_packet[11] = 1;
    quantified_packet[19] = 254;
    count = 2;
    CHECK(navteq_decode_digital_traffic(quantified_packet, sizeof(quantified_packet),
                                         &generation, &is_terminal, entries, &count));
    CHECK(entries[0].bidirectional == 1);
    CHECK(entries[0].diversion == 1);
    CHECK(entries[0].duration_type == 1);
    CHECK(entries[0].control_code == 3);
    CHECK(entries[0].quantifier == 1);
    CHECK(entries[1].quantifier == 254);

    uint8_t terminal_packet[sizeof(packet)];
    memcpy(terminal_packet, packet, sizeof(packet));
    terminal_packet[0] = 0x79;
    count = 0;
    CHECK(navteq_decode_digital_traffic(terminal_packet, sizeof(terminal_packet),
                                        &generation, &is_terminal, NULL, &count));
    CHECK(is_terminal);

    terminal_packet[3] = 0x20;
    CHECK(!navteq_decode_digital_traffic(terminal_packet, sizeof(terminal_packet),
                                          &generation, &is_terminal, NULL, &count));

    memcpy(terminal_packet, packet, sizeof(packet));
    terminal_packet[9] |= 0x80;
    CHECK(!navteq_decode_digital_traffic(terminal_packet, sizeof(terminal_packet),
                                         &generation, &is_terminal, NULL, &count));
    return true;
}

static bool test_alternate_frequencies(void)
{
    static const uint8_t packet[] = {
        /* Index 14, frequency code 186: 87.5 MHz + 18.6 MHz = 106.1 MHz. */
        0x23, 0xe0, 0xef, 0xba, 0x14, 0x90, 0x91, 0x73,
        /* Index 15, frequency code 138: 87.5 MHz + 13.8 MHz = 101.3 MHz. */
        0x23, 0xe0, 0xff, 0x8a, 0x14, 0x90, 0x8d, 0x6a,
    };
    nrsc5_navteq_alternate_frequency_entry_t entries[2];
    unsigned int count = 0;

    CHECK(navteq_decode_alternate_frequencies(packet, sizeof(packet), NULL, &count));
    CHECK(count == 2);
    CHECK(navteq_decode_alternate_frequencies(packet, sizeof(packet), entries, &count));
    CHECK(entries[0].index == 14);
    CHECK(entries[0].frequency_hz == 106100000);
    CHECK(memcmp(entries[0].data, packet, 8) == 0);
    CHECK(entries[1].index == 15);
    CHECK(entries[1].frequency_hz == 101300000);
    CHECK(memcmp(entries[1].data, packet + 8, 8) == 0);

    uint8_t malformed[sizeof(packet)];
    memcpy(malformed, packet, sizeof(packet));
    malformed[4] = 0;
    CHECK(!navteq_decode_alternate_frequencies(malformed, sizeof(malformed), NULL, &count));
    return true;
}

int main(void)
{
    return test_digital_traffic() && test_alternate_frequencies() ? 0 : 1;
}
