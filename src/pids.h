#pragma once

#include <stdint.h>

#define MAX_AUDIO_SERVICES 8
#define MAX_DATA_SERVICES 16
#define NUM_PARAMETERS 12

typedef struct
{
    int access;
    int type;
    int sound_exp;
} asd_t;

typedef struct
{
    int access;
    int type;
    int mime_type;
} dsd_t;

typedef struct
{
    char country_code[3];
    int fcc_facility_id;

    char short_name[8];

    char long_name[57];
    uint8_t long_name_have_frame[8];
    int long_name_seq;
    int long_name_displayed;

    float latitude;
    float longitude;
    int altitude;

    char message[192];
    uint8_t message_have_frame[32];
    int message_seq;
    int message_priority;
    int message_encoding;
    int message_len;
    int message_displayed;

    asd_t audio_services[MAX_AUDIO_SERVICES];
    dsd_t data_services[MAX_DATA_SERVICES];

    int parameters[NUM_PARAMETERS];

    char slogan[96];
    uint8_t slogan_have_frame[16];
    int slogan_encoding;
    int slogan_len;
    int slogan_displayed;
} pids_t;

void pids_frame_push(pids_t *st, uint8_t *bits);
void pids_init(pids_t *st);
