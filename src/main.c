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
#include <sys/time.h>
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
#endif

#include "bitwriter.h"
#include "log.h"

#define AUDIO_BUFFERS 128
#define AUDIO_THRESHOLD 8
#define AUDIO_DATA_LENGTH 8192

typedef struct buffer_t {
    struct buffer_t *next;
    // The samples are signed 16-bit integers, but ao_play requires a char buffer.
    char data[AUDIO_DATA_LENGTH];
} audio_buffer_t;

typedef struct {
    float freq;
    int mode;
    float gain;
    unsigned int device_index;
    int bias_tee;
    int direct_sampling;
    int ppm_error;
    char *input_name;
    char *rtltcp_host;
    ao_device *dev;
    FILE *hdc_file;
    FILE *iq_file;
    char *aas_files_path;

    audio_buffer_t *head, *tail, *free;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    unsigned int program;
    unsigned int audio_ready;
    unsigned int audio_packets;
    unsigned int audio_bytes;
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
    struct timespec ts;
    struct timeval now;

    gettimeofday(&now, NULL);
    ts.tv_sec = now.tv_sec;
    ts.tv_nsec = (now.tv_usec + 100000) * 1000;
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_nsec -= 1000000000;
        ts.tv_sec += 1;
    }

    pthread_mutex_lock(&st->mutex);
    if (program != st->program)
        goto unlock;

    while (st->free == NULL)
    {
        if (pthread_cond_timedwait(&st->cond, &st->mutex, &ts) == ETIMEDOUT)
        {
            log_warn("Audio output timed out, dropping samples");
            reset_audio_buffers(st);
        }
    }
    b = st->free;
    st->free = b->next;
    pthread_mutex_unlock(&st->mutex);

    assert(AUDIO_DATA_LENGTH == count * sizeof(data[0]));
    memcpy(b->data, data, count * sizeof(data[0]));

    pthread_mutex_lock(&st->mutex);
    if (program != st->program)
    {
        b->next = st->free;
        st->free = b;
        goto unlock;
    }

    b->next = NULL;
    if (st->tail)
        st->tail->next = b;
    else
        st->head = b;
    st->tail = b;

    if (st->audio_ready < AUDIO_THRESHOLD)
        st->audio_ready++;

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

static void change_program(state_t *st, unsigned int program)
{
    pthread_mutex_lock(&st->mutex);

    // reset audio buffers
    st->audio_ready = 0;
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
            if (st->audio_packets >= 32) {
                log_info("Audio bit rate: %.1f kbps", (float)st->audio_bytes * 8 * NRSC5_SAMPLE_RATE_AUDIO / NRSC5_AUDIO_FRAME_SAMPLES / st->audio_packets / 1000);
                st->audio_packets = 0;
                st->audio_bytes = 0;
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
        st->audio_ready = 0;
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
        log_debug("Stream data: port=%04X seq=%04X mime=%08X size=%d", evt->stream.component->data.port, evt->stream.seq, evt->stream.component->data.mime, evt->stream.size);
        break;
    case NRSC5_EVENT_PACKET:
        log_debug("Packet data: port=%04X seq=%04X mime=%08X size=%d", evt->packet.component->data.port, evt->packet.seq, evt->packet.component->data.mime, evt->packet.size);
        break;
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
                strcat(alert_details, "SAME=[");
                break;
            case NRSC5_LOCATION_FORMAT_FIPS:
                strcat(alert_details, "FIPS=[");
                break;
            case NRSC5_LOCATION_FORMAT_ZIP:
                strcat(alert_details, "ZIP=[");
                break;
            }

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

#ifndef __MINGW32__
static void restore_termios(void *arg)
{
    tcsetattr(STDIN_FILENO, TCSANOW, arg);
}
#endif

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
    pthread_cleanup_push(restore_termios, &prev_termios);
    t = prev_termios;
    t.c_lflag &= ~ICANON;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif

    while (!st->done)
    {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) != 1)
            break;

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

#ifndef __MINGW32__
    // restore terminal settings
    pthread_cleanup_pop(1);
#endif

    return NULL;
}

static void help(const char *progname)
{
    fprintf(stderr, "Usage: %s [-v] [-q] [--am] [-l log-level] [-d device-index] [-H rtltcp-host] [-p ppm-error] [-g gain] [-r iq-input] [-w iq-output] [-o audio-output] [-t audio-type] [-T] [-D direct-sampling-mode] [--dump-hdc hdc-output] [--dump-aas-files directory] frequency program\n", progname);
}

static int parse_args(state_t *st, int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "dump-aas-files", required_argument, NULL, 1 },
        { "dump-hdc", required_argument, NULL, 2 },
        { "am", no_argument, NULL, 3 },
        { 0 }
    };
    const char *version = NULL;
    char *output_name = NULL, *audio_name = NULL, *hdc_name = NULL;
    char *audio_type = "wav";
    char *endptr;
    int opt;

    st->mode = NRSC5_MODE_FM;
    st->gain = -1;
    st->bias_tee = 0;
    st->direct_sampling = -1;
    st->ppm_error = INT_MIN;
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
    if (st->iq_file)
        fclose(st->iq_file);

    free(st->input_name);
    free(st->aas_files_path);

    if (st->dev)
        ao_close(st->dev);
}

int main(int argc, char *argv[])
{
    pthread_mutex_t log_mutex;
    pthread_t input_thread;
    nrsc5_t *radio = NULL;
    state_t *st = calloc(1, sizeof(state_t));

    pthread_mutex_init(&log_mutex, NULL);
    log_set_lock(log_lock);
    log_set_udata(&log_mutex);

    ao_initialize();
    init_audio_buffers(st);
    if (parse_args(st, argc, argv) != 0)
        return 0;

#ifdef __MINGW32__
    SetConsoleOutputCP(CP_UTF8);
    setmode(fileno(stdin), O_BINARY);
    setmode(fileno(stdout), O_BINARY);
#endif

    if (st->input_name)
    {
        FILE *fp = strcmp(st->input_name, "-") == 0 ? stdin : fopen(st->input_name, "rb");
        if (fp == NULL)
        {
            log_fatal("Open IQ file failed.");
            return 1;
        }
        if (nrsc5_open_file(&radio, fp) != 0)
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

    pthread_create(&input_thread, NULL, input_main, st);

    while (1)
    {
        audio_buffer_t *b;

        pthread_mutex_lock(&st->mutex);
        while (!st->done && (st->head == NULL || st->audio_ready < AUDIO_THRESHOLD))
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

    pthread_cancel(input_thread);
    pthread_join(input_thread, NULL);

    nrsc5_stop(radio);
    nrsc5_set_bias_tee(radio, 0);
    nrsc5_close(radio);
    cleanup(st);
    free(st);
    ao_shutdown();
    return 0;
}
