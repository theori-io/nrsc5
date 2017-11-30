#pragma once

#include "config.h"

#include <complex.h>

typedef struct
{
    struct input_t *input;
    float complex *buffer;
    float *phases;
    uint8_t *ref_buf;
    unsigned int idx;
    unsigned int used;
    int ready;
    int cfo_wait;
    int samperr;
    float angle;

    float alpha;
    float beta;
    float costas_freq[FFT];
    float costas_phase[FFT];

    int mer_cnt;
    float error_lb;
    float error_ub;
} sync_t;

void sync_adjust(sync_t *st, int sample_adj);
void sync_push(sync_t *st, float complex *fft);
void sync_init(sync_t *st, struct input_t *input);
