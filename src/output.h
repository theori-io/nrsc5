#pragma once

#include <ao/ao.h>
#include <neaacdec.h>

#ifdef USE_THREADS
#include <pthread.h>
#endif

#define AUDIO_FRAME_BYTES 8192

typedef enum
{
    OUTPUT_ADTS,
    OUTPUT_HDC,
    OUTPUT_WAV,
    OUTPUT_LIVE
} output_method_t;

typedef struct output_buffer_t
{
    struct output_buffer_t *next;
    uint8_t data[AUDIO_FRAME_BYTES];
} output_buffer_t;

typedef struct
{
    output_method_t method;

    FILE *outfp;

    ao_device *dev;
    NeAACDecHandle handle;
#ifdef USE_THREADS
    output_buffer_t *head, *tail, *free;
    pthread_t worker_thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
} output_t;

void output_push(output_t *st, uint8_t *pkt, unsigned int len);
void output_reset(output_t *st);
void output_init_adts(output_t *st, const char *name);
void output_init_hdc(output_t *st, const char *name);
void output_init_wav(output_t *st, const char *name);
void output_init_live(output_t *st);
