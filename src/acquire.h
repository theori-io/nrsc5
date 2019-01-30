#pragma once

#include <complex.h>
#include <fftw3.h>
#include "firdecim_q15.h"

typedef struct
{
    struct input_t *input;
    firdecim_q15 filter;
    cint16_t in_buffer[FFTCP * (ACQUIRE_SYMBOLS + 1)];
    float complex buffer[FFTCP * (ACQUIRE_SYMBOLS + 1)];
    float complex sums[FFTCP + CP];
    float complex fftin[FFT];
    float complex fftout[FFT];
    float shape[FFTCP];
    fftwf_plan fft;

    unsigned int idx;
    float prev_angle;
    float complex phase;
    int cfo;
} acquire_t;

void acquire_process(acquire_t *st);
void acquire_cfo_adjust(acquire_t *st, int cfo);
unsigned int acquire_push(acquire_t *st, cint16_t *buf, unsigned int length);
void acquire_reset(acquire_t *st);
void acquire_init(acquire_t *st, struct input_t *input);
void acquire_free(acquire_t *st);
