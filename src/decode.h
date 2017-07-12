#pragma once

#include <stdint.h>
#include "defines.h"

typedef struct
{
    struct input_t *input;
    int8_t *buffer;
    unsigned int idx;

    int8_t *viterbi_p1;
    uint8_t *scrambler_p1;
    int8_t *viterbi_pids;
    uint8_t *scrambler_pids;
} decode_t;

void decode_process_p1(decode_t *st);
void decode_process_pids(decode_t *st);
static inline unsigned int decode_get_block(decode_t *st)
{
    return st->idx / (720 * BLKSZ);
}
static inline void decode_push(decode_t *st, int8_t sbit)
{
    st->buffer[st->idx] = sbit;
    if (++st->idx % (720 * BLKSZ) == 0)
    {
        decode_process_pids(st);
    }
    if (st->idx == 720 * BLKSZ * 16)
    {
        decode_process_p1(st);
        st->idx = 0;
    }
}
void decode_reset(decode_t *st);
void decode_init(decode_t *st, struct input_t *input);
