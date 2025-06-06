#pragma once

#include <stdint.h>
#include <nrsc5.h>

#define MAX_PAYLOAD_BYTES (65536 + 2)
#define HERE_TRAFFIC_TILES 9

typedef struct
{
    nrsc5_t *radio;
    uint8_t buffer[MAX_PAYLOAD_BYTES];
    int buffer_idx;
    int expected_seq;
    int payload_len;
    uint64_t sync_state;
    unsigned int last_timestamp[1 + HERE_TRAFFIC_TILES];
} here_images_t;

void here_images_push(here_images_t *st, uint16_t seq, unsigned int len, uint8_t *buf);
void here_images_reset(here_images_t *st);
void here_images_init(here_images_t *st, nrsc5_t *radio);
