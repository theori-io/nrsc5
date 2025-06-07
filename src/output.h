#pragma once

#include "config.h"
#include "here_images.h"

#include <nrsc5.h>

#ifdef HAVE_FAAD2
#include <neaacdec.h>
#endif

#define AUDIO_FRAME_BYTES 8192
#define MAX_SIG_SERVICES 16
#define MAX_SIG_COMPONENTS 8
#define MAX_LOT_FILES 12
#define LOT_FRAGMENT_SIZE 256
#define MAX_FILE_BYTES 65536
#define MAX_LOT_FRAGMENTS (MAX_FILE_BYTES / LOT_FRAGMENT_SIZE)

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
    uint32_t bytes_so_far;
    uint8_t **fragments;
} aas_file_t;

typedef struct
{
    uint8_t type;
    uint8_t id;

    nrsc5_sig_service_t *service_ext;
    nrsc5_sig_component_t *component_ext;

    union
    {
        struct {
            uint16_t port;
            uint16_t service_data_type;
            uint8_t type;
            uint32_t mime;
            aas_file_t lot_files[MAX_LOT_FILES];
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
    unsigned int size;
    uint8_t data[MAX_PDU_LEN];
} packet_t;

typedef struct
{
    packet_t packets[ELASTIC_BUFFER_LEN];
    int audio_offset;
} elastic_buffer_t;

typedef struct
{
    nrsc5_t *radio;
    elastic_buffer_t elastic[MAX_PROGRAMS][MAX_STREAMS];
#ifdef HAVE_FAAD2
    NeAACDecHandle aacdec[MAX_PROGRAMS];
    int16_t silence[NRSC5_AUDIO_FRAME_SAMPLES * 2];
#endif
    sig_service_t services[MAX_SIG_SERVICES];
    unsigned int lot_lru_counter;
    here_images_t here_images;
} output_t;

void output_align(output_t *st, unsigned int program, unsigned int stream_id, unsigned int offset);
void output_push(output_t *st, uint8_t *pkt, unsigned int len, unsigned int program, unsigned int stream_id, unsigned int seq);
void output_advance(output_t *st);
void output_reset(output_t *st);
void output_init(output_t *st, nrsc5_t *);
void output_free(output_t *st);
void output_aas_push(output_t *st, uint8_t *psd, unsigned int len);
