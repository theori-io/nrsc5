#pragma once

#include <complex.h>
#include <liquid/liquid.h>
#include <fftw3.h>

typedef struct
{
    struct input_t *input;
    float complex *buffer;
    float complex *sums;
    float complex *fftin;
    float complex *fftout;
    float *sintbl;
    float *shape;
    fftwf_plan fft;

    float samperr;
    unsigned int idx;
    int ready;
} acquire_t;

void acquire_process(acquire_t *st);
unsigned int acquire_push(acquire_t *st, float complex *buf, unsigned int length);
void acquire_init(acquire_t *st, struct input_t *input);
