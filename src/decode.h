#pragma once

#include <stdint.h>
#include "defines.h"
#include "pids.h"

typedef struct
{
    struct input_t *input;
    int8_t *buffer_pm;
    unsigned int idx_pm;
    int8_t *buffer_px1;
    unsigned int idx_px1;

    int8_t *viterbi_p1;
    uint8_t *scrambler_p1;
    int8_t *viterbi_pids;
    uint8_t *scrambler_pids;
    int8_t *internal_p3;
    unsigned int i_p3;
    unsigned int pt_p3[4];
    int8_t *viterbi_p3;
    uint8_t *scrambler_p3;

    pids_t pids;
} decode_t;

void decode_process_p1(decode_t *st);
void decode_process_pids(decode_t *st);
void decode_process_p3(decode_t *st);
static inline unsigned int decode_get_block(decode_t *st)
{
    return st->idx_pm / (720 * BLKSZ);
}
static inline void decode_push_pm(decode_t *st, int8_t sbit)
{
    st->buffer_pm[st->idx_pm++] = sbit;
    if (st->idx_pm % (720 * BLKSZ) == 0)
    {
        decode_process_pids(st);
    }
    if (st->idx_pm == 720 * BLKSZ * 16)
    {
        decode_process_p1(st);
        st->idx_pm = 0;
    }
}
static inline void decode_push_px1(decode_t *st, int8_t sbit)
{
    st->buffer_px1[st->idx_px1++] = sbit;
    if (st->idx_px1 % (144 * BLKSZ * 2) == 0)
    {
        decode_process_p3(st);
        st->idx_px1 = 0;
    }
}
void decode_reset(decode_t *st);
void decode_init(decode_t *st, struct input_t *input);
