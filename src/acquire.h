#pragma once

#include <complex.h>
#include <fftw3.h>

typedef struct
{
    struct input_t *input;
    float complex *buffer;
    float complex *sums;
    float complex *fftin;
    float complex *fftout;
    float *shape;
    fftwf_plan fft;

    unsigned int idx;
    float prev_angle;
    float complex phase;
} acquire_t;

void acquire_process(acquire_t *st);
unsigned int acquire_push(acquire_t *st, float complex *buf, unsigned int length);
void acquire_init(acquire_t *st, struct input_t *input);
