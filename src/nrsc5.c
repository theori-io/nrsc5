#include <assert.h>
#include <string.h>

#include "private.h"

pthread_mutex_t fftw_mutex = PTHREAD_MUTEX_INITIALIZER;

static int get_tuner_gains(nrsc5_t *st, int *gains)
{
    if (st->dev)
        return rtlsdr_get_tuner_gains(st->dev, gains);
    assert(st->rtltcp);
    return rtltcp_get_tuner_gains(st->rtltcp, gains);
}

static int set_tuner_gain(nrsc5_t *st, int gain)
{
    if (st->dev)
        return rtlsdr_set_tuner_gain(st->dev, gain);
    assert(st->rtltcp);
    return rtltcp_set_tuner_gain(st->rtltcp, gain);
}

static int do_auto_gain(nrsc5_t *st)
{
    int gain_count, best_gain = 0, ret = 1;
    int *gain_list = NULL;
    float best_amplitude_db = 0.0f;

    gain_count = get_tuner_gains(st, NULL);
    if (gain_count < 0)
        goto error;

    gain_list = malloc(gain_count * sizeof(*gain_list));
    if (!gain_list)
        goto error;

    gain_count = get_tuner_gains(st, gain_list);
    if (gain_count < 0)
        goto error;

    for (int i = 0; i < gain_count; i++)
    {
        int gain = gain_list[i];

        if (set_tuner_gain(st, gain_list[i]) != 0)
            continue;

        if (st->rtltcp)
        {
            // there is no good way to wait for samples after the new gain was applied
            // dump 250ms of samples and hope for the best
            rtltcp_reset_buffer(st->rtltcp, (NRSC5_SAMPLE_RATE_CU8 / 4) * 2);
        }

        int len = sizeof(st->samples_buf);

        if (st->dev)
        {
            if (rtlsdr_read_sync(st->dev, st->samples_buf, len, &len) != 0)
                goto error;
        }
        else
        {
            assert(st->rtltcp);
            if (rtltcp_read(st->rtltcp, st->samples_buf, len) != len)
                goto error;
        }

        uint8_t max_sample = 0;
        uint8_t min_sample = 255;
        for (int j = 0; j < len; j++)
        {
            if (st->samples_buf[j] > max_sample)
                max_sample = st->samples_buf[j];
            if (st->samples_buf[j] < min_sample)
                min_sample = st->samples_buf[j];
        }

        float amplitude_db = 20 * log10f((max_sample - min_sample + 1) / 256.0f);
        log_debug("Gain: %.1f dB, Peak amplitude: %.1f dBFS", gain / 10.0f, amplitude_db);

        if ((i == 0) || (amplitude_db < -6.0f))
        {
            best_gain = gain;
            best_amplitude_db = amplitude_db;
        }
    }

    log_debug("Best gain: %.1f dB, Peak amplitude: %.1f dBFS", best_gain / 10.0f, best_amplitude_db);
    st->gain = best_gain;
    set_tuner_gain(st, best_gain);
    ret = 0;

error:
    free(gain_list);
    return ret;
}

static int using_worker(nrsc5_t *st)
{
    return st->dev || st->rtltcp || st->iq_file;
}

static void worker_cb(uint8_t *buf, uint32_t len, void *arg)
{
    nrsc5_t *st = arg;

    if (st->stopped && st->dev)
        rtlsdr_cancel_async(st->dev);
    else
        input_push_cu8(&st->input, buf, len);
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

            if (st->dev && rtlsdr_reset_buffer(st->dev) != 0)
                log_error("rtlsdr_reset_buffer failed");

            if (st->dev || st->rtltcp)
            {
                if (st->auto_gain && st->gain < 0 && do_auto_gain(st) != 0)
                {
                    st->stopped = 1;
                    continue;
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
            else if (st->rtltcp)
            {
                err = rtltcp_read(st->rtltcp, st->samples_buf, sizeof(st->samples_buf));
                if (err >= 0)
                {
                    // a short read is possible when EOF and may not be aligned
                    input_push_cu8(&st->input, st->samples_buf, err & ~3);
                    if (err == sizeof(st->samples_buf))
                        err = 0;
                }
            }
            else if (st->iq_file)
            {
                int count = fread(st->samples_buf, 4, sizeof(st->samples_buf) / 4, st->iq_file);
                if (count > 0)
                    input_push_cu8(&st->input, st->samples_buf, count * 4);
                if (feof(st->iq_file) || ferror(st->iq_file))
                    err = 1;
            }

            pthread_mutex_lock(&st->worker_mutex);

            if (err)
            {
                st->stopped = 1;
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
    st->mode = NRSC5_MODE_FM;
    st->callback = NULL;

    output_init(&st->output, st);
    input_init(&st->input, st, &st->output);

    if (using_worker(st))
    {
        // Create worker thread
        pthread_mutex_init(&st->worker_mutex, NULL);
        pthread_cond_init(&st->worker_cond, NULL);
        pthread_create(&st->worker, NULL, worker_thread, st);
    }
}

void nrsc5_get_version(const char **version)
{
    *version = GIT_COMMIT_HASH;
}

void nrsc5_service_data_type_name(unsigned int type, const char **name)
{
    switch (type)
    {
    case NRSC5_SERVICE_DATA_TYPE_NON_SPECIFIC: *name = "Non-specific"; break;
    case NRSC5_SERVICE_DATA_TYPE_NEWS: *name = "News"; break;
    case NRSC5_SERVICE_DATA_TYPE_SPORTS: *name = "Sports"; break;
    case NRSC5_SERVICE_DATA_TYPE_WEATHER: *name = "Weather"; break;
    case NRSC5_SERVICE_DATA_TYPE_EMERGENCY: *name = "Emergency"; break;
    case NRSC5_SERVICE_DATA_TYPE_TRAFFIC: *name = "Traffic"; break;
    case NRSC5_SERVICE_DATA_TYPE_IMAGE_MAPS: *name = "Image Maps"; break;
    case NRSC5_SERVICE_DATA_TYPE_TEXT: *name = "Text"; break;
    case NRSC5_SERVICE_DATA_TYPE_ADVERTISING: *name = "Advertising"; break;
    case NRSC5_SERVICE_DATA_TYPE_FINANCIAL: *name = "Financial"; break;
    case NRSC5_SERVICE_DATA_TYPE_STOCK_TICKER: *name = "Stock Ticker"; break;
    case NRSC5_SERVICE_DATA_TYPE_NAVIGATION: *name = "Navigation"; break;
    case NRSC5_SERVICE_DATA_TYPE_ELECTRONIC_PROGRAM_GUIDE: *name = "Electronic Program Guide"; break;
    case NRSC5_SERVICE_DATA_TYPE_AUDIO: *name = "Audio"; break;
    case NRSC5_SERVICE_DATA_TYPE_PRIVATE_DATA_NETWORK: *name = "Private Data Network"; break;
    case NRSC5_SERVICE_DATA_TYPE_SERVICE_MAINTENANCE: *name = "Service Maintenance"; break;
    case NRSC5_SERVICE_DATA_TYPE_HD_RADIO_SYSTEM_SERVICES: *name = "HD Radio System Services"; break;
    case NRSC5_SERVICE_DATA_TYPE_AUDIO_RELATED_DATA: *name = "Audio-Related Objects"; break;
    case NRSC5_SERVICE_DATA_TYPE_RESERVED_FOR_SPECIAL_TESTS: *name = "Reserved for Special Tests"; break;
    default: *name = "Unknown"; break;
    }
}

void nrsc5_program_type_name(unsigned int type, const char **name)
{
    switch (type)
    {
    case NRSC5_PROGRAM_TYPE_UNDEFINED: *name = "None"; break;
    case NRSC5_PROGRAM_TYPE_NEWS: *name = "News"; break;
    case NRSC5_PROGRAM_TYPE_INFORMATION: *name = "Information"; break;
    case NRSC5_PROGRAM_TYPE_SPORTS: *name = "Sports"; break;
    case NRSC5_PROGRAM_TYPE_TALK: *name = "Talk"; break;
    case NRSC5_PROGRAM_TYPE_ROCK: *name = "Rock"; break;
    case NRSC5_PROGRAM_TYPE_CLASSIC_ROCK: *name = "Classic Rock"; break;
    case NRSC5_PROGRAM_TYPE_ADULT_HITS: *name = "Adult Hits"; break;
    case NRSC5_PROGRAM_TYPE_SOFT_ROCK: *name = "Soft Rock"; break;
    case NRSC5_PROGRAM_TYPE_TOP_40: *name = "Top 40"; break;
    case NRSC5_PROGRAM_TYPE_COUNTRY: *name = "Country"; break;
    case NRSC5_PROGRAM_TYPE_OLDIES: *name = "Oldies"; break;
    case NRSC5_PROGRAM_TYPE_SOFT: *name = "Soft"; break;
    case NRSC5_PROGRAM_TYPE_NOSTALGIA: *name = "Nostalgia"; break;
    case NRSC5_PROGRAM_TYPE_JAZZ: *name = "Jazz"; break;
    case NRSC5_PROGRAM_TYPE_CLASSICAL: *name = "Classical"; break;
    case NRSC5_PROGRAM_TYPE_RHYTHM_AND_BLUES: *name = "Rhythm and Blues"; break;
    case NRSC5_PROGRAM_TYPE_SOFT_RHYTHM_AND_BLUES: *name = "Soft Rhythm and Blues"; break;
    case NRSC5_PROGRAM_TYPE_FOREIGN_LANGUAGE: *name = "Foreign Language"; break;
    case NRSC5_PROGRAM_TYPE_RELIGIOUS_MUSIC: *name = "Religious Music"; break;
    case NRSC5_PROGRAM_TYPE_RELIGIOUS_TALK: *name = "Religious Talk"; break;
    case NRSC5_PROGRAM_TYPE_PERSONALITY: *name = "Personality"; break;
    case NRSC5_PROGRAM_TYPE_PUBLIC: *name = "Public"; break;
    case NRSC5_PROGRAM_TYPE_COLLEGE: *name = "College"; break;
    case NRSC5_PROGRAM_TYPE_SPANISH_TALK: *name = "Spanish Talk"; break;
    case NRSC5_PROGRAM_TYPE_SPANISH_MUSIC: *name = "Spanish Music"; break;
    case NRSC5_PROGRAM_TYPE_HIP_HOP: *name = "Hip-Hop"; break;
    case NRSC5_PROGRAM_TYPE_WEATHER: *name = "Weather"; break;
    case NRSC5_PROGRAM_TYPE_EMERGENCY_TEST: *name = "Emergency Test"; break;
    case NRSC5_PROGRAM_TYPE_EMERGENCY: *name = "Emergency"; break;
    case NRSC5_PROGRAM_TYPE_TRAFFIC: *name = "Traffic"; break;
    case NRSC5_PROGRAM_TYPE_SPECIAL_READING_SERVICES: *name = "Special Reading Services"; break;
    default: *name = "Unknown"; break;
    }
}

static nrsc5_t *nrsc5_alloc()
{
    nrsc5_t *st = calloc(1, sizeof(*st));
    return st;
}

int nrsc5_open(nrsc5_t **result, int device_index)
{
    int err;
    nrsc5_t *st = nrsc5_alloc();

    if (rtlsdr_open(&st->dev, device_index) != 0)
        goto error_init;

    err = rtlsdr_set_sample_rate(st->dev, NRSC5_SAMPLE_RATE_CU8);
    if (err) goto error;
    err = rtlsdr_set_tuner_gain_mode(st->dev, 1);
    if (err) goto error;
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

int nrsc5_open_file(nrsc5_t **result, FILE *fp)
{
    nrsc5_t *st = nrsc5_alloc();
    st->iq_file = fp;
    nrsc5_init(st);

    *result = st;
    return 0;
}

int nrsc5_open_pipe(nrsc5_t **result)
{
    nrsc5_t *st = nrsc5_alloc();
    nrsc5_init(st);

    *result = st;
    return 0;
}

int nrsc5_open_rtltcp(nrsc5_t **result, int socket)
{
    int err;
    nrsc5_t *st = nrsc5_alloc();

    st->rtltcp = rtltcp_open(socket);
    if (st->rtltcp == NULL)
        goto error;

    err = rtltcp_set_sample_rate(st->rtltcp, NRSC5_SAMPLE_RATE_CU8);
    if (err) goto error;
    err = rtltcp_set_tuner_gain_mode(st->rtltcp, 1);
    if (err) goto error;
    err = rtltcp_set_offset_tuning(st->rtltcp, 1);
    if (err) goto error;

    nrsc5_init(st);

    *result = st;
    return 0;
error:
    free(st);
    *result = NULL;
    return 1;
}

void nrsc5_close(nrsc5_t *st)
{
    if (!st)
        return;

    if (using_worker(st))
    {
        // signal the worker to exit
        pthread_mutex_lock(&st->worker_mutex);
        st->closed = 1;
        pthread_cond_broadcast(&st->worker_cond);
        pthread_mutex_unlock(&st->worker_mutex);

        // wait for worker to finish
        pthread_join(st->worker, NULL);
    }

    if (st->dev)
        rtlsdr_close(st->dev);
    if (st->iq_file)
        fclose(st->iq_file);
    if (st->rtltcp)
        rtltcp_close(st->rtltcp);

    input_free(&st->input);
    output_free(&st->output);
    free(st);
}

void nrsc5_start(nrsc5_t *st)
{
    if (using_worker(st))
    {
        // signal the worker to start
        pthread_mutex_lock(&st->worker_mutex);
        st->stopped = 0;
        pthread_cond_broadcast(&st->worker_cond);
        pthread_mutex_unlock(&st->worker_mutex);
    }
}

void nrsc5_stop(nrsc5_t *st)
{
    if (using_worker(st))
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
}

int nrsc5_set_mode(nrsc5_t *st, int mode)
{
    if (mode == NRSC5_MODE_FM || mode == NRSC5_MODE_AM)
    {
        st->mode = mode;
        input_set_mode(&st->input);
        return 0;
    }
    return 1;
}

int nrsc5_set_bias_tee(nrsc5_t *st, int on)
{
    if (st->dev)
    {
        int err = rtlsdr_set_bias_tee(st->dev, on);
        if (err)
            return 1;
    }
    else if (st->rtltcp)
    {
        int err = rtltcp_set_bias_tee(st->rtltcp, on);
        if (err)
            return 1;
    }
    return 0;
}

int nrsc5_set_direct_sampling(nrsc5_t *st, int on)
{
    if (st->dev)
    {
        int err = rtlsdr_set_direct_sampling(st->dev, on);
        if (err)
            return 1;
    }
    else if (st->rtltcp)
    {
        int err = rtltcp_set_direct_sampling(st->rtltcp, on);
        if (err)
            return 1;
    }
    return 0;
}

int nrsc5_set_freq_correction(nrsc5_t *st, int ppm_error)
{
    if (st->dev)
    {
        int err = rtlsdr_set_freq_correction(st->dev, ppm_error);
        if (err && err != -2)
            return 1;
    }
    else if (st->rtltcp)
    {
        int err = rtltcp_set_freq_correction(st->rtltcp, ppm_error);
        if (err)
            return 1;
    }
    return 0;
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

    if (st->dev && rtlsdr_set_center_freq(st->dev, freq) != 0)
        return 1;
    if (st->rtltcp && rtltcp_set_center_freq(st->rtltcp, freq) != 0)
        return 1;

    if (st->auto_gain)
        st->gain = -1;
    input_reset(&st->input);
    output_reset(&st->output);

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

    if (st->dev && rtlsdr_set_tuner_gain(st->dev, gain * 10) != 0)
        return 1;
    if (st->rtltcp && rtltcp_set_tuner_gain(st->rtltcp, gain * 10) != 0)
        return 1;

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
    if (using_worker(st))
        pthread_mutex_lock(&st->worker_mutex);
    st->callback = callback;
    st->callback_opaque = opaque;
    if (using_worker(st))
        pthread_mutex_unlock(&st->worker_mutex);
}

int nrsc5_pipe_samples_cu8(nrsc5_t *st, const uint8_t *samples, unsigned int length)
{
    unsigned int sample_groups;

    while (st->leftover_u8_num > 0 && length > 0)
    {
        st->leftover_u8[st->leftover_u8_num++] = samples[0];
        samples++;
        length--;

        if (st->leftover_u8_num == 4) {
            input_push_cu8(&st->input, st->leftover_u8, 4);
            st->leftover_u8_num = 0;
            break;
        }
    }

    sample_groups = length / 4;
    input_push_cu8(&st->input, samples, sample_groups * 4);
    samples += (sample_groups * 4);
    length -= (sample_groups * 4);

    while (length > 0)
    {
        st->leftover_u8[st->leftover_u8_num++] = samples[0];
        samples++;
        length--;
    }

    return 0;
}

int nrsc5_pipe_samples_cs16(nrsc5_t *st, const int16_t *samples, unsigned int length)
{
    unsigned int sample_groups;

    if (st->leftover_s16_num == 1 && length > 0)
    {
        st->leftover_s16[st->leftover_s16_num++] = samples[0];
        samples++;
        length--;

        input_push_cs16(&st->input, st->leftover_s16, 2);
        st->leftover_s16_num = 0;
    }

    sample_groups = length / 2;
    input_push_cs16(&st->input, samples, sample_groups * 2);
    samples += (sample_groups * 2);
    length -= (sample_groups * 2);

    if (length == 1)
        st->leftover_s16[st->leftover_s16_num++] = samples[0];

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

void nrsc5_report_stream(nrsc5_t *st, uint16_t port, uint16_t seq, unsigned int size, uint32_t mime, const uint8_t *data)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_STREAM;
    evt.stream.port = port;
    evt.stream.seq = seq;
    evt.stream.size = size;
    evt.stream.mime = mime;
    evt.stream.data = data;
    nrsc5_report(st, &evt);
}

void nrsc5_report_packet(nrsc5_t *st, uint16_t port, uint16_t seq, unsigned int size, uint32_t mime, const uint8_t *data)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_PACKET;
    evt.packet.port = port;
    evt.packet.seq = seq;
    evt.packet.size = size;
    evt.packet.mime = mime;
    evt.packet.data = data;
    nrsc5_report(st, &evt);
}

void nrsc5_report_lot(nrsc5_t *st, uint16_t port, unsigned int lot, unsigned int size, uint32_t mime, const char *name, const uint8_t *data, struct tm *expiry_utc)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_LOT;
    evt.lot.port = port;
    evt.lot.lot = lot;
    evt.lot.size = size;
    evt.lot.mime = mime;
    evt.lot.name = name;
    evt.lot.data = data;
    evt.lot.expiry_utc = expiry_utc;
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
    nrsc5_sig_service_t *service = NULL;
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_SIG;

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

void nrsc5_report_sis(nrsc5_t *st, const char *country_code, int fcc_facility_id, const char *name,
                      const char *slogan, const char *message, const char *alert,
                      float latitude, float longitude, int altitude, nrsc5_sis_asd_t *audio_services,
                      nrsc5_sis_dsd_t *data_services)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_SIS;
    evt.sis.country_code = country_code;
    evt.sis.fcc_facility_id = fcc_facility_id;
    evt.sis.name = name;
    evt.sis.slogan = slogan;
    evt.sis.message = message;
    evt.sis.alert = alert;
    evt.sis.latitude = latitude;
    evt.sis.longitude = longitude;
    evt.sis.altitude = altitude;
    evt.sis.audio_services = audio_services;
    evt.sis.data_services = data_services;

    nrsc5_report(st, &evt);
}
