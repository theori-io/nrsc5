#pragma once

#include <pthread.h>
#include <rtl-sdr.h>
#include <stdio.h>

#include <nrsc5.h>

#include "config.h"
#include "defines.h"
#include "input.h"
#include "output.h"

struct nrsc5_t
{
    rtlsdr_dev_t *dev;
    FILE *iq_file;
    uint8_t samples_buf[128 * 256];
    float freq;
    int gain;
    int auto_gain;
    int auto_gain_snr_ready;
    float auto_gain_snr;
    int stopped;
    int worker_stopped;
    int closed;
    nrsc5_callback_t callback;
    void *callback_opaque;

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
void nrsc5_report_lot(nrsc5_t *, uint16_t port, unsigned int lot, unsigned int size, uint32_t mime, const char *name, const uint8_t *data);
void nrsc5_report_sig(nrsc5_t *, sig_service_t *services, unsigned int count);
void nrsc5_report_sis(nrsc5_t *, const char *country_code, int fcc_facility_id, const char *name,
                      const char *slogan, const char *message, const char *alert,
                      float latitude, float longitude, int altitude, nrsc5_sis_asd_t *audio_services,
                      nrsc5_sis_dsd_t *data_services);
