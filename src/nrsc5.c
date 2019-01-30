#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "private.h"

static int snr_callback(void *arg, float snr)
{
    nrsc5_t *st = arg;
    st->auto_gain_snr_ready = 1;
    st->auto_gain_snr = snr;
    return 1;
}

static int do_auto_gain(nrsc5_t *st)
{
    int gain_count, best_gain = 0, ret = 1;
    int *gain_list = NULL;
    float best_snr = 0;

    input_set_snr_callback(&st->input, snr_callback, st);

    gain_count = rtlsdr_get_tuner_gains(st->dev, NULL);
    if (gain_count < 0)
        goto error;

    gain_list = malloc(gain_count * sizeof(*gain_list));
    if (!gain_list)
        goto error;

    gain_count = rtlsdr_get_tuner_gains(st->dev, gain_list);
    if (gain_count < 0)
        goto error;

    for (int i = 0; i < gain_count; i++)
    {
        int gain = gain_list[i];

        if (rtlsdr_set_tuner_gain(st->dev, gain_list[i]) != 0)
            continue;

        st->auto_gain_snr_ready = 0;
        while (!st->auto_gain_snr_ready)
        {
            int len = sizeof(st->samples_buf);

            if (rtlsdr_read_sync(st->dev, st->samples_buf, len, &len) != 0)
                goto error;

            input_push(&st->input, st->samples_buf, len);
        }
        log_debug("Gain: %.1f dB, CNR: %.1f dB", gain / 10.0f, 20 * log10f(st->auto_gain_snr));
        if (st->auto_gain_snr > best_snr)
        {
            best_snr = st->auto_gain_snr;
            best_gain = gain;
        }
        input_reset(&st->input);
    }

    log_debug("Best gain: %.1f dB, CNR: %.1f dB", best_gain / 10.0f, 20 * log10f(best_snr));
    st->gain = best_gain;
    rtlsdr_set_tuner_gain(st->dev, best_gain);
    ret = 0;

error:
    free(gain_list);
    input_set_snr_callback(&st->input, NULL, NULL);
    return ret;
}

static void worker_cb(uint8_t *buf, uint32_t len, void *arg)
{
    nrsc5_t *st = arg;

    if (st->stopped && st->dev)
        rtlsdr_cancel_async(st->dev);
    else
        input_push(&st->input, buf, len);
}

static void *worker_thread(void *arg)
{
    nrsc5_t *st = arg;

    pthread_mutex_lock(&st->worker_mutex);
    while (!st->closed)
    {
        if (st->stopped && !st->worker_stopped)
        {
            st->worker_stopped = 1;
            pthread_cond_broadcast(&st->worker_cond);
        }
        else if (!st->stopped && st->worker_stopped)
        {
            st->worker_stopped = 0;
            pthread_cond_broadcast(&st->worker_cond);

            if (st->dev)
            {
                if (rtlsdr_reset_buffer(st->dev) != 0)
                    log_error("rtlsdr_reset_buffer failed");

                if (st->auto_gain && st->gain < 0)
                {
                    if (do_auto_gain(st) != 0)
                    {
                        st->stopped = 1;
                        st->worker_stopped = 1;
                    }
                }
            }
        }

        if (st->stopped)
        {
            // wait for a signal
            pthread_cond_wait(&st->worker_cond, &st->worker_mutex);
        }
        else
        {
            int err = 0;

            pthread_mutex_unlock(&st->worker_mutex);

            if (st->dev)
            {
                err = rtlsdr_read_async(st->dev, worker_cb, st, 8, 512 * 1024);
            }
            else if (st->iq_fd >= 0)
            {
                int count = read(st->iq_fd, st->samples_buf, sizeof(st->samples_buf));
                if (count > 0)
                    input_push(&st->input, st->samples_buf, count);
                else if (errno != EINTR)
                    err = 1;
            }

            pthread_mutex_lock(&st->worker_mutex);

            if (err)
            {
                st->stopped = 1;
                st->worker_stopped = 1;
                nrsc5_report_lost_device(st);
            }
        }
    }

    pthread_mutex_unlock(&st->worker_mutex);
    return NULL;
}

static void nrsc5_init(nrsc5_t *st)
{
    st->closed = 0;
    st->stopped = 1;
    st->worker_stopped = 1;
    st->auto_gain = 1;
    st->gain = -1;
    st->freq = NRSC5_SCAN_BEGIN;
    st->callback = NULL;

    output_init(&st->output, st);
    input_init(&st->input, st, &st->output);

    // Create worker thread
    pthread_mutex_init(&st->worker_mutex, NULL);
    pthread_cond_init(&st->worker_cond, NULL);
    pthread_create(&st->worker, NULL, worker_thread, st);
}

int nrsc5_open(nrsc5_t **result, int device_index, int ppm_error)
{
    int err;
    nrsc5_t *st = calloc(1, sizeof(*st));
    st->iq_fd = -1;
    st->pipe_fd = -1;

    if (rtlsdr_open(&st->dev, device_index) != 0)
        goto error_init;

    err = rtlsdr_set_sample_rate(st->dev, SAMPLE_RATE);
    if (err) goto error;
    err = rtlsdr_set_tuner_gain_mode(st->dev, 1);
    if (err) goto error;
    err = rtlsdr_set_freq_correction(st->dev, ppm_error);
    if (err && err != -2) goto error;
    err = rtlsdr_set_offset_tuning(st->dev, 1);
    if (err && err != -2) goto error;

    nrsc5_init(st);

    *result = st;
    return 0;

error:
    log_error("nrsc5_open error: %d", err);
    rtlsdr_close(st->dev);
error_init:
    free(st);
    *result = NULL;
    return 1;
}

int nrsc5_open_fd(nrsc5_t **result, int fd)
{
    nrsc5_t *st;

    st = calloc(1, sizeof(*st));
    st->iq_fd = fd;
    st->pipe_fd = -1;

    nrsc5_init(st);

    *result = st;
    return 0;
}

int nrsc5_open_pipe(nrsc5_t **result)
{
    int fds[2];

    if (pipe(fds) != 0)
        return 1;

    if (nrsc5_open_fd(result, fds[0]) != 0)
        return 1;

    (*result)->pipe_fd = fds[1];
    return 0;
}

void nrsc5_close(nrsc5_t *st)
{
    if (!st)
        return;

    // signal the worker to exit
    pthread_mutex_lock(&st->worker_mutex);
    st->closed = 1;
    pthread_cond_broadcast(&st->worker_cond);
    pthread_mutex_unlock(&st->worker_mutex);

    // wait for worker to finish
    pthread_join(st->worker, NULL);

    if (st->dev)
        rtlsdr_close(st->dev);
    if (st->iq_fd >= 0)
        close(st->iq_fd);
    if (st->pipe_fd >= 0)
        close(st->pipe_fd);

    input_free(&st->input);
    output_free(&st->output);
    free(st);
}

void nrsc5_start(nrsc5_t *st)
{
    // signal the worker to start
    pthread_mutex_lock(&st->worker_mutex);
    st->stopped = 0;
    pthread_cond_broadcast(&st->worker_cond);
    pthread_mutex_unlock(&st->worker_mutex);
}

void nrsc5_stop(nrsc5_t *st)
{
    // signal the worker to stop
    pthread_mutex_lock(&st->worker_mutex);
    st->stopped = 1;
    pthread_cond_broadcast(&st->worker_cond);
    pthread_mutex_unlock(&st->worker_mutex);

    // wait for worker to stop
    pthread_mutex_lock(&st->worker_mutex);
    while (st->stopped != st->worker_stopped)
        pthread_cond_wait(&st->worker_cond, &st->worker_mutex);
    pthread_mutex_unlock(&st->worker_mutex);
}

void nrsc5_get_frequency(nrsc5_t *st, float *freq)
{
    if (st->dev)
        *freq = rtlsdr_get_center_freq(st->dev);
    else
        *freq = st->freq;
}

int nrsc5_set_frequency(nrsc5_t *st, float freq)
{
    if (st->freq == freq)
        return 0;
    if (!st->stopped)
        return 1;

    if (st->dev)
    {
        if (rtlsdr_set_center_freq(st->dev, freq) != 0)
            return 1;

        if (st->auto_gain)
            st->gain = -1;
        input_reset(&st->input);
        output_reset(&st->output);
    }

    st->freq = freq;
    return 0;
}

void nrsc5_get_gain(nrsc5_t *st, float *gain)
{
    if (st->dev)
        *gain = rtlsdr_get_tuner_gain(st->dev) / 10.0f;
    else
        *gain = st->gain;
}

int nrsc5_set_gain(nrsc5_t *st, float gain)
{
    if (st->gain == gain)
        return 0;
    if (!st->stopped)
        return 1;

    if (st->dev)
    {
        if (rtlsdr_set_tuner_gain(st->dev, gain * 10) != 0)
            return 1;
    }

    st->gain = gain;
    return 0;
}

void nrsc5_set_auto_gain(nrsc5_t *st, int enabled)
{
    st->auto_gain = enabled;
    st->gain = -1;
}

void nrsc5_set_callback(nrsc5_t *st, nrsc5_callback_t callback, void *opaque)
{
    pthread_mutex_lock(&st->worker_mutex);
    st->callback = callback;
    st->callback_opaque = opaque;
    pthread_mutex_unlock(&st->worker_mutex);
}

int nrsc5_pipe_samples(nrsc5_t *st, uint8_t *samples, unsigned int length)
{
    int ret;

    while (length)
    {
        ret = write(st->pipe_fd, samples, length);
        if (ret > 0)
            length -= ret;
        else if (errno != EINTR)
            return 1;
    }

    return 0;
}

void nrsc5_report(nrsc5_t *st, const nrsc5_event_t *evt)
{
    if (st->callback)
        st->callback(evt, st->callback_opaque);
}

void nrsc5_report_lost_device(nrsc5_t *st)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_LOST_DEVICE;
    nrsc5_report(st, &evt);
}

void nrsc5_report_iq(nrsc5_t *st, const void *data, size_t count)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_IQ;
    evt.iq.data = data;
    evt.iq.count = count;
    nrsc5_report(st, &evt);
}

void nrsc5_report_sync(nrsc5_t *st)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_SYNC;
    nrsc5_report(st, &evt);
}

void nrsc5_report_lost_sync(nrsc5_t *st)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_LOST_SYNC;
    nrsc5_report(st, &evt);
}

void nrsc5_report_hdc(nrsc5_t *st, unsigned int program, const uint8_t *data, size_t count)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_HDC;
    evt.hdc.program = program;
    evt.hdc.data = data;
    evt.hdc.count = count;
    nrsc5_report(st, &evt);
}

void nrsc5_report_audio(nrsc5_t *st, unsigned int program, const int16_t *data, size_t count)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_AUDIO;
    evt.audio.program = program;
    evt.audio.data = data;
    evt.audio.count = count;
    nrsc5_report(st, &evt);
}

void nrsc5_report_mer(nrsc5_t *st, float lower, float upper)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_MER;
    evt.mer.lower = lower;
    evt.mer.upper = upper;
    nrsc5_report(st, &evt);
}

void nrsc5_report_ber(nrsc5_t *st, float cber)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_BER;
    evt.ber.cber = cber;
    nrsc5_report(st, &evt);
}

void nrsc5_report_lot(nrsc5_t *st, uint16_t port, unsigned int lot, unsigned int size, uint32_t mime, const char *name, const uint8_t *data)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_LOT;
    evt.lot.port = port;
    evt.lot.lot = lot;
    evt.lot.size = size;
    evt.lot.mime = mime;
    evt.lot.name = name;
    evt.lot.data = data;
    nrsc5_report(st, &evt);
}

static uint8_t convert_sig_component_type(uint8_t type)
{
    switch (type)
    {
    case SIG_COMPONENT_AUDIO: return NRSC5_SIG_COMPONENT_AUDIO;
    case SIG_COMPONENT_DATA: return NRSC5_SIG_COMPONENT_DATA;
    default:
        assert(0 && "Invalid component type");
        return 0;
    }
}

static uint8_t convert_sig_service_type(uint8_t type)
{
    switch (type)
    {
    case SIG_SERVICE_AUDIO: return NRSC5_SIG_SERVICE_AUDIO;
    case SIG_SERVICE_DATA: return NRSC5_SIG_SERVICE_DATA;
    default:
        assert(0 && "Invalid service type");
        return 0;
    }
}

void nrsc5_report_sig(nrsc5_t *st, sig_service_t *services, unsigned int count)
{
    nrsc5_event_t evt = { NRSC5_EVENT_SIG };
    nrsc5_sig_service_t *service = NULL;

    // convert internal structures to public structures
    for (unsigned int i = 0; i < count; i++)
    {
        nrsc5_sig_component_t *component = NULL;
        nrsc5_sig_service_t *prev = service;

        service = calloc(1, sizeof(nrsc5_sig_service_t));
        service->type = convert_sig_service_type(services[i].type);
        service->number = services[i].number;
        service->name = services[i].name;

        if (prev == NULL)
            evt.sig.services = service;
        else
            prev->next = service;

        for (unsigned int j = 0; j < MAX_SIG_COMPONENTS; j++)
        {
            nrsc5_sig_component_t *prevc = component;
            sig_component_t *internal = &services[i].component[j];

            if (internal->type == SIG_COMPONENT_NONE)
                continue;

            component = calloc(1, sizeof(nrsc5_sig_component_t));
            component->type = convert_sig_component_type(internal->type);
            component->id = internal->id;

            if (internal->type == SIG_COMPONENT_AUDIO)
            {
                component->audio.port = internal->audio.port;
                component->audio.type = internal->audio.type;
                component->audio.mime = internal->audio.mime;
            }
            else if (internal->type == SIG_COMPONENT_DATA)
            {
                component->data.port = internal->data.port;
                component->data.service_data_type = internal->data.service_data_type;
                component->data.type = internal->data.type;
                component->data.mime = internal->data.mime;
            }

            if (prevc == NULL)
                service->components = component;
            else
                prevc->next = component;
        }
    }

    nrsc5_report(st, &evt);

    // free the data structures
    for (service = evt.sig.services; service != NULL; )
    {
        void *p;
        nrsc5_sig_component_t *component;

        for (component = service->components; component != NULL; )
        {
            p = component;
            component = component->next;
            free(p);
        }

        p = service;
        service = service->next;
        free(p);
    }
}
