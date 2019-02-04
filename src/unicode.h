#pragma once

#include <stdint.h>

char *iso_8859_1_to_utf_8(uint8_t *buf, unsigned int len);
char *ucs_2_to_utf_8(uint8_t *buf, unsigned int len);
