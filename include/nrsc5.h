#ifndef NRSC5_H_
#define NRSC5_H_

/*
 * External API for nrsc5.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * Definitions.
 */
#define NRSC5_SCAN_BEGIN  87.9e6
#define NRSC5_SCAN_END   107.9e6
#define NRSC5_SCAN_SKIP    0.2e6

#define NRSC5_MIME_PRIMARY_IMAGE    0xBE4B7536
#define NRSC5_MIME_STATION_LOGO     0xD9C72536
#define NRSC5_MIME_NAVTEQ           0x2D42AC3E
#define NRSC5_MIME_HERE_TPEG        0x82F03DFC
#define NRSC5_MIME_HERE_IMAGE       0xB7F03DFC
#define NRSC5_MIME_HD_TMC           0xEECB55B6
#define NRSC5_MIME_HDC              0x4DC66C5A
#define NRSC5_MIME_TEXT             0xBB492AAC
#define NRSC5_MIME_JPEG             0x1E653E9C
#define NRSC5_MIME_PNG              0x4F328CA0
#define TTN_TPEG_1                  0xB39EBEB2
#define TTN_TPEG_2                  0x4EB03469
#define TTN_TPEG_3                  0x52103469
#define TTN_STM_TRAFFIC             0xFF8422D7
#define TTN_STM_WEATHER             0xEF042E96

/*
 * Data types.
 */
enum
{
    NRSC5_SIG_COMPONENT_AUDIO,
    NRSC5_SIG_COMPONENT_DATA
};

struct nrsc5_sig_component_t
{
    struct nrsc5_sig_component_t *next;
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
};
typedef struct nrsc5_sig_component_t nrsc5_sig_component_t;

enum
{
    NRSC5_SIG_SERVICE_AUDIO,
    NRSC5_SIG_SERVICE_DATA
};

struct nrsc5_sig_service_t
{
    struct nrsc5_sig_service_t *next;
    uint8_t type;
    uint16_t number;
    const char *name;
    nrsc5_sig_component_t *components;
};
typedef struct nrsc5_sig_service_t nrsc5_sig_service_t;

enum
{
    NRSC5_EVENT_LOST_DEVICE,
    NRSC5_EVENT_IQ,
    NRSC5_EVENT_SYNC,
    NRSC5_EVENT_LOST_SYNC,
    NRSC5_EVENT_MER,
    NRSC5_EVENT_BER,
    NRSC5_EVENT_HDC,
    NRSC5_EVENT_AUDIO,
    NRSC5_EVENT_ID3,
    NRSC5_EVENT_SIG,
    NRSC5_EVENT_LOT,
    NRSC5_EVENT_SIS
};

struct nrsc5_sis_asd_t
{
    struct nrsc5_sis_asd_t *next;
    unsigned int program;
    unsigned int access;
    unsigned int type;
    unsigned int sound_exp;
};
typedef struct nrsc5_sis_asd_t nrsc5_sis_asd_t;

struct nrsc5_sis_dsd_t
{
    struct nrsc5_sis_dsd_t *next;
    unsigned int access;
    unsigned int type;
    uint32_t mime_type;
};
typedef struct nrsc5_sis_dsd_t nrsc5_sis_dsd_t;

struct nrsc5_event_t
{
    unsigned int event;
    union
    {
        struct {
            const void *data;
            size_t count;
        } iq;
        struct {
            float cber;
        } ber;
        struct {
            float lower;
            float upper;
        } mer;
        struct {
            unsigned int program;
            const uint8_t *data;
            size_t count;
        } hdc;
        struct {
            unsigned int program;
            const int16_t *data;
            size_t count;
        } audio;
        struct {
            unsigned int program;
            const char *title;
            const char *artist;
            const char *album;
            const char *genre;
            struct {
                const char *owner;
                const char *id;
            } ufid;
            struct {
                uint32_t mime;
                int param;
                int lot;
            } xhdr;
        } id3;
        struct {
            uint16_t port;
            unsigned int lot;
            unsigned int size;
            uint32_t mime;
            const char *name;
            const uint8_t *data;
        } lot;
        struct {
            nrsc5_sig_service_t *services;
        } sig;
        struct {
            const char *country_code;
            int fcc_facility_id;
            const char *name;
            const char *slogan;
            const char *message;
            const char *alert;
            float latitude;
            float longitude;
            int altitude;
            nrsc5_sis_asd_t *audio_services;
            nrsc5_sis_dsd_t *data_services;
        } sis;
    };
};
typedef struct nrsc5_event_t nrsc5_event_t;

typedef void (*nrsc5_callback_t)(const nrsc5_event_t *evt, void *opaque);

/*
 * Opaque data types.
 */
typedef struct nrsc5_t nrsc5_t;

/*
 * Public functions. All functions return void or an error code (0 == success).
 */
void nrsc5_get_version(const char **version);
int nrsc5_open(nrsc5_t **, int device_index, int ppm_error);
int nrsc5_open_file(nrsc5_t **, FILE *fp);
int nrsc5_open_pipe(nrsc5_t **);
void nrsc5_close(nrsc5_t *);
void nrsc5_start(nrsc5_t *);
void nrsc5_stop(nrsc5_t *);
void nrsc5_get_frequency(nrsc5_t *, float *freq);
int nrsc5_set_frequency(nrsc5_t *, float freq);
void nrsc5_get_gain(nrsc5_t *, float *gain);
int nrsc5_set_gain(nrsc5_t *, float gain);
void nrsc5_set_auto_gain(nrsc5_t *, int enabled);
void nrsc5_set_callback(nrsc5_t *, nrsc5_callback_t callback, void *opaque);
int nrsc5_pipe_samples(nrsc5_t *, uint8_t *samples, unsigned int length);

#endif /* NRSC5_H_ */
