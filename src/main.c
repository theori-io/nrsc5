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
#include <errno.h>
#endif

#include "miniaudio.h"
#include "dr_wav.h"
#include "bitwriter.h"
#include "log.h"

#define AUDIO_CHANNELS 2
#define AUDIO_BUFFERS 16
#define FILE_BUFFER_LENGTH 32768
#define STDIN_POLL_RATE_MS 100

enum iq_format {
    IQ_FORMAT_NONE,
    IQ_FORMAT_CU8,
    IQ_FORMAT_CS16,
    IQ_FORMAT_CF32,
};

enum output_format
{
    OUTPUT_FORMAT_WAV,
    OUTPUT_FORMAT_RAW,
    OUTPUT_FORMAT_DEVICE,
};

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
    enum output_format out_format;
    ma_device dev;
    drwav wav;
    FILE *audio_file;
    FILE *hdc_file;
    FILE *iq_file;
    char *aas_files_path;
    enum iq_format iq_input_format;

    ma_pcm_rb buffer;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

#ifdef __MINGW32__
    HANDLE hStdin;
#else
    struct pollfd pfd;
#endif

    unsigned int program;
    unsigned int audio_packets_valid;
    unsigned int audio_packets;
    unsigned int audio_bytes;
    unsigned int audio_errors;
    int done;
} state_t;

static void push_audio_buffer(state_t *st, unsigned int program, const int16_t *data, size_t count, unsigned int flags)
{
    const ma_uint32 frames = count / AUDIO_CHANNELS;

    pthread_mutex_lock(&st->mutex);
    unsigned int prog = st->program;
    pthread_mutex_unlock(&st->mutex);

    if (program != prog)
        return;

    if (flags & NRSC5_AUDIO_FLAGS_DECODING_ERROR)
        log_warn("Audio decoding error");

    if (st->out_format == OUTPUT_FORMAT_RAW)
    {
        fwrite(data, count, sizeof(int16_t), st->audio_file);
    }
    else if (st->out_format == OUTPUT_FORMAT_WAV)
    {
        const unsigned int framesWritten = drwav_write_pcm_frames(&st->wav, frames, data);
        if (framesWritten < frames)
            log_error("Failed to write audio");
    }
    else
    {
        ma_uint32 frames_written = 0;
        ma_result result;

        while (frames_written < frames)
        {
            void* buffer;
            ma_uint32 frames_to_write = frames - frames_written;

            result = ma_pcm_rb_acquire_write(&st->buffer, &frames_to_write, &buffer);
            if (result != MA_SUCCESS) {
                break;
            }
            if (frames_to_write == 0) {
                break;
            }

            /* Copy the data from the capture buffer to the ring buffer. */
            memcpy(buffer, data + frames_written * AUDIO_CHANNELS, sizeof(int16_t) * frames_to_write * AUDIO_CHANNELS);

            result = ma_pcm_rb_commit_write(&st->buffer, frames_to_write);
            if (result != MA_SUCCESS) {
                break;
            }

            frames_written += frames_to_write;
        }
        if (frames_written < frames) {
            log_warn("Audio output queue full, dropping samples");
        }
    }
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
        push_audio_buffer(st, evt->audio.program, evt->audio.data, evt->audio.count, evt->audio.flags);
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

void audio_callback(ma_device* pDevice, void* p_output, const void* p_input, ma_uint32 frame_count)
{
    state_t* st = pDevice->pUserData;
    (void)p_input;
    ma_result result;
    ma_uint32 frame_read = 0;

    while (frame_read < frame_count)
    {
        ma_uint32 frames_to_read = frame_count - frame_read;
        void* buffer;

        result = ma_pcm_rb_acquire_read(&st->buffer, &frames_to_read, &buffer);
        if (result != MA_SUCCESS) {
            break;
        }
        if (frames_to_read == 0) {
            break;
        }

        /* Copy the data from the capture buffer to the ring buffer. */
        memcpy((int16_t*)p_output + frame_read * AUDIO_CHANNELS, buffer, frames_to_read * AUDIO_CHANNELS * sizeof(int16_t));

        result = ma_pcm_rb_commit_read(&st->buffer, frames_to_read);
        if (result != MA_SUCCESS) {
            break;
        }

        frame_read += frames_to_read;
    }
    if (frame_read < frame_count)
    {
        const ma_uint32 frames_left = frame_count - frame_read;
        memset((int16_t*)p_output + frame_read * AUDIO_CHANNELS, 0, sizeof(int16_t) * AUDIO_CHANNELS * frames_left);
    }
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

static void read_input(state_t *st, const unsigned int wait_time)
{
#ifdef __MINGW32__
    INPUT_RECORD r;
    DWORD read;

    switch (WaitForSingleObject(st->hStdin, wait_time))
    {
    case WAIT_TIMEOUT:
        break;
    case WAIT_OBJECT_0:
        if (!ReadConsoleInput(st->hStdin, &r, 1, &read))
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
    int ret = poll(&st->pfd, 1, wait_time);
    char ch;

    if (ret > 0)
    {
        if (st->pfd.revents & POLLIN)
        {
            if (read(STDIN_FILENO, &ch, 1))
                on_key_press(st, ch);
        }
    }
    else if (ret < 0)
    {
        log_error("Stdin read failed: poll error %d", errno);
    }
#endif
}

static void help(const char *progname)
{
    fprintf(stderr, "Usage: %s [-v] [-q] [--am] [-l log-level] [-d device-index] [-H rtltcp-host] [-p ppm-error] [-g gain] [-r iq-input] [--iq-input-format {cu8,cs16}] [-w iq-output] [-o audio-output] [-t audio-type] [-T] [-D direct-sampling-mode] [--dump-hdc hdc-output] [--dump-aas-files directory] frequency program\n", progname);
}

static int ends_with(const char *str, const char *suffix)
{
    const size_t len = strlen(str);
    const size_t suffix_len = strlen(suffix);
    return (len >= suffix_len) && (strcmp(str + len - suffix_len, suffix) == 0);
}

size_t file_write(void* pUserData, const void* pData, size_t bytesToWrite)
{
    return fwrite(pData, 1, bytesToWrite, pUserData);
}

drwav_bool32 file_seek(void* pUserData, int offset, drwav_seek_origin origin)
{
    int whence;

    if (origin == DRWAV_SEEK_SET) {
        whence = SEEK_SET;
    } else if (origin == DRWAV_SEEK_END) {
        whence = SEEK_END;
    } else {
        whence = SEEK_CUR;
    }
    fseek(pUserData, offset, whence);
    return 1;
}

static int parse_args(state_t *st, int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "dump-aas-files", required_argument, NULL, 1 },
        { "dump-hdc", required_argument, NULL, 2 },
        { "am", no_argument, NULL, 3 },
        { "iq-input-format", required_argument, NULL, 4 },
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
    {
        if (strcmp(audio_name, "-") == 0)
            st->audio_file = stdout;
        else
            st->audio_file = fopen(audio_name, "wb");
        if (st->audio_file == NULL)
        {
            log_fatal("Unable to open file output.");
            return 1;
        }

        if (strcmp(audio_type, "wav") == 0)
        {
            drwav_data_format format;
            format.container = drwav_container_rf64;
            format.format = DR_WAVE_FORMAT_PCM;
            format.channels = AUDIO_CHANNELS;
            format.sampleRate = NRSC5_SAMPLE_RATE_AUDIO;
            format.bitsPerSample = 16;
            if (!drwav_init_write(&st->wav, &format, file_write, file_seek, st->audio_file, NULL))
            {
                log_fatal("Unable to open encoder");
                return -1;
            }

            st->out_format = OUTPUT_FORMAT_WAV;
        }
        else
        {
            st->out_format = OUTPUT_FORMAT_RAW;
        }
    }
    else
    {
        ma_result result;
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format   = ma_format_s16;
        config.playback.channels = AUDIO_CHANNELS;
        config.sampleRate        = NRSC5_SAMPLE_RATE_AUDIO;
        config.dataCallback      = audio_callback;
        config.pUserData         = st;
        config.periodSizeInFrames = NRSC5_AUDIO_FRAME_SAMPLES; /* provide a hint of the size of each frame */
        config.noFixedSizedCallback = 1;
        config.noPreSilencedOutputBuffer = 1;

        result = ma_device_init(NULL, &config, &st->dev);
        if (result != MA_SUCCESS) {
            log_fatal("Unable to open audio device: %s", ma_result_description(result));
            return -1;  // Failed to initialize the device.
        }

        result = ma_pcm_rb_init(config.playback.format, config.playback.channels, NRSC5_AUDIO_FRAME_SAMPLES * AUDIO_BUFFERS, NULL, NULL, &st->buffer);
        if (result != MA_SUCCESS) {
            log_fatal("Failed to allocate ring buffer: %s", ma_result_description(result));
            return -1; // Failed to initialize the device.
        }

        pthread_cond_init(&st->cond, NULL);
        pthread_mutex_init(&st->mutex, NULL);

        st->out_format = OUTPUT_FORMAT_DEVICE;
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
    if (st->hdc_file)
        fclose(st->hdc_file);
    if (st->iq_file)
        fclose(st->iq_file);

    free(st->input_name);
    free(st->aas_files_path);

    if (st->out_format == OUTPUT_FORMAT_DEVICE)
    {
        ma_pcm_rb_uninit(&st->buffer);
        ma_device_uninit(&st->dev);
    }
    else
    {
        drwav_uninit(&st->wav);
    }

    if (st->audio_file)
        fclose(st->audio_file);
}

static int read_more_input(state_t *st)
{
    const unsigned int len = NRSC5_AUDIO_FRAME_SAMPLES * (st->mode == NRSC5_MODE_FM ? 2 : 4);
    return st->out_format != OUTPUT_FORMAT_DEVICE ||
           ma_pcm_rb_available_write(&st->buffer) > len;
}

static int is_playback_done(state_t *st)
{
    return st->out_format != OUTPUT_FORMAT_DEVICE ||
           ma_pcm_rb_available_read(&st->buffer) == 0;
}

int main(int argc, char *argv[])
{
    pthread_mutex_t log_mutex;
    nrsc5_t *radio = NULL;
    state_t *st = calloc(1, sizeof(state_t));
    FILE *fp = NULL;

    pthread_mutex_init(&log_mutex, NULL);
    log_set_lock(log_lock);
    log_set_udata(&log_mutex);

    if (parse_args(st, argc, argv) != 0)
        return 0;

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

    if (st->out_format == OUTPUT_FORMAT_DEVICE)
    {
        ma_result result;
        if ((result = ma_device_start(&st->dev)) != MA_SUCCESS)
        {
            log_fatal("Device start failed: %s", ma_result_description(result));
            return 1;
        }
    }

#ifndef __MINGW32__
    struct termios prev_termios, t;
#endif

    if (isatty(STDIN_FILENO))
    {
#ifdef __MINGW32__
        st->hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(st->hStdin, &mode);
        SetConsoleMode(st->hStdin, mode & (~ENABLE_ECHO_INPUT) & (~ENABLE_LINE_INPUT));
#else
        // disable terminal canonical mode
        tcgetattr(STDIN_FILENO, &prev_termios);
        t = prev_termios;
        t.c_lflag &= ~ICANON;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);

        st->pfd.fd = STDIN_FILENO;
        st->pfd.events = POLLIN;
#endif
    }

    while (!is_done(st))
    {
        const int can_feed = st->input_name && read_more_input(st);
        const int wait_time = can_feed ? 0 : STDIN_POLL_RATE_MS;

        if (isatty(STDIN_FILENO))
        {
            read_input(st, wait_time);
        }
        else if (wait_time > 0)
        {
            struct timespec delay = {
                .tv_sec = wait_time / 1000,
                .tv_nsec = (wait_time % 1000) * 1000000L,
            };

            nanosleep(&delay, NULL);
        }

        if (st->input_name && read_more_input(st))
        {
            uint8_t buffer[FILE_BUFFER_LENGTH];
            size_t samples_read = 0;

            if (st->iq_input_format == IQ_FORMAT_CU8) {
                samples_read = fread(buffer, 2, sizeof(buffer) / 2, fp);
            } else if (st->iq_input_format == IQ_FORMAT_CS16) {
                samples_read = fread(buffer, 4, sizeof(buffer) / 4, fp);
            } else if (st->iq_input_format == IQ_FORMAT_CF32) {
                samples_read = fread(buffer, 8, sizeof(buffer) / 8, fp);
            }

            if (samples_read == 0 && is_playback_done(st))
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

#ifndef __MINGW32__
    if (isatty(STDIN_FILENO))
    {
        // restore terminal settings
        tcsetattr(STDIN_FILENO, TCSANOW, &prev_termios);
    }
#endif

    nrsc5_stop(radio);
    nrsc5_set_bias_tee(radio, 0);
    nrsc5_close(radio);

    if (st->input_name)
    {
        fclose(fp);
    }

    cleanup(st);
    free(st);
    return 0;
}
