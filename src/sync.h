#pragma once

#include <complex.h>
#include <pthread.h>

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

    pthread_t worker_thread;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
} sync_t;

void sync_push(sync_t *st, float complex *fft);
void sync_wait(sync_t *st);
void sync_init(sync_t *st, struct input_t *input);
