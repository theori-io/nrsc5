#pragma once

#include "config.h"

#include <nrsc5.h>

#ifdef HAVE_FAAD2
#include <neaacdec.h>
#endif

#define MAX_PORTS 32
#define MAX_SIG_SERVICES 8
#define MAX_SIG_COMPONENTS 8
#define MAX_LOT_FILES 8
#define LOT_FRAGMENT_SIZE 256
#define MAX_FILE_BYTES 65536
#define MAX_LOT_FRAGMENTS (MAX_FILE_BYTES / LOT_FRAGMENT_SIZE)

#define AUDIO_FRAME_CHANNELS 2
#define AUDIO_FRAME_LENGTH (NRSC5_AUDIO_FRAME_SAMPLES * AUDIO_FRAME_CHANNELS)

#define OUTPUT_BUFFER_LENGTH (64 * AUDIO_FRAME_LENGTH)

#define AAS_TYPE_STREAM 0
#define AAS_TYPE_PACKET 1
#define AAS_TYPE_LOT    3

enum
{
    SIG_COMPONENT_NONE,
    SIG_COMPONENT_DATA,
    SIG_COMPONENT_AUDIO
};

enum
{
    SIG_SERVICE_NONE,
    SIG_SERVICE_DATA,
    SIG_SERVICE_AUDIO
};

typedef struct
{
    unsigned int timestamp;
    char *name;
    uint32_t mime;
    struct tm expiry_utc;
    uint16_t lot;
    uint32_t size;
    uint8_t **fragments;
} aas_file_t;

typedef struct
{
    uint16_t port;
    uint8_t type;
    unsigned int service_number;
    uint32_t mime;
    aas_file_t lot_files[MAX_LOT_FILES];
} aas_port_t;

typedef struct
{
    uint8_t type;
    uint8_t id;

    union
    {
        struct {
            uint16_t port;
            uint16_t service_data_type;
            uint8_t type;
            uint32_t mime;
        } data;
        struct {
            uint8_t port;
            uint8_t type;
            uint32_t mime;
        } audio;
    };
} sig_component_t;

typedef struct
{
    uint8_t type;
    uint16_t number;
    char *name;

    sig_component_t component[MAX_SIG_COMPONENTS];
} sig_service_t;

typedef struct
{
    unsigned int seq;
    unsigned int size;
    uint8_t data[MAX_PDU_LEN];
} packet_t;

#ifdef HAVE_FAAD2
typedef struct
{
    NeAACDecHandle aacdec;

    int16_t* output_buffer;
    unsigned int write, read, leftover, delay;

    int input_start_pos;
} decoder_t;
#endif

typedef struct
{
    packet_t *ptr;

    unsigned int size, read, write;
    unsigned int latency, avg, delay;

    unsigned int clock;
} elastic_buffer_t;

typedef struct
{
    nrsc5_t *radio;
    aas_port_t ports[MAX_PORTS];
    sig_service_t services[MAX_SIG_SERVICES];
    elastic_buffer_t elastic[MAX_PROGRAMS][MAX_STREAMS];

#ifdef HAVE_FAAD2
    decoder_t decoder[MAX_PROGRAMS];
    int16_t silence[AUDIO_FRAME_LENGTH];
#endif
} output_t;

void output_align(output_t *st, unsigned int program, unsigned int stream_id, unsigned int pdu_seq, unsigned int latency, unsigned int avg, unsigned int seq, unsigned int nop);
void output_push(output_t *st, uint8_t *pkt, unsigned int len, unsigned int program, unsigned int stream_id, unsigned int seq);
void output_advance_elastic(output_t *st, int pos, unsigned int used);
void output_advance(output_t *st, unsigned int len, int mode);
void output_begin(output_t *st);
void output_reset(output_t *st);
void output_init(output_t *st, nrsc5_t *);
void output_free(output_t *st);
void output_aas_push(output_t *st, uint8_t *psd, unsigned int len);
