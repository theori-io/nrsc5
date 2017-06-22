#pragma once

#include <complex.h>
#ifdef USE_THREADS
#include <pthread.h>
#endif

typedef struct
{
    struct input_t *input;
    float complex *buffer;
    float *phases;
    uint8_t *ref_buf;
    unsigned int idx;
    unsigned int buf_idx;
    unsigned int used;
    int ready;
    int cfo_wait;

    int mer_cnt;
    float error_lb;
    float error_ub;

#ifdef USE_THREADS
    pthread_t worker_thread;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
#endif
} sync_t;

void sync_push(sync_t *st, float complex *fft);
void sync_wait(sync_t *st);
void sync_init(sync_t *st, struct input_t *input);
