#pragma once

#include <complex.h>
#include <fftw3.h>
#include "firdecim_q15.h"

typedef struct
{
    struct input_t *input;
    firdecim_q15 filter;
    cint16_t *in_buffer;
    float complex *buffer;
    float complex *sums;
    float complex *fftin;
    float complex *fftout;
    float *shape;
    fftwf_plan fft;

    unsigned int idx;
    float prev_angle;
    float complex phase;
    int cfo;
} acquire_t;

void acquire_process(acquire_t *st);
void acquire_cfo_adjust(acquire_t *st, int cfo);
unsigned int acquire_push(acquire_t *st, cint16_t *buf, unsigned int length);
void acquire_init(acquire_t *st, struct input_t *input);
