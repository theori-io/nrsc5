#pragma once

#include <pthread.h>
#include <rtl-sdr.h>
#include <stdio.h>

#include <nrsc5.h>

#include "config.h"
#include "defines.h"
#include "input.h"
#include "output.h"
#include "rtltcp.h"

extern pthread_mutex_t fftw_mutex;

struct nrsc5_t
{
    rtlsdr_dev_t *dev;
    FILE *iq_file;
    rtltcp_t *rtltcp;
    uint8_t samples_buf[128 * 256];
    float freq;
    int mode;
    int gain;
    int auto_gain;
    int stopped;
    int worker_stopped;
    int closed;
    nrsc5_callback_t callback;
    void *callback_opaque;

    uint8_t leftover_u8[4];
    unsigned int leftover_u8_num;
    int16_t leftover_s16[2];
    unsigned int leftover_s16_num;

    pthread_t worker;
    pthread_mutex_t worker_mutex;
    pthread_cond_t worker_cond;

    input_t input;
    output_t output;
};

void nrsc5_report(nrsc5_t *, const nrsc5_event_t *evt);
void nrsc5_report_lost_device(nrsc5_t *st);
void nrsc5_report_iq(nrsc5_t *, const void *data, size_t count);
void nrsc5_report_sync(nrsc5_t *);
void nrsc5_report_lost_sync(nrsc5_t *);
void nrsc5_report_mer(nrsc5_t *, float lower, float upper);
void nrsc5_report_ber(nrsc5_t *, float cber);
void nrsc5_report_hdc(nrsc5_t *, unsigned int program, const uint8_t *data, size_t count);
void nrsc5_report_audio(nrsc5_t *, unsigned int program, const int16_t *data, size_t count);
void nrsc5_report_stream(nrsc5_t *, uint16_t port, uint16_t seq, unsigned int size, uint32_t mime, const uint8_t *data);
void nrsc5_report_packet(nrsc5_t *, uint16_t port, uint16_t seq, unsigned int size, uint32_t mime, const uint8_t *data);
void nrsc5_report_lot(nrsc5_t *, uint16_t port, unsigned int lot, unsigned int size, uint32_t mime, const char *name,
                      const uint8_t *data, struct tm *expiry_utc, uint32_t component_mime);
void nrsc5_report_audio_service(nrsc5_t *, unsigned int program, unsigned int access, unsigned int type, 
                                unsigned int codec_mode, unsigned int blend_control, int digital_audio_gain,
                                unsigned int common_delay, unsigned int latency);
void nrsc5_report_sig(nrsc5_t *, sig_service_t *services, unsigned int count);
void nrsc5_report_sis(nrsc5_t *, const char *country_code, int fcc_facility_id, const char *name,
                      const char *slogan, const char *message, const char *alert, const uint8_t *cnt, int cnt_length,
                      int category1, int category2, int location_format, int num_locations, const int *locations,
                      float latitude, float longitude, int altitude, nrsc5_sis_asd_t *audio_services,
                      nrsc5_sis_dsd_t *data_services);
