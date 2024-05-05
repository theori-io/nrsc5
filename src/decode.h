#pragma once

#include <stdint.h>
#include "defines.h"
#include "pids.h"

#define DIVERSITY_DELAY_AM (18000 * 3)

typedef struct
{
  int8_t buffer[144 * BLKSZ * 2];
  unsigned int idx;
  int8_t internal[P3_FRAME_LEN_FM * 32];
  unsigned int i;
  unsigned int pt[4];
  int ready;
} interleaver_iv_t;

typedef struct
{
    struct input_t *input;
    int8_t buffer_pm[720 * BLKSZ * 16];
    unsigned int idx_pm;
    uint8_t buffer_pids_am[2 * BLKSZ];
    unsigned int idx_pids_am;
    uint8_t buffer_pu[PARTITION_WIDTH_AM * BLKSZ * 8];
    uint8_t buffer_pl[PARTITION_WIDTH_AM * BLKSZ * 8];
    uint8_t buffer_s[PARTITION_WIDTH_AM * BLKSZ * 8];
    uint8_t buffer_t[PARTITION_WIDTH_AM * BLKSZ * 8];
    unsigned int idx_pu_pl_s_t;
    unsigned int am_diversity_wait;

    uint8_t bl[18000];
    uint8_t bu[18000];
    uint8_t ml[18000 + DIVERSITY_DELAY_AM];
    uint8_t mu[18000 + DIVERSITY_DELAY_AM];
    uint8_t el[12000];
    uint8_t eu[24000];
    uint8_t ebl[18000];
    uint8_t ebu[18000];
    uint8_t eml[18000 + DIVERSITY_DELAY_AM];
    uint8_t emu[18000 + DIVERSITY_DELAY_AM];

    int8_t viterbi_p1[P1_FRAME_LEN_FM * 3];
    uint8_t scrambler_p1[P1_FRAME_LEN_FM];
    int8_t viterbi_pids[PIDS_FRAME_LEN * 3];
    uint8_t scrambler_pids[PIDS_FRAME_LEN];
    interleaver_iv_t interleaver_px1;
    interleaver_iv_t interleaver_px2;
    int8_t viterbi_p3[P3_FRAME_LEN_FM * 3];
    int8_t viterbi_p4[P3_FRAME_LEN_FM * 3];
    uint8_t scrambler_p3[P3_FRAME_LEN_FM];
    uint8_t scrambler_p4[P3_FRAME_LEN_FM];

    uint8_t p1_am[8 * P1_FRAME_LEN_ENCODED_AM];
    int8_t viterbi_p1_am[8 * P1_FRAME_LEN_AM * 3];
    uint8_t scrambler_p1_am[P1_FRAME_LEN_AM];
    uint8_t p3_am[P3_FRAME_LEN_ENCODED_MA3];
    int8_t viterbi_p3_am[P3_FRAME_LEN_MA3 * 3];
    uint8_t scrambler_p3_am[P3_FRAME_LEN_MA3];

    pids_t pids;
} decode_t;

void decode_process_p1(decode_t *st);
void decode_process_pids(decode_t *st);
void decode_process_p3_p4(decode_t *st, interleaver_iv_t *interleaver, int8_t *viterbi, uint8_t *scrambler, unsigned int frame_len, logical_channel_t lc);
void decode_process_pids_am(decode_t *st);
void decode_process_p1_p3_am(decode_t *st);
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
static inline void decode_push_px1_px2(decode_t *st, interleaver_iv_t *interleaver, int8_t *viterbi, uint8_t *scrambler, int8_t sbit, unsigned int frame_len)
{
    interleaver->buffer[interleaver->idx++] = sbit;
    if (interleaver->idx % (frame_len * 2) == 0)
    {
        decode_process_p3_p4(st, interleaver, viterbi, scrambler, frame_len, (interleaver == &st->interleaver_px1) ? P3_LOGICAL_CHANNEL : P4_LOGICAL_CHANNEL);
        interleaver->idx = 0;
    }
}
static inline void decode_push_px1(decode_t *st, int8_t sbit, unsigned int frame_len)
{
    decode_push_px1_px2(st, &st->interleaver_px1, st->viterbi_p3, st->scrambler_p3, sbit, frame_len);
}
static inline void decode_push_px2(decode_t *st, int8_t sbit)
{
    decode_push_px1_px2(st, &st->interleaver_px2, st->viterbi_p4, st->scrambler_p4, sbit, P3_FRAME_LEN_FM);
}
static inline void decode_push_pids(decode_t *st, uint8_t sym)
{
    st->buffer_pids_am[st->idx_pids_am++] = sym;
    if (st->idx_pids_am == 2 * BLKSZ)
    {
        decode_process_pids_am(st);
        st->idx_pids_am = 0;
    }
}
static inline void decode_push_pl_pu_s_t(decode_t *st, uint8_t sym_pl, uint8_t sym_pu, uint8_t sym_s, uint8_t sym_t)
{
    st->buffer_pl[st->idx_pu_pl_s_t] = sym_pl;
    st->buffer_pu[st->idx_pu_pl_s_t] = sym_pu;
    st->buffer_s[st->idx_pu_pl_s_t] = sym_s;
    st->buffer_t[st->idx_pu_pl_s_t] = sym_t;
    st->idx_pu_pl_s_t++;
    if (st->idx_pu_pl_s_t == PARTITION_WIDTH_AM * BLKSZ * 8)
    {
        decode_process_p1_p3_am(st);
        st->idx_pu_pl_s_t = 0;
    }
}
void decode_reset(decode_t *st);
void decode_init(decode_t *st, struct input_t *input);
