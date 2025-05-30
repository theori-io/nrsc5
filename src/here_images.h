#pragma once

#include <stdint.h>
#include <nrsc5.h>

#define MAX_PAYLOAD_BYTES (65536 + 2)

typedef struct
{
    nrsc5_t *radio;
    uint8_t buffer[MAX_PAYLOAD_BYTES];
    int buffer_idx;
    int expected_seq;
    int payload_len;
    uint64_t sync_state;
} here_images_t;

void here_images_push(here_images_t *st, uint16_t seq, unsigned int len, uint8_t *buf);
void here_images_reset(here_images_t *st);
