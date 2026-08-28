/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ao/ao.h>
#include <getopt.h>
#include <math.h>
#include <nrsc5.h>
#include <pthread.h>
#include <unistd.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef __MINGW32__
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <termios.h>
#include <poll.h>
#endif

#include "bitwriter.h"
#include "log.h"

#define AUDIO_BUFFERS 16
#define AUDIO_DATA_LENGTH 8192
#define FILE_BUFFER_LENGTH 32768
#define STDIN_POLL_RATE_MS 100

typedef struct buffer_t {
    struct buffer_t *next;
    // The samples are signed 16-bit integers, but ao_play requires a char buffer.
    char data[AUDIO_DATA_LENGTH];
} audio_buffer_t;

enum iq_format {
    IQ_FORMAT_NONE,
    IQ_FORMAT_CU8,
    IQ_FORMAT_CS16,
    IQ_FORMAT_CF32,
};

typedef struct {
    float freq;
    int mode;
    float gain;
    unsigned int device_index;
    int bias_tee;
    int direct_sampling;
    int decode_weather;
    int decode_traffic;
    int ppm_error;
    char *input_name;
    char *rtltcp_host;
    ao_device *dev;
    FILE *hdc_file;
    FILE *packet_file;
    FILE *iq_file;
    nrsc5_location_table_t *location_table;
    char *aas_files_path;
    enum iq_format iq_input_format;

    audio_buffer_t *head, *tail, *free;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    unsigned int program;
    unsigned int audio_packets_valid;
    unsigned int audio_packets;
    unsigned int audio_bytes;
    unsigned int audio_errors;
    uint32_t navteq_alternate_frequencies[16];
    int done;
} state_t;

static ao_sample_format sample_format = {
    16,
    NRSC5_SAMPLE_RATE_AUDIO,
    2,
    AO_FMT_NATIVE,
    "L,R"
};

static ao_device *open_ao_live(void)
{
    return ao_open_live(ao_default_driver_id(), &sample_format, NULL);
}

static ao_device *open_ao_file(const char *name, const char *type)
{
    return ao_open_file(ao_driver_id(type), name, 1, &sample_format, NULL);
}

static void reset_audio_buffers(state_t *st)
{
    audio_buffer_t *b;

    // find the end of the head list
    for (b = st->head; b && b->next; b = b->next) { }

    // if the head list is non-empty, prepend to free list
    if (b != NULL)
    {
        b->next = st->free;
        st->free = st->head;
    }

    st->head = NULL;
    st->tail = NULL;
}

static void push_audio_buffer(state_t *st, unsigned int program, const int16_t *data, size_t count)
{
    audio_buffer_t *b;

    pthread_mutex_lock(&st->mutex);
    if (program != st->program)
        goto unlock;

    if (st->input_name)
    {
        while (st->free == NULL)
            pthread_cond_wait(&st->cond, &st->mutex);
    }
    else
    {
        if (st->free == NULL)
        {
            log_warn("Audio output queue full, dropping samples");
            goto unlock;
        }
    }

    b = st->free;
    st->free = b->next;

    assert(AUDIO_DATA_LENGTH == count * sizeof(data[0]));
    memcpy(b->data, data, count * sizeof(data[0]));

    b->next = NULL;
    if (st->tail)
        st->tail->next = b;
    else
        st->head = b;
    st->tail = b;

    pthread_cond_signal(&st->cond);

unlock:
    pthread_mutex_unlock(&st->mutex);
}

static void init_audio_buffers(state_t *st)
{
    st->head = NULL;
    st->tail = NULL;
    st->free = NULL;

    for (int i = 0; i < AUDIO_BUFFERS; ++i)
    {
        audio_buffer_t *b = malloc(sizeof(audio_buffer_t));
        b->next = st->free;
        st->free = b;
    }

    pthread_cond_init(&st->cond, NULL);
    pthread_mutex_init(&st->mutex, NULL);
}

static void write_adts_header(FILE *fp, unsigned int len)
{
    uint8_t hdr[7];
    bitwriter_t bw;

    bw_init(&bw, hdr);
    bw_addbits(&bw, 0xFFF, 12); // sync word
    bw_addbits(&bw, 0, 1); // MPEG-4
    bw_addbits(&bw, 0, 2); // Layer
    bw_addbits(&bw, 1, 1); // no CRC
    bw_addbits(&bw, 1, 2); // AAC-LC
    bw_addbits(&bw, 7, 4); // 22050 HZ
    bw_addbits(&bw, 0, 1); // private bit
    bw_addbits(&bw, 2, 3); // 2-channel configuration
    bw_addbits(&bw, 0, 1);
    bw_addbits(&bw, 0, 1);
    bw_addbits(&bw, 0, 1);
    bw_addbits(&bw, 0, 1);
    bw_addbits(&bw, len + 7, 13); // frame length
    bw_addbits(&bw, 0x7FF, 11); // buffer fullness (VBR)
    bw_addbits(&bw, 0, 2); // 1 AAC frame per ADTS frame

    fwrite(hdr, 7, 1, fp);
}

static void dump_hdc(FILE *fp, const uint8_t *pkt, unsigned int len)
{
    write_adts_header(fp, len);
    fwrite(pkt, len, 1, fp);
    fflush(fp);
}

static void log_packet_data(const char *kind, uint16_t port, uint16_t seq,
                            uint32_t mime, const uint8_t *data, unsigned int size)
{
    char *hex = malloc(2 * (size_t) size + 1);

    if (hex == NULL)
        return;
    for (unsigned int i = 0; i < size; i++)
        sprintf(hex + 2 * i, "%02x", data[i]);
    hex[2 * (size_t) size] = '\0';
    log_debug("%s data: port=%04X seq=%04X mime=%08X size=%d data_hex=%s",
              kind, port, seq, mime, size, hex);
    free(hex);
}

static const char *traffic_mime_name(uint32_t mime)
{
    switch (mime)
    {
    case NRSC5_MIME_NAVTEQ: return "NAVTEQ";
    case NRSC5_MIME_HERE_TPEG: return "HERE_TPEG";
    case NRSC5_MIME_HERE_IMAGE: return "HERE_IMAGE";
    case NRSC5_MIME_HD_TMC: return "HD_TMC";
    case NRSC5_MIME_TTN_TPEG_1: return "TTN_TPEG_1";
    case NRSC5_MIME_TTN_TPEG_2: return "TTN_TPEG_2";
    case NRSC5_MIME_TTN_TPEG_3: return "TTN_TPEG_3";
    case NRSC5_MIME_TTN_STM_TRAFFIC: return "TTN_STM_TRAFFIC";
    case NRSC5_MIME_TTN_STM_WEATHER: return "TTN_STM_WEATHER";
    default: return NULL;
    }
}

static void dump_packets(FILE *fp, const char *kind, uint16_t port, uint16_t seq,
                         uint32_t mime, const uint8_t *data, unsigned int size)
{
    const char *mime_name = traffic_mime_name(mime);

    if (mime_name == NULL)
        mime_name = "UNKNOWN";
    fprintf(fp, "{\"kind\":\"%s\",\"port\":%u,\"seq\":%u,\"mime\":\"%s\",\"mime_id\":\"%08x\",\"data_hex\":\"",
            kind, port, seq, mime_name, mime);
    for (unsigned int i = 0; i < size; i++)
        fprintf(fp, "%02x", data[i]);
    fprintf(fp, "\"}\n");
    fflush(fp);
}

static void dump_aas_file(state_t *st, const nrsc5_event_t *evt)
{
#if defined(WIN32) || defined(_WIN32)
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

    const char *name;
    const uint8_t *data;
    unsigned int size;
    unsigned int number;

    switch (evt->event)
    {
    case NRSC5_EVENT_LOT:
        name = evt->lot.name;
        data = evt->lot.data;
        size = evt->lot.size;
        number = evt->lot.lot;
        break;
    case NRSC5_EVENT_HERE_IMAGE:
        name = evt->here_image.name;
        data = evt->here_image.data;
        size = evt->here_image.size;
#if defined(WIN32) || defined(_WIN32)
        number = _mkgmtime64(evt->here_image.time_utc);
#else
        number = timegm(evt->here_image.time_utc);
#endif
        break;
    default:
        log_error("invalid event type");
        return;
    }

    char fullpath[strlen(st->aas_files_path) + strlen(name) + 16];
    FILE *fp;

    sprintf(fullpath, "%s" PATH_SEPARATOR "%u_%s", st->aas_files_path, number, name);
    fp = fopen(fullpath, "wb");
    if (fp == NULL)
    {
        log_warn("Failed to open %s (%d)", fullpath, errno);
        return;
    }
    fwrite(data, 1, size, fp);
    fclose(fp);
}

static void dump_ber(float cber)
{
    static float min = 1, max = 0, sum = 0, count = 0;
    sum += cber;
    count += 1;
    if (cber < min) min = cber;
    if (cber > max) max = cber;
    log_info("BER: %f, avg: %f, min: %f, max: %f", cber, sum / count, min, max);
}

static void done_signal(state_t *st)
{
    pthread_mutex_lock(&st->mutex);
    st->done = 1;
    pthread_cond_signal(&st->cond);
    pthread_mutex_unlock(&st->mutex);
}

static int is_done(state_t *st)
{
    int done;

    pthread_mutex_lock(&st->mutex);
    done = st->done;
    pthread_mutex_unlock(&st->mutex);

    return done;
}

static void change_program(state_t *st, unsigned int program)
{
    pthread_mutex_lock(&st->mutex);

    // reset audio buffers
    if (st->tail)
    {
        st->tail->next = st->free;
        st->free = st->head;
        st->head = st->tail = NULL;
    }
    // update current program
    st->program = program;

    pthread_mutex_unlock(&st->mutex);
}

static void log_navteq_digital_traffic_entry(
    state_t *st, const nrsc5_navteq_digital_traffic_entry_t *entry)
{
    char event_description[512];
    nrsc5_location_t point, endpoint;
    uint16_t visited[32];
    unsigned int resolved_extent = 0;
    int found;

    nrsc5_alert_c_event_description(entry->event, entry->quantifier,
                                    event_description, sizeof(event_description));
    found = st->location_table == NULL ? 0
          : nrsc5_location_table_lookup(st->location_table, entry->country_code,
                                        entry->location_table_number, entry->location,
                                        &point);
    if (found == 1)
    {
        endpoint = point;
        visited[0] = point.location;
        while (resolved_extent < entry->extent)
        {
            uint16_t next = entry->direction ? endpoint.negative : endpoint.positive;
            nrsc5_location_t next_point;
            unsigned int i;
            if (next == 0)
                break;
            for (i = 0; i <= resolved_extent; i++)
            {
                if (visited[i] == next)
                    break;
            }
            if (i <= resolved_extent
                || nrsc5_location_table_lookup(st->location_table, entry->country_code,
                                               entry->location_table_number, next,
                                               &next_point) != 1)
                break;
            endpoint = next_point;
            resolved_extent++;
            visited[resolved_extent] = endpoint.location;
        }
        log_debug("  NAVTEQ Digital Traffic entry: type=%d country=%d reserved=%d ltn=%d location=%d direction=%s extent=%d bidirectional=%d diversion=%d duration_type=%d control_code=%d event=%d quantifier=%d description=\"%s\" name=\"%s\" lat=%.5f lon=%.5f span_to=%d span_name=\"%s\" span_lat=%.5f span_lon=%.5f resolved_extent=%d/%d",
                   entry->record_type, entry->country_code, entry->reserved,
                   entry->location_table_number, entry->location,
                   entry->direction ? "negative" : "positive", entry->extent,
                   entry->bidirectional, entry->diversion, entry->duration_type,
                   entry->control_code, entry->event, entry->quantifier,
                   event_description, point.name, point.latitude, point.longitude, endpoint.location,
                   endpoint.name, endpoint.latitude, endpoint.longitude,
                   resolved_extent, entry->extent);
    }
    else
    {
        log_debug("  NAVTEQ Digital Traffic entry: type=%d country=%d reserved=%d ltn=%d location=%d direction=%s extent=%d bidirectional=%d diversion=%d duration_type=%d control_code=%d event=%d quantifier=%d description=\"%s\"",
                   entry->record_type, entry->country_code, entry->reserved,
                   entry->location_table_number, entry->location,
                   entry->direction ? "negative" : "positive", entry->extent,
                   entry->bidirectional, entry->diversion, entry->duration_type,
                   entry->control_code, entry->event, entry->quantifier,
                   event_description);
    }
}

static void callback(const nrsc5_event_t *evt, void *opaque)
{
    state_t *st = opaque;
    nrsc5_sig_service_t *sig_service;
    nrsc5_sig_component_t *sig_component;
    nrsc5_id3_comment_t *comment;
    const char *name;
    char time_str[64];

    switch (evt->event)
    {
    case NRSC5_EVENT_LOST_DEVICE:
        done_signal(st);
        break;
    case NRSC5_EVENT_AGC:
        if (evt->agc.is_final)
            log_info("Best gain: %.1f dB, Peak amplitude: %.1f dBFS", evt->agc.gain_db, evt->agc.peak_dbfs);
        else
            log_debug("Gain: %.1f dB, Peak amplitude: %.1f dBFS", evt->agc.gain_db, evt->agc.peak_dbfs);
        break;
    case NRSC5_EVENT_BER:
        dump_ber(evt->ber.cber);
        break;
    case NRSC5_EVENT_MER:
        log_info("MER: %.1f dB (lower), %.1f dB (upper)", evt->mer.lower, evt->mer.upper);
        break;
    case NRSC5_EVENT_IQ:
        if (st->iq_file)
            fwrite(evt->iq.data, 1, evt->iq.count, st->iq_file);
        break;
    case NRSC5_EVENT_HDC:
        if (evt->hdc.program == st->program)
        {
            if (st->hdc_file)
                dump_hdc(st->hdc_file, evt->hdc.data, evt->hdc.count);

            st->audio_packets++;
            st->audio_bytes += evt->hdc.count * sizeof(evt->hdc.data[0]);
            if (evt->hdc.flags & NRSC5_PKT_FLAGS_CRC_ERROR)
                st->audio_errors++;
            else
                st->audio_packets_valid++;

            if (st->audio_packets_valid >= 32) {
                log_info("Audio bit rate: %.1f kbps", (float)st->audio_bytes * 8 * NRSC5_SAMPLE_RATE_AUDIO / NRSC5_AUDIO_FRAME_SAMPLES / st->audio_packets_valid / 1000);
                st->audio_packets_valid = 0;
                st->audio_bytes = 0;
            }
            if (st->audio_packets >= 32)
            {
                if (st->audio_errors > 0)
                    log_warn("Audio packet CRC mismatches: %d", st->audio_errors);
                st->audio_packets = 0;
                st->audio_errors = 0;
            }
        }
        break;
    case NRSC5_EVENT_AUDIO:
        push_audio_buffer(st, evt->audio.program, evt->audio.data, evt->audio.count);
        break;
    case NRSC5_EVENT_SYNC:
        log_info("Synchronized");
        log_info("Frequency offset: %.0f Hz", evt->sync.freq_offset);
        log_info("Primary service mode: %d", evt->sync.psmi);
        if (evt->sync.pli != -1)
        {
            char am_flags[128] = "";
            strcat(am_flags, "Digital bandwidth: ");
            strcat(am_flags, evt->sync.rdbi ? "reduced" : "full");
            if (!evt->sync.rdbi)
            {
                if (evt->sync.psmi != 2)
                {
                    strcat(am_flags, ", analog bandwidth: ");
                    strcat(am_flags, evt->sync.aabi ? "8 kHz" : "5 kHz");
                    strcat(am_flags, ", secondary/tertiary power: ");
                    strcat(am_flags, evt->sync.pli ? "high" : "low");
                }
                strcat(am_flags, ", PIDS power: ");
                strcat(am_flags, evt->sync.hppi ? "high" : "low");
            }
            log_info(am_flags);
        }
        break;
    case NRSC5_EVENT_LOST_SYNC:
        log_info("Lost synchronization");
        break;
    case NRSC5_EVENT_ID3:
        if (evt->id3.program == st->program)
        {
            if (evt->id3.title)
                log_info("Title: %s", evt->id3.title);
            if (evt->id3.artist)
                log_info("Artist: %s", evt->id3.artist);
            if (evt->id3.album)
                log_info("Album: %s", evt->id3.album);
            if (evt->id3.genre)
                log_info("Genre: %s", evt->id3.genre);
            for (comment = evt->id3.comments; comment != NULL; comment = comment->next)
                log_info("Comment: lang=%s %s %s", comment->lang, comment->short_content_desc, comment->full_text);
            if (evt->id3.ufid.owner)
                log_info("Unique file identifier: %s %s", evt->id3.ufid.owner, evt->id3.ufid.id);
            if (evt->id3.xhdr.param >= 0)
                log_info("XHDR: %d %08X %d", evt->id3.xhdr.param, evt->id3.xhdr.mime, evt->id3.xhdr.lot);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d", evt->id3.commercial.valid_until);
            if (evt->id3.commercial.price)
                log_info("Commercial: price=%s until=%s url=\"%s\" seller=\"%s\" desc=\"%s\" received_as=%d",
                    evt->id3.commercial.price, time_str, evt->id3.commercial.contact_url, evt->id3.commercial.seller,
                    evt->id3.commercial.description, evt->id3.commercial.received_as);
        }
        break;
    case NRSC5_EVENT_SIG:
        for (sig_service = evt->sig.services; sig_service != NULL; sig_service = sig_service->next)
        {
            log_info("SIG Service: type=%s number=%d name=%s",
                     sig_service->type == NRSC5_SIG_SERVICE_AUDIO ? "audio" : "data",
                     sig_service->number, sig_service->name);

            for (sig_component = sig_service->components; sig_component != NULL; sig_component = sig_component->next)
            {
                if (sig_component->type == NRSC5_SIG_SERVICE_AUDIO)
                {
                    log_info("  Audio component: id=%d port=%04X type=%d mime=%08X", sig_component->id,
                             sig_component->audio.port, sig_component->audio.type, sig_component->audio.mime);
                }
                else if (sig_component->type == NRSC5_SIG_SERVICE_DATA)
                {
                    log_info("  Data component: id=%d port=%04X service_data_type=%d type=%d mime=%08X",
                             sig_component->id, sig_component->data.port, sig_component->data.service_data_type,
                             sig_component->data.type, sig_component->data.mime);
                }
            }
        }
        break;
    case NRSC5_EVENT_STREAM:
        log_packet_data("Stream", evt->stream.component->data.port, evt->stream.seq,
                        evt->stream.component->data.mime, evt->stream.data, evt->stream.size);
        if (st->packet_file)
            dump_packets(st->packet_file, "stream", evt->stream.component->data.port,
                         evt->stream.seq, evt->stream.component->data.mime,
                         evt->stream.data, evt->stream.size);
        break;
    case NRSC5_EVENT_PACKET:
        log_packet_data("Packet", evt->packet.component->data.port, evt->packet.seq,
                        evt->packet.component->data.mime, evt->packet.data, evt->packet.size);
        if (st->packet_file)
            dump_packets(st->packet_file, "packet", evt->packet.component->data.port,
                         evt->packet.seq, evt->packet.component->data.mime,
                         evt->packet.data, evt->packet.size);
        break;
    case NRSC5_EVENT_NAVTEQ_DIGITAL_TRAFFIC:
        log_debug("NAVTEQ Digital Traffic: port=%04X seq=%04X generation=%d terminal=%d entries=%d",
                  evt->navteq_digital_traffic.port, evt->navteq_digital_traffic.seq,
                  evt->navteq_digital_traffic.generation,
                  evt->navteq_digital_traffic.is_terminal,
                  evt->navteq_digital_traffic.count);
        for (unsigned int i = 0; i < evt->navteq_digital_traffic.count; i++)
        {
            const nrsc5_navteq_digital_traffic_entry_t *entry
                = &evt->navteq_digital_traffic.entries[i];
            log_navteq_digital_traffic_entry(st, entry);
            if (st->decode_traffic)
            {
                char description[512];
                nrsc5_location_t point;
                nrsc5_alert_c_event_description(entry->event, entry->quantifier,
                                                description, sizeof(description));
                printf("{\"kind\":\"navteq_digital_traffic\",\"port\":%u,\"seq\":%u,\"event\":%u,\"location\":%u,\"extent\":%u,\"description\":",
                       evt->navteq_digital_traffic.port, evt->navteq_digital_traffic.seq,
                       entry->event, entry->location, entry->extent);
                printf("\"%s\"", description);
                if (st->location_table
                    && nrsc5_location_table_lookup(st->location_table, entry->country_code,
                                                   entry->location_table_number, entry->location,
                                                   &point) == 1)
                    printf(",\"location_name\":\"%s\",\"latitude\":%.7f,\"longitude\":%.7f",
                           point.name, point.latitude, point.longitude);
                printf("}\n");
                fflush(stdout);
            }
        }
        break;
    case NRSC5_EVENT_NAVTEQ_ALTERNATE_FREQUENCIES:
        for (unsigned int i = 0; i < evt->navteq_alternate_frequencies.count; i++)
        {
            const nrsc5_navteq_alternate_frequency_entry_t *entry
                = &evt->navteq_alternate_frequencies.entries[i];
            if (entry->index < 16
                && st->navteq_alternate_frequencies[entry->index] != entry->frequency_hz)
            {
                st->navteq_alternate_frequencies[entry->index] = entry->frequency_hz;
                log_info("NAVTEQ alternate frequency: index=%d frequency=%.1f MHz",
                         entry->index, entry->frequency_hz / 1000000.0);
                if (st->decode_traffic)
                    printf("{\"kind\":\"navteq_alternate_frequency\",\"port\":%u,\"seq\":%u,\"index\":%u,\"frequency_hz\":%u}\n",
                           evt->navteq_alternate_frequencies.port,
                           evt->navteq_alternate_frequencies.seq, entry->index,
                           entry->frequency_hz);
                fflush(stdout);
            }
        }
        break;
    case NRSC5_EVENT_TTN_TEC:
    {
        const nrsc5_ttn_tec_t *event = &evt->ttn_tec.tec;
        log_info("TTN TEC: message=%u location=%u event=%u cause=%u",
                 event->message_id, event->location, event->effect_code,
                 event->cause_code);
        if (st->decode_traffic)
        {
            printf("{\"kind\":\"ttn_tec\",\"message_id\":%u,\"version\":%u,\"expiry_time\":%u,\"cancel\":%d,\"location\":%u,"
                   "\"country_code\":%u,\"location_table_number\":%u,"
                   "\"effect\":%u,\"cause\":%u,\"warning_level\":%u,"
                   "\"direction_positive\":%d,\"both_directions\":%d,\"extent\":%u",
                   event->message_id, event->version, event->expiry_time, event->cancel,
                   event->location, event->country_code,
                   event->location_table_number, event->effect_code,
                   event->cause_code, event->warning_level,
                   event->direction_positive, event->both_directions, event->extent);
            if (event->location_resolved)
                printf(",\"location_name\":\"%s\",\"latitude\":%.7f,\"longitude\":%.7f",
                       event->resolved_location.name,
                       event->resolved_location.latitude,
                       event->resolved_location.longitude);
            printf("}\n");
            fflush(stdout);
        }
        break;
    }
    case NRSC5_EVENT_HERE_TFP:
    {
        const nrsc5_here_tfp_t *flow = &evt->here_tfp.flow;
        log_info("HERE TFP: message=%u location=%u LOS=%d speed=%d km/h",
                 flow->message_id, flow->location, flow->level_of_service,
                 flow->average_speed);
        if (st->decode_traffic)
        {
            printf("{\"kind\":\"here_tfp\",\"message_id\":%u,\"version\":%u,"
                   "\"expiry_time\":%u,\"cancel\":%d,\"start_time\":%u,"
                   "\"duration\":%d,\"spatial_resolution\":%u,\"polygon_index\":%u,"
                   "\"level_of_service\":%d,\"average_speed_kph\":%d,"
                   "\"free_flow_travel_time\":%d,\"delay\":%d,\"location\":%u,"
                   "\"country_code\":%u,\"location_table_number\":%u,"
                   "\"direction_positive\":%d,\"both_directions\":%d,\"extent\":%u",
                   flow->message_id, flow->version, flow->expiry_time, flow->cancel,
                   flow->start_time, flow->duration, flow->spatial_resolution,
                   flow->polygon_index, flow->level_of_service, flow->average_speed,
                   flow->free_flow_travel_time, flow->delay, flow->location,
                   flow->country_code, flow->location_table_number,
                   flow->direction_positive, flow->both_directions, flow->extent);
            if (flow->location_resolved)
                printf(",\"location_name\":\"%s\",\"latitude\":%.7f,\"longitude\":%.7f",
                       flow->resolved_location.name,
                       flow->resolved_location.latitude,
                       flow->resolved_location.longitude);
            printf("}\n");
            fflush(stdout);
        }
        break;
    }
    case NRSC5_EVENT_LOT:
        if (st->aas_files_path)
            dump_aas_file(st, evt);
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", evt->lot.expiry_utc);
        log_info("LOT file: port=%04X lot=%d name=%s size=%d mime=%08X expiry=%s", evt->lot.component->data.port, evt->lot.lot, evt->lot.name, evt->lot.size, evt->lot.mime, time_str);
        break;
    case NRSC5_EVENT_LOT_HEADER:
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", evt->lot.expiry_utc);
        log_debug("LOT header: port=%04X lot=%d name=%s size=%d mime=%08X expiry=%s",
                  evt->lot.component->data.port, evt->lot.lot, evt->lot.name, evt->lot.size, evt->lot.mime, time_str);
        break;
    case NRSC5_EVENT_LOT_FRAGMENT:
        if (!evt->lot_fragment.is_duplicate)
            log_debug("LOT fragment: port=%04X lot=%d seq=%d repeat=%d size=%d bytes_so_far=%d",
                      evt->lot_fragment.component->data.port, evt->lot_fragment.lot, evt->lot_fragment.seq,
                      evt->lot_fragment.repeat, evt->lot_fragment.size, evt->lot_fragment.bytes_so_far);
        break;
    case NRSC5_EVENT_STATION_ID:
        log_info("Country: %s, FCC facility ID: %d", evt->station_id.country_code, evt->station_id.fcc_facility_id);
        break;
    case NRSC5_EVENT_STATION_NAME:
        log_info("Station name: %s", evt->station_name.name);
        break;
    case NRSC5_EVENT_STATION_SLOGAN:
        log_info("Slogan: %s", evt->station_slogan.slogan);
        break;
    case NRSC5_EVENT_STATION_MESSAGE:
        log_info("Message: %s", evt->station_message.message);
        break;
    case NRSC5_EVENT_STATION_LOCATION:
        log_info("Station location: %.4f, %.4f, %dm", evt->station_location.latitude, evt->station_location.longitude, evt->station_location.altitude);
        break;
    case NRSC5_EVENT_AUDIO_SERVICE_DESCRIPTOR:
        nrsc5_program_type_name(evt->asd.type, &name);
        log_info("Audio program %d: %s, type: %s, sound experience %d",
                    evt->asd.program,
                    evt->asd.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                    name, evt->asd.sound_exp);
        break;
    case NRSC5_EVENT_DATA_SERVICE_DESCRIPTOR:
        nrsc5_service_data_type_name(evt->dsd.type, &name);
        log_info("Data service: %s, type: %s, MIME type %03x",
                    evt->dsd.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                    name, evt->dsd.mime_type);
        break;
    case NRSC5_EVENT_EMERGENCY_ALERT:
        if (evt->emergency_alert.message)
        {
            int i;
            char alert_details[512] = "";
            const char *name = NULL;

            strcat(alert_details, "Category=[");
            if (evt->emergency_alert.category1 >= 1)
            {
                nrsc5_alert_category_name(evt->emergency_alert.category1, &name);
                strcat(alert_details, name);
            }
            if (evt->emergency_alert.category2 >= 1)
            {
                nrsc5_alert_category_name(evt->emergency_alert.category2, &name);
                strcat(alert_details, ", ");
                strcat(alert_details, name);
            }
            strcat(alert_details, "] ");

            switch (evt->emergency_alert.location_format)
            {
            case NRSC5_LOCATION_FORMAT_SAME:
                strcat(alert_details, "SAME=");
                break;
            case NRSC5_LOCATION_FORMAT_FIPS:
                strcat(alert_details, "FIPS=");
                break;
            case NRSC5_LOCATION_FORMAT_ZIP:
                strcat(alert_details, "ZIP=");
                break;
            }

            strcat(alert_details, "[");
            for (i = 0; i < evt->emergency_alert.num_locations; i++)
            {
                if (i > 0)
                    strcat(alert_details, ", ");
                sprintf(alert_details + strlen(alert_details), "%d", evt->emergency_alert.locations[i]);
            }
            strcat(alert_details, "]");

            log_info("Alert: %s %s", alert_details, evt->emergency_alert.message);
        }
        else
            log_info("Alert ended");
        break;
    case NRSC5_EVENT_AUDIO_SERVICE:
        nrsc5_program_type_name(evt->audio_service.type, &name);
        log_info("Audio service %d: %s, type: %s, codec: %d, blend: %d, gain: %d dB, delay: %d, latency: %d",
                evt->audio_service.program,
                evt->audio_service.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                name,
                evt->audio_service.codec_mode,
                evt->audio_service.blend_control,
                evt->audio_service.digital_audio_gain,
                evt->audio_service.common_delay,
                evt->audio_service.latency);
        break;
    case NRSC5_EVENT_HERE_IMAGE:
        if (st->aas_files_path)
            dump_aas_file(st, evt);
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", evt->here_image.time_utc);
        log_info("HERE Image: type=%s, seq=%d, n1=%d, n2=%d, time=%s, lat1=%.5f, lon1=%.5f, lat2=%.5f, lon2=%.5f, name=%s, size=%d",
                 evt->here_image.image_type == NRSC5_HERE_IMAGE_TRAFFIC ? "TRAFFIC" : "WEATHER",
                 evt->here_image.seq,
                 evt->here_image.n1,
                 evt->here_image.n2,
                 time_str,
                 evt->here_image.latitude1,
                 evt->here_image.longitude1,
                 evt->here_image.latitude2,
                 evt->here_image.longitude2,
                 evt->here_image.name,
                 evt->here_image.size);
        break;
    case NRSC5_EVENT_EXCITER_INFO:
        log_debug("Exciter manuf. \"%s\", core version %d.%d.%d.%d, core status %d, manuf. version %d.%d.%d.%d, manuf. status %d, importer connected? %s",
                  evt->exciter_info.manufacturer_id,
                  evt->exciter_info.core_version[0], evt->exciter_info.core_version[1], evt->exciter_info.core_version[2],
                  evt->exciter_info.core_version[3], evt->exciter_info.core_status,
                  evt->exciter_info.manufacturer_version[0], evt->exciter_info.manufacturer_version[1], evt->exciter_info.manufacturer_version[2],
                  evt->exciter_info.manufacturer_version[3], evt->exciter_info.manufacturer_status,
                  evt->exciter_info.importer_connected ? "yes" : "no");
        break;
    case NRSC5_EVENT_IMPORTER_INFO:
        log_debug("Importer manuf. \"%s\", core version %d.%d.%d.%d, core status %d, manuf. version %d.%d.%d.%d, manuf. status %d",
                  evt->importer_info.manufacturer_id,
                  evt->importer_info.core_version[0], evt->importer_info.core_version[1], evt->importer_info.core_version[2],
                  evt->importer_info.core_version[3], evt->importer_info.core_status,
                  evt->importer_info.manufacturer_version[0], evt->importer_info.manufacturer_version[1], evt->importer_info.manufacturer_version[2],
                  evt->importer_info.manufacturer_version[3], evt->importer_info.manufacturer_status);
        break;
    case NRSC5_EVENT_LEAP_SECOND_OFFSET:
        log_debug("Leap second offset: pending=%d, current=%d, ALFN of pending adjustment=%d",
                 evt->leap_second_offset.pending_offset,
                 evt->leap_second_offset.current_offset,
                 evt->leap_second_offset.pending_alfn);
        break;
    case NRSC5_EVENT_LOCAL_TIME:
        log_debug("Local time: UTC offset=%d minutes, DST schedule=%d, DST in effect regionally? %s, DST practiced locally? %s",
                  evt->local_time.utc_offset,
                  evt->local_time.dst_schedule,
                  evt->local_time.dst_regional ? "yes" : "no",
                  evt->local_time.dst_local ? "yes" : "no");
        break;
    case NRSC5_EVENT_TTN_CITY_DATABASE:
        log_info("TTN city database: version=%d timestamp=%d cities=%d",
                 evt->ttn_city_database.database.database_version,
                 evt->ttn_city_database.database.timestamp,
                 evt->ttn_city_database.database.count);
        if (st->decode_weather)
        {
            printf("{\"kind\":\"ttn_city_database\",\"version\":%u,\"timestamp\":%u,\"cities\":%u}\n",
                   evt->ttn_city_database.database.database_version,
                   evt->ttn_city_database.database.timestamp,
                   evt->ttn_city_database.database.count);
            fflush(stdout);
        }
        break;
    case NRSC5_EVENT_TTN_WEATHER:
        log_info("TTN weather: timestamp=%d cities=%d",
                 evt->ttn_weather.timestamp, evt->ttn_weather.count);
        if (st->decode_weather)
        {
            for (unsigned int i = 0; i < evt->ttn_weather.count; i++)
            {
                const char *condition_name;
                const nrsc5_ttn_weather_city_t *city = &evt->ttn_weather.cities[i];
                printf("{\"kind\":\"ttn_weather\",\"timestamp\":%u,\"city_id\":%u,\"provider_city_id\":%u,\"latitude\":%.7f,\"longitude\":%.7f,\"name\":",
                       evt->ttn_weather.timestamp, city->city.city_id,
                       city->city.provider_city_id, city->city.latitude,
                       city->city.longitude);
                printf("\"%s\"", city->city.name ? city->city.name : "");
                printf(",\"short_forecasts\":[");
                for (unsigned int j = 0; j < city->short_forecast_count; j++)
                {
                    const nrsc5_ttn_short_forecast_t *forecast = &city->short_forecasts[j];
                    nrsc5_ttn_weather_condition_name(forecast->weather_condition,
                                                     &condition_name);
                    if (j) printf(",");
                    printf("{\"offset_hours\":%u,\"weather_condition\":%u,\"weather_condition_name\":\"%s\",\"wind_direction\":%u,\"wind_speed_mph\":%u,\"temperature_f\":%d,\"feels_like_temperature_f\":%d,\"humidity_percent\":%u,\"chance_of_precipitation_percent\":%u}",
                           forecast->offset_hours, forecast->weather_condition,
                           condition_name ? condition_name : "",
                           forecast->wind_direction, forecast->wind_speed,
                           forecast->temperature, forecast->feels_like_temperature,
                           forecast->humidity, forecast->chance_of_precipitation);
                }
                printf("],\"long_forecasts\":[");
                for (unsigned int j = 0; j < city->long_forecast_count; j++)
                {
                    const nrsc5_ttn_long_forecast_t *forecast = &city->long_forecasts[j];
                    nrsc5_ttn_weather_condition_name(forecast->weather_condition,
                                                     &condition_name);
                    if (j) printf(",");
                    printf("{\"offset_days\":%u,\"weather_condition\":%u,\"weather_condition_name\":\"%s\",\"wind_direction\":%u,\"wind_speed_mph\":%u,\"high_temperature_f\":%d,\"low_temperature_f\":%d,\"chance_of_precipitation_percent\":%u,\"external_feels_like_temperature_f\":%d,\"maximum_humidity_percent\":%u}",
                           forecast->offset_days, forecast->weather_condition,
                           condition_name ? condition_name : "",
                           forecast->wind_direction, forecast->wind_speed,
                           forecast->high_temperature, forecast->low_temperature,
                           forecast->chance_of_precipitation,
                           forecast->external_feels_like_temperature,
                           forecast->maximum_humidity);
                }
                printf("]}\n");
            }
            fflush(stdout);
        }
        break;
    case NRSC5_EVENT_TTN_SERVICE_NETWORK:
        log_info("TTN service/network: component_id=%u timestamp=%u provider=%s bearers=%u",
                 evt->ttn_service_network.service_network.service_component_id,
                 evt->ttn_service_network.service_network.timestamp,
                 evt->ttn_service_network.service_network.service_provider_name
                     ? evt->ttn_service_network.service_network.service_provider_name : "",
                 evt->ttn_service_network.service_network.bearer_count);
        break;

    }
}

static int connect_tcp(char *host, const char *default_port)
{
    int err, s;
    struct addrinfo hints, *res0;
    char *p = strchr(host, ':');

#ifdef __MINGW32__
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
        return -1;
#endif

    if (p)
    {
        *p = 0;
        default_port = p + 1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    err = getaddrinfo(host, default_port, &hints, &res0);
    if (err)
        return -1;

    for (struct addrinfo *res = res0; res != NULL; res = res->ai_next)
    {
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == -1)
            continue;

        if (connect(s, res->ai_addr, res->ai_addrlen) == 0)
            break;

        // failed, try next address
        close(s);
        s = -1;
    }

    freeaddrinfo(res0);
    return s;
}

static void *audio_main(void *arg)
{
    state_t *st = arg;

    while (1)
    {
        audio_buffer_t *b;

        pthread_mutex_lock(&st->mutex);
        while (!st->done && (st->head == NULL))
            pthread_cond_wait(&st->cond, &st->mutex);

        // exit once done and no more audio buffers
        if (st->head == NULL)
        {
            pthread_mutex_unlock(&st->mutex);
            break;
        }

        // unlink from head list
        b = st->head;
        st->head = b->next;
        if (st->head == NULL)
            st->tail = NULL;
        pthread_mutex_unlock(&st->mutex);

        ao_play(st->dev, b->data, sizeof(b->data));

        pthread_mutex_lock(&st->mutex);
        // add to free list
        b->next = st->free;
        st->free = b;
        pthread_cond_signal(&st->cond);
        pthread_mutex_unlock(&st->mutex);
    }

    return NULL;
}

static void on_key_press(state_t *st, char ch)
{
    switch (ch)
    {
    case 'q':
        done_signal(st);
        // user wants to immediately exit, so reset audio buffer
        change_program(st, -1);
        break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        change_program(st, ch - '0');
        break;
    }
}

static void *input_main(void *arg)
{
    state_t *st = arg;

    if (!isatty(STDIN_FILENO))
        return NULL;

#ifdef __MINGW32__
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT) & (~ENABLE_LINE_INPUT));
#else
    struct termios prev_termios, t;

    // disable terminal canonical mode
    tcgetattr(STDIN_FILENO, &prev_termios);
    t = prev_termios;
    t.c_lflag &= ~ICANON;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);

    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
#endif

    while (!is_done(st))
    {
#ifdef __MINGW32__
        INPUT_RECORD r;
        DWORD read;

        switch (WaitForSingleObject(hStdin, STDIN_POLL_RATE_MS))
        {
        case WAIT_TIMEOUT:
            continue;
        case WAIT_OBJECT_0:
            if (!ReadConsoleInput(hStdin, &r, 1, &read))
            {
                log_error("Stdin read failed: ReadConsoleInput error %d", GetLastError());
                break;
            }

            const KEY_EVENT_RECORD key = r.Event.KeyEvent;

            if (r.EventType == KEY_EVENT && key.bKeyDown)
                on_key_press(st, key.uChar.AsciiChar);
            break;
        case WAIT_ABANDONED:
            log_error("Waiting for stdin failed: WAIT_ABANDONED");
            break;
        case WAIT_FAILED:
            log_error("Waiting for stdin failed: WAIT_FAILED");
            break;
        default:
            break;
        }

#else
        int ret = poll(&pfd, 1, STDIN_POLL_RATE_MS);
        char ch;

        if (ret > 0)
        {
            if (pfd.revents & POLLIN)
            {
                if (read(STDIN_FILENO, &ch, 1))
                    on_key_press(st, ch);
            }
        }
        else if (ret == 0)
            continue;
        else
        {
            log_error("Stdin read failed: poll error %d", errno);
            break;
        }
#endif
    }

#ifndef __MINGW32__
    // restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &prev_termios);
#endif

    return NULL;
}

static void help(const char *progname)
{
    fprintf(stderr, "Usage: %s [-v] [-q] [--am] [-l log-level] [-d device-index] [-H rtltcp-host] [-p ppm-error] [-g gain] [-r iq-input] [--iq-input-format {cu8,cs16}] [-w iq-output] [-o audio-output] [-t audio-type] [-T] [-D direct-sampling-mode] [--dump-hdc hdc-output] [--decode-weather] [--decode-traffic] [--dump-packets packet-output] [--location-table file] [--dump-aas-files directory] frequency program\n", progname);
}

static int ends_with(const char *str, const char *suffix)
{
    const size_t len = strlen(str);
    const size_t suffix_len = strlen(suffix);
    return (len >= suffix_len) && (strcmp(str + len - suffix_len, suffix) == 0);
}

static int parse_args(state_t *st, int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "dump-aas-files", required_argument, NULL, 1 },
        { "dump-hdc", required_argument, NULL, 2 },
        { "am", no_argument, NULL, 3 },
        { "iq-input-format", required_argument, NULL, 4 },
        { "decode-traffic", no_argument, NULL, 5 },
        { "location-table", required_argument, NULL, 6 },
        { "dump-packets", required_argument, NULL, 7 },
        { "decode-weather", no_argument, NULL, 8 },
        { 0 }
    };
    const char *version = NULL;
    char *output_name = NULL, *audio_name = NULL, *hdc_name = NULL;
    char *packet_name = NULL;
    char *location_table_name = NULL;
    char *audio_type = "wav";
    char *endptr;
    int opt;

    st->mode = NRSC5_MODE_FM;
    st->gain = -1;
    st->bias_tee = 0;
    st->direct_sampling = -1;
    st->ppm_error = INT_MIN;
    st->iq_input_format = IQ_FORMAT_NONE;
    log_set_level(LOG_INFO);

    while ((opt = getopt_long(argc, argv, "r:w:o:t:d:p:g:ql:vH:TD:", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 1:
            st->aas_files_path = strdup(optarg);
            break;
        case 2:
            hdc_name = optarg;
            break;
        case 3:
            st->mode = NRSC5_MODE_AM;
            break;
        case 4:
            if (strcmp(optarg, "cu8") == 0)
            {
                st->iq_input_format = IQ_FORMAT_CU8;
            }
            else if (strcmp(optarg, "cs16") == 0)
            {
                st->iq_input_format = IQ_FORMAT_CS16;
            }
            else if (strcmp(optarg, "cf32") == 0)
            {
                st->iq_input_format = IQ_FORMAT_CF32;
            }
            else
            {
                log_fatal("I/Q input format must be either cu8, cs16 or cf32.");
                return -1;
            }
            break;
        case 5:
            st->decode_traffic = 1;
            break;
        case 6:
            location_table_name = optarg;
            break;
        case 7:
            packet_name = optarg;
            break;
        case 8:
            st->decode_weather = 1;
            break;
        case 'r':
            st->input_name = strdup(optarg);
            break;
        case 'w':
            output_name = optarg;
            break;
        case 'o':
            audio_name = optarg;
            break;
        case 't':
            if ((strcmp(optarg, "wav") != 0) && (strcmp(optarg, "raw") != 0))
            {
                log_fatal("Audio type must be either wav or raw.");
                return -1;
            }
            audio_type = optarg;
            break;
        case 'd':
            st->device_index = strtoul(optarg, NULL, 10);
            break;
        case 'p':
            st->ppm_error = strtol(optarg, NULL, 10);
            break;
        case 'g':
            st->gain = strtof(optarg, &endptr);
            if (*endptr != 0)
            {
                log_fatal("Invalid gain.");
                return -1;
            }
            break;
        case 'q':
            log_set_quiet(1);
            break;
        case 'l':
            log_set_level(atoi(optarg));
            break;
        case 'v':
            nrsc5_get_version(&version);
            printf("nrsc5 revision %s\n", version);
            return 1;
        case 'H':
            st->rtltcp_host = strdup(optarg);
            break;
        case 'T':
            st->bias_tee = 1;
            break;
        case 'D':
            st->direct_sampling = atoi(optarg);
            break;
        default:
            help(argv[0]);
            return 1;
        }
    }

    if (optind + (!st->input_name + 1) != argc)
    {
        help(argv[0]);
        return 1;
    }

    if (!st->input_name)
    {
        st->freq = strtof(argv[optind++], &endptr);
        if (*endptr != 0)
        {
            log_fatal("Invalid frequency.");
            return -1;
        }

        // compatibility with previous versions
        if (st->freq < 10000.0f)
            st->freq *= 1e6f;
    }

    if (st->input_name && (st->iq_input_format == IQ_FORMAT_NONE))
    {
        if (ends_with(st->input_name, ".cs16"))
            st->iq_input_format = IQ_FORMAT_CS16;
        else if (ends_with(st->input_name, ".cf32"))
            st->iq_input_format = IQ_FORMAT_CF32;
        else
            st->iq_input_format = IQ_FORMAT_CU8;
    }

    st->program = strtoul(argv[optind++], &endptr, 0);
    if (*endptr != 0)
    {
        log_fatal("Invalid program.");
        return -1;
    }

    if (audio_name)
        st->dev = open_ao_file(audio_name, audio_type);
    else
        st->dev = open_ao_live();

    if (st->dev == NULL)
    {
        log_fatal("Unable to open audio device.");
        return 1;
    }

    if (output_name)
    {
        if (strcmp(output_name, "-") == 0)
            st->iq_file = stdout;
        else
            st->iq_file = fopen(output_name, "wb");
        if (st->iq_file == NULL)
        {
            log_fatal("Unable to open IQ output.");
            return 1;
        }
    }

    if (hdc_name)
    {
        if (strcmp(hdc_name, "-") == 0)
            st->hdc_file = stdout;
        else
            st->hdc_file = fopen(hdc_name, "wb");
        if (st->hdc_file == NULL)
        {
            log_fatal("Unable to open HDC output.");
            return 1;
        }
    }

    if (packet_name)
    {
        if (strcmp(packet_name, "-") == 0)
            st->packet_file = stdout;
        else
            st->packet_file = fopen(packet_name, "w");
        if (st->packet_file == NULL)
        {
            log_fatal("Unable to open packet output.");
            return 1;
        }
    }

    if (location_table_name
        && nrsc5_location_table_open(&st->location_table, location_table_name) != 0)
    {
        log_fatal("Unable to open location table %s: %s",
                  location_table_name, strerror(errno));
        return 1;
    }
    return 0;
}

static void log_lock(void *udata, int lock)
{
    pthread_mutex_t *mutex = udata;
    if (lock)
        pthread_mutex_lock(mutex);
    else
        pthread_mutex_unlock(mutex);
}

static void cleanup(state_t *st)
{
    reset_audio_buffers(st);
    while (st->free)
    {
        audio_buffer_t *b = st->free;
        st->free = b->next;
        free(b);
    }

    if (st->hdc_file)
        fclose(st->hdc_file);
    if (st->packet_file)
        fclose(st->packet_file);
    if (st->iq_file)
        fclose(st->iq_file);
    nrsc5_location_table_close(st->location_table);

    free(st->input_name);
    free(st->aas_files_path);

    if (st->dev)
        ao_close(st->dev);
}

int main(int argc, char *argv[])
{
    pthread_mutex_t log_mutex;
    pthread_t audio_thread;
    pthread_t input_thread;
    nrsc5_t *radio = NULL;
    state_t *st = calloc(1, sizeof(state_t));
    FILE *fp = NULL;

    pthread_mutex_init(&log_mutex, NULL);
    log_set_lock(log_lock);
    log_set_udata(&log_mutex);

    ao_initialize();
    init_audio_buffers(st);
    int parse_result = parse_args(st, argc, argv);
    if (parse_result != 0)
    {
        cleanup(st);
        free(st);
        ao_shutdown();
        pthread_mutex_destroy(&log_mutex);
        return parse_result;
    }

#ifdef __MINGW32__
    SetConsoleOutputCP(CP_UTF8);
    setmode(fileno(stdin), O_BINARY);
    setmode(fileno(stdout), O_BINARY);
#endif

    if (st->input_name)
    {
        fp = strcmp(st->input_name, "-") == 0 ? stdin : fopen(st->input_name, "rb");
        if (fp == NULL)
        {
            log_fatal("Open IQ file failed: %s", strerror(errno));
            return 1;
        }
        if (nrsc5_open_pipe(&radio) != 0)
        {
            log_fatal("Open IQ failed.");
            return 1;
        }
    }
    else if (st->rtltcp_host)
    {
        int s = connect_tcp(st->rtltcp_host, "1234");
        if (s == -1)
        {
            log_fatal("Connection failed.");
            return 1;
        }
        if (nrsc5_open_rtltcp(&radio, s) != 0)
        {
            log_fatal("Open remote device failed.");
            return 1;
        }
    }
    else
    {
        if (nrsc5_open(&radio, st->device_index) != 0)
        {
            log_fatal("Open device failed.");
            return 1;
        }
    }
    nrsc5_set_location_table(radio, st->location_table);
    if (nrsc5_set_bias_tee(radio, st->bias_tee) != 0)
    {
        log_fatal("Set bias-T failed.");
        return 1;
    }
    if (st->direct_sampling != -1)
    {
        if (nrsc5_set_direct_sampling(radio, st->direct_sampling) != 0)
        {
            log_fatal("Set direct sampling failed.");
            return 1;
        }
    }
    if (st->ppm_error != INT_MIN && nrsc5_set_freq_correction(radio, st->ppm_error) != 0)
    {
        log_fatal("Set frequency correction failed.");
        return 1;
    }
    if (nrsc5_set_frequency(radio, st->freq) != 0)
    {
        log_fatal("Set frequency failed.");
        return 1;
    }
    nrsc5_set_mode(radio, st->mode);
    if (st->gain >= 0.0f)
        nrsc5_set_gain(radio, st->gain);
    nrsc5_set_callback(radio, callback, st);
    nrsc5_start(radio);

    pthread_create(&audio_thread, NULL, audio_main, st);
    pthread_create(&input_thread, NULL, input_main, st);

    if (st->input_name)
    {
        uint8_t buffer[FILE_BUFFER_LENGTH];

        while (!is_done(st))
        {
            size_t samples_read = 0;
            
            if (st->iq_input_format == IQ_FORMAT_CU8) {
                samples_read = fread(buffer, 2, sizeof(buffer) / 2, fp);
            } else if (st->iq_input_format == IQ_FORMAT_CS16) {
                samples_read = fread(buffer, 4, sizeof(buffer) / 4, fp);
            } else if (st->iq_input_format == IQ_FORMAT_CF32) {
                samples_read = fread(buffer, 8, sizeof(buffer) / 8, fp);
            }

            if (samples_read == 0)
            {
                done_signal(st);
                break;
            }

            if (st->iq_input_format == IQ_FORMAT_CU8) {
                nrsc5_pipe_samples_cu8(radio, buffer, samples_read * 2);
            } else if (st->iq_input_format == IQ_FORMAT_CS16) {
                nrsc5_pipe_samples_cs16(radio, (int16_t *)buffer, samples_read * 2);
            } else if (st->iq_input_format == IQ_FORMAT_CF32) {
                nrsc5_pipe_samples_cf32(radio, (float *)buffer, samples_read * 2);
            }
        }
    }

    pthread_join(audio_thread, NULL);
    pthread_join(input_thread, NULL);

    nrsc5_stop(radio);
    nrsc5_set_bias_tee(radio, 0);
    nrsc5_close(radio);

    if (st->input_name)
    {
        fclose(fp);
    }

    cleanup(st);
    free(st);
    ao_shutdown();
    return 0;
}
