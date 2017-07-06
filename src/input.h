#pragma once

#include <stdint.h>
#include <stdio.h>
#include <complex.h>
#ifdef USE_THREADS
#include <pthread.h>
#endif

#include "acquire.h"
#include "decode.h"
#include "defines.h"
#include "firdecim_q15.h"
#include "frame.h"
#include "output.h"
#include "resamp_q15.h"
#include "sync.h"

typedef int (*input_snr_cb_t) (void *, float, float, float);

typedef struct input_t
{
    output_t *output;
    FILE *outfp;

    firdecim_q15 filter;
    resamp_q15 resamp;
    float resamp_rate;
    float complex *buffer;
    double center;
    unsigned int avail, used, skip;
    int cfo, cfo_idx, cfo_used;
    float complex cfo_tbl[FFT];

    fftwf_plan snr_fft;
    float complex snr_fft_in[64];
    float complex snr_fft_out[64];
    float snr_power[64];
    int snr_cnt;
    input_snr_cb_t snr_cb;
    void *snr_cb_arg;

#ifdef USE_THREADS
    pthread_t worker_thread;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
#endif

    acquire_t acq;
    decode_t decode;
    frame_t frame;
    sync_t sync;
} input_t;

void input_init(input_t *st, output_t *output, double center, unsigned int program, FILE *outfp);
void input_cb(uint8_t *, uint32_t, void *);
void input_set_snr_callback(input_t *st, input_snr_cb_t cb, void *);
void input_rate_adjust(input_t *st, float adj);
void input_cfo_adjust(input_t *st, int cfo);
void input_set_skip(input_t *st, unsigned int skip);
void input_wait(input_t *st, int flush);
void input_pdu_push(input_t *st, uint8_t *pdu, unsigned int len);
void input_psd_push(char *psd, unsigned int len);
