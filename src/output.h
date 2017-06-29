#pragma once

#ifdef HAVE_FAAD2
#include <ao/ao.h>
#include <neaacdec.h>
#endif

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

#ifdef HAVE_FAAD2
    ao_device *dev;
    NeAACDecHandle handle;
#endif
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
#ifdef HAVE_FAAD2
void output_init_wav(output_t *st, const char *name);
void output_init_live(output_t *st);
#endif
