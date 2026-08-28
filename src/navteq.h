#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <nrsc5.h>

bool navteq_decode_digital_traffic(const uint8_t *buf, unsigned int len,
                                   uint8_t *generation, int *is_terminal,
                                   nrsc5_navteq_digital_traffic_entry_t *entries,
                                   unsigned int *count);
bool navteq_decode_alternate_frequencies(const uint8_t *buf, unsigned int len,
                                         nrsc5_navteq_alternate_frequency_entry_t *entries,
                                         unsigned int *count);
