#pragma once

#include "config.h"

#ifdef HAVE_FAAD2
#include <ao/ao.h>
#include <neaacdec.h>
#endif

#ifdef USE_THREADS
#include <pthread.h>
#endif

#define AUDIO_FRAME_BYTES 8192
#define MAX_PORTS 32

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
    uint16_t port;
    uint16_t pkt_size;
    uint8_t type;

    union
    {
        struct
        {
            char *name;
            uint32_t type;
            uint8_t *data;
            unsigned int size;
            unsigned int idx;
            unsigned int seq;
        } file;
    } u;
} aas_port_t;

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

    char *aas_files_path;
    aas_port_t ports[32];
    unsigned int audio_packets;
    unsigned int audio_bytes;
} output_t;

void output_push(output_t *st, uint8_t *pkt, unsigned int len);
void output_reset(output_t *st);
void output_init_adts(output_t *st, const char *name);
void output_init_hdc(output_t *st, const char *name);
#ifdef HAVE_FAAD2
void output_init_wav(output_t *st, const char *name);
void output_init_live(output_t *st);
#endif
void output_aas_push(output_t *st, uint8_t *psd, unsigned int len);
void output_set_aas_files_path(output_t *st, const char *path);
