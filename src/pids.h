#pragma once

#include <stdint.h>

#define MAX_LONG_NAME_LEN 56
#define MAX_LONG_NAME_FRAMES 8
#define MAX_MESSAGE_LEN 190
#define MAX_MESSAGE_FRAMES 32
#define MAX_AUDIO_SERVICES 8
#define MAX_DATA_SERVICES 16
#define NUM_PARAMETERS 12
#define MAX_SLOGAN_LEN 95
#define MAX_SLOGAN_FRAMES 16
#define MAX_ALERT_LEN 381
#define MAX_ALERT_FRAMES 64

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
    struct input_t *input;

    char country_code[3];
    int fcc_facility_id;

    char short_name[8];

    char long_name[MAX_LONG_NAME_LEN + 1];
    uint8_t long_name_have_frame[MAX_LONG_NAME_FRAMES];
    int long_name_seq;
    int long_name_displayed;

    float latitude;
    float longitude;
    int altitude;

    char message[MAX_MESSAGE_LEN + 1];
    uint8_t message_have_frame[MAX_MESSAGE_FRAMES];
    int message_seq;
    int message_priority;
    int message_encoding;
    int message_len;
    int message_displayed;

    asd_t audio_services[MAX_AUDIO_SERVICES];
    dsd_t data_services[MAX_DATA_SERVICES];

    int parameters[NUM_PARAMETERS];

    char slogan[MAX_SLOGAN_LEN + 1];
    uint8_t slogan_have_frame[MAX_SLOGAN_FRAMES];
    int slogan_encoding;
    int slogan_len;
    int slogan_displayed;

    char alert[MAX_ALERT_LEN + 1];
    uint8_t alert_have_frame[MAX_ALERT_FRAMES];
    int alert_seq;
    int alert_encoding;
    int alert_len;
    int alert_cnt_len;
    int alert_displayed;
} pids_t;

void pids_frame_push(pids_t *st, uint8_t *bits);
void pids_init(pids_t *st, struct input_t *input);
