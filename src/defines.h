#pragma once

#include "config.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <complex.h>
#include <math.h>

// Sample rate before decimation
#define SAMPLE_RATE 1488375
// FFT length in samples
#define FFT_FM 2048
#define FFT_AM 256
// cyclic preflex length in samples
#define CP_FM 112
#define CP_AM 14
#define FFTCP_FM (FFT_FM + CP_FM)
#define FFTCP_AM (FFT_AM + CP_AM)
// OFDM symbols per L1 block
#define BLKSZ 32
// symbols processed by each invocation of acquire_process
#define ACQUIRE_SYMBOLS (BLKSZ * 2)
// index of first lower sideband subcarrier
#define LB_START ((FFT_FM / 2) - 546)
// index of last upper sideband subcarrier
#define UB_END ((FFT_FM / 2) + 546)
// index of AM carrier
#define CENTER_AM (FFT_AM / 2)
// indexes of AM subcarriers
#define REF_INDEX_AM 1
#define PIDS_1_INDEX_AM 27
#define PIDS_2_INDEX_AM 53
#define TERTIARY_INDEX_AM 2
#define SECONDARY_INDEX_AM 28
#define PRIMARY_INDEX_AM 57
#define MAX_INDEX_AM 81
// bits per P1 frame
#define P1_FRAME_LEN_FM 146176
#define P1_FRAME_LEN_AM 3750
// bits per encoded P1 frame
#define P1_FRAME_LEN_ENCODED_FM (P1_FRAME_LEN_FM * 5 / 2)
#define P1_FRAME_LEN_ENCODED_AM (P1_FRAME_LEN_AM * 12 / 5)
// bits per PIDS frame
#define PIDS_FRAME_LEN 80
// bits per encoded PIDS frame
#define PIDS_FRAME_LEN_ENCODED_FM (PIDS_FRAME_LEN * 5 / 2)
#define PIDS_FRAME_LEN_ENCODED_AM (PIDS_FRAME_LEN * 3)
// bits per P3 frame
#define P3_FRAME_LEN_FM 4608
#define P3_FRAME_LEN_AM 24000
// bits per encoded P3 frame
#define P3_FRAME_LEN_ENCODED_FM (P3_FRAME_LEN_FM * 2)
#define P3_FRAME_LEN_ENCODED_AM (P3_FRAME_LEN_AM * 3 / 2)
// bits per L2 PCI
#define PCI_LEN 24
// bytes per L2 PDU (max)
#define MAX_PDU_LEN ((P1_FRAME_LEN_FM - PCI_LEN) / 8)
// bytes per L2 PDU in P1 frame (AM)
#define P1_PDU_LEN_AM 466
// number of programs (max)
#define MAX_PROGRAMS 8
// number of streams per program (max)
#define MAX_STREAMS 4
// number of subcarriers per AM partition
#define PARTITION_WIDTH_AM 25

#define DEBUG_LEVEL 5
#define log_debug(...) \
            do { if (DEBUG_LEVEL <= 1) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define log_info(...) \
            do { if (DEBUG_LEVEL <= 2) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define log_warn(...) \
            do { if (DEBUG_LEVEL <= 3) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define log_error(...) \
            do { if (DEBUG_LEVEL <= 4) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define U8_F(x) ( (((float)(x)) - 127) / 128 )
#define U8_Q15(x) ( ((int16_t)(x) - 127) * 64 )

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

static inline float complex cq15_to_cf_conj(cint16_t cq15)
{
    return CMPLXF((float)cq15.r / 32767.0f, (float)cq15.i / -32767.0f);
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
