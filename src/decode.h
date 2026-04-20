#pragma once

#include <stdint.h>
#include "defines.h"
#include "pids.h"

#define DIVERSITY_DELAY_AM (18000 * 3)

typedef struct
{
  int8_t buffer[144 * BLKSZ * 2];
  int8_t internal[P3_FRAME_LEN_MP3_MP11 * 32];
  unsigned int i;
  unsigned int pt[4];
  int ready;
  int started;
} interleaver_iv_t;

typedef struct
{
    struct input_t *input;
    int8_t buffer_pm[PM_BLOCK_SIZE * 16];
    unsigned int idx_pm;
    int started_pm;
    uint8_t buffer_pu[PARTITION_WIDTH_AM * BLKSZ * 8];
    uint8_t buffer_pl[PARTITION_WIDTH_AM * BLKSZ * 8];
    uint8_t buffer_s[PARTITION_WIDTH_AM * BLKSZ * 8];
    uint8_t buffer_t[PARTITION_WIDTH_AM * BLKSZ * 8];
    unsigned int am_errors;
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
    int8_t viterbi_p3[P3_FRAME_LEN_MP3_MP11 * 3];
    int8_t viterbi_p4[P3_FRAME_LEN_MP3_MP11 * 3];
    uint8_t scrambler_p3[P3_FRAME_LEN_MP3_MP11];
    uint8_t scrambler_p4[P3_FRAME_LEN_MP3_MP11];

    uint8_t p1_am[8 * P1_FRAME_LEN_ENCODED_AM];
    int8_t viterbi_p1_am[8 * P1_FRAME_LEN_AM * 3];
    uint8_t scrambler_p1_am[P1_FRAME_LEN_AM];
    uint8_t p3_am[P3_FRAME_LEN_ENCODED_MA3];
    int8_t viterbi_p3_am[P3_FRAME_LEN_MA3 * 3];
    uint8_t scrambler_p3_am[P3_FRAME_LEN_MA3];

    pids_t pids;
} decode_t;

void decode_process_p1(decode_t *st);
void decode_process_pids(decode_t *st, unsigned int bc);
void decode_process_p3_p4(const decode_t *st, interleaver_iv_t *interleaver, int8_t *viterbi, uint8_t *scrambler, logical_channel_t lc);
void decode_process_pids_am(decode_t *st, const uint8_t* sbit);
void decode_process_p1_p3_am(decode_t *st, unsigned int bc);

void decode_push_pm(decode_t *st, const int8_t* sbit, unsigned int bc);
void decode_push_px1(decode_t *st, const int8_t* sbit, unsigned int len, unsigned int bc);
void decode_push_px2(decode_t *st, const int8_t* sbit, unsigned int len, unsigned int bc);

void decode_push_pl_pu_s_t(decode_t *st,
    const uint8_t* sym_pl, const uint8_t* sym_pu, const uint8_t* sym_s, const uint8_t* sym_t,
    unsigned int bc);

void decode_reset(decode_t *st);
void decode_init(decode_t *st, struct input_t *input);
