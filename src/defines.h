#pragma once

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <complex.h>
#include <math.h>
#include "log.h"

#define FATAL_EXIT(...) do { log_fatal(__VA_ARGS__); exit(1); } while (0)

#define SNR_FFT_COUNT 256

// Sample rate before decimation
#define SAMPLE_RATE 1488375
// FFT length in samples
#define FFT 2048
// cyclic preflex length in samples
#define CP 112
#define FFTCP (FFT + CP)
// OFDM symbols per L1 block
#define BLKSZ 32
// symbols processed by each invocation of acquire_process
#define ACQUIRE_SYMBOLS (BLKSZ * 2)
// number of primary main partitions
#define PM_PARTITIONS 10
// index of first lower sideband subcarrier
#define LB_START (1024 - 546)
// index of last upper sideband subcarrier
#define UB_END (1024 + 546)
// bits per P1 frame
#define P1_FRAME_LEN 146176
// bits per PIDS frame
#define PIDS_FRAME_LEN 80
// bits per P1 frame
#define P3_FRAME_LEN 4608
// bits per L2 PCI
#define PCI_LEN 24
// bytes per L2 PDU (max)
#define PDU_LEN ((P1_FRAME_LEN - PCI_LEN) / 8)
// number of programs (max)
#define MAX_PROGRAMS 8

#define U8_F(x) ( (((float)(x)) - 127) / 128 )
#define U8_Q15(x) ( ((int16_t)(x) - 127) << 6 )

typedef struct {
    int16_t r, i;
} cint16_t;

static inline cint16_t cf_to_cq15(float complex x)
{
    cint16_t cq15;
    cq15.r = crealf(x) * 32767.0f;
    cq15.i = cimagf(x) * 32767.0f;
    return cq15;
}

static inline float complex cq15_to_cf(cint16_t cq15)
{
    return CMPLXF((float)cq15.r / 32767.0f, (float)cq15.i / 32767.0f);
}

static inline float normf(float complex v)
{
    float realf = crealf(v);
    float imagf = cimagf(v);
    return realf * realf + imagf * imagf;
}

static inline void fftshift(float complex *x, unsigned int size)
{
    int i, h = size / 2;
    for (i = 0; i < h; i += 4)
    {
        float complex t1 = x[i], t2 = x[i+1], t3 = x[i+2], t4 = x[i+3];
        x[i] = x[i + h];
        x[i+1] = x[i+1 + h];
        x[i+2] = x[i+2 + h];
        x[i+3] = x[i+3 + h];
        x[i + h] = t1;
        x[i+1 + h] = t2;
        x[i+2 + h] = t3;
        x[i+3 + h] = t4;
    }
}
