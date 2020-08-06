#pragma once

#include "config.h"

#include <complex.h>

typedef struct
{
    struct input_t *input;
    float complex buffer[FFT_FM][BLKSZ];
    float phases[FFT_FM][BLKSZ];
    unsigned int idx;
    int psmi;
    int cfo_wait;
    unsigned int offset_history;
    int samperr;
    float angle;

    float alpha;
    float beta;
    float costas_freq[FFT_FM];
    float costas_phase[FFT_FM];

    int mer_cnt;
    float error_lb;
    float error_ub;
} sync_t;

void sync_adjust(sync_t *st, int sample_adj);
void sync_push(sync_t *st, float complex *fft);
void sync_reset(sync_t *st);
void sync_init(sync_t *st, struct input_t *input);
