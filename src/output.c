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

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "defines.h"
#include "output.h"
#include "private.h"
#include "unicode.h"

#define RADIO_FRAME_SAMPLES_FM (NRSC5_AUDIO_FRAME_SAMPLES * 135 / 8)
#define RADIO_FRAME_SAMPLES_AM (NRSC5_AUDIO_FRAME_SAMPLES * 135 / 256)

static unsigned int average_acquire_samples(output_t *st, elastic_buffer_t* dec)
{
    return dec->avg * (st->radio->mode == NRSC5_MODE_FM ? RADIO_FRAME_SAMPLES_FM : RADIO_FRAME_SAMPLES_AM);
}

static unsigned int compute_forward_sequence_position(elastic_buffer_t *elastic, unsigned int seq)
{
    return (seq - elastic->ptr[elastic->write].seq) % MAX_AUDIO_PACKETS;
}

static void elastic_realign_forward(elastic_buffer_t *elastic, unsigned int forward, unsigned int pdu_seq, unsigned int avg, unsigned int seq)
{
    elastic->write = (elastic->write + forward) % elastic->size;
    elastic->ptr[elastic->write].seq = seq;

    unsigned int offset = pdu_seq * avg;
    if (((offset + 64 - seq) % MAX_AUDIO_PACKETS) < 32)
        offset = (offset + 32) % MAX_AUDIO_PACKETS;

    elastic->read = (elastic->write - elastic->delay - seq + offset) % elastic->size;
}

static unsigned int elastic_writable(elastic_buffer_t *elastic)
{
    if (elastic->read > elastic->write)
        return (elastic->read - elastic->write) - 1;
    else
        return elastic->size - (elastic->write - elastic->read) - 1;
}

static void elastic_decode_packet(output_t *st, unsigned int program, int16_t** audio, unsigned int* frames)
{
    decoder_t *dec = &st->decoder[program];
    elastic_buffer_t *elastic = &dec->elastic_buffer;

    if (elastic->ptr[elastic->read].size != 0)
    {
        nrsc5_report_hdc(st->radio, program, elastic->ptr[elastic->read].data, elastic->ptr[elastic->read].size);

#ifdef USE_FAAD2
        NeAACDecFrameInfo info;
        void *buffer;

        if (!dec->aacdec)
        {
            unsigned long samprate = 22050;
            NeAACDecInitHDC(&dec->aacdec, &samprate);
        }

        buffer = NeAACDecDecode(dec->aacdec, &info, elastic->ptr[elastic->read].data,
                                elastic->ptr[elastic->read].size);
        if (info.error > 0)
            log_error("Decode error: %s", NeAACDecGetErrorMessage(info.error));

        if (info.error > 0 || info.samples == 0)
        {
            *audio = st->silence;
            *frames = AUDIO_FRAME_LENGTH;
        }
        else
        {
            assert(info.samples == AUDIO_FRAME_LENGTH);
            *audio = buffer;
            *frames = info.samples;
        }
    }
    else
    {
        *audio = st->silence;
        *frames = AUDIO_FRAME_LENGTH;

        // Reset decoder. Missing packets.
        if (dec->aacdec)
        {
            NeAACDecClose(dec->aacdec);
            dec->aacdec = NULL;
        }
#endif
    }

    elastic->ptr[elastic->read].size = 0;
    elastic->read = (elastic->read + 1) % elastic->size;
}

void output_align(output_t *st, unsigned int program, unsigned int stream_id, unsigned int pdu_seq, unsigned int latency, unsigned int avg, unsigned int seq, unsigned int nop)
{
    decoder_t *dec = &st->decoder[program];
    elastic_buffer_t *elastic = &dec->elastic_buffer;
    unsigned int forward;

    if (stream_id != 0)
        return; // TODO: Process enhanced stream

    elastic->latency = latency * 2;
    elastic->avg = avg;

    // Create Elastic buffer
    if (!elastic->ptr)
    {
        // Buffer Diagram: ||delay|| + ||64|| + ||delay||
        elastic->delay = elastic->latency;
        elastic->size  = (elastic->delay * 2) + MAX_AUDIO_PACKETS;
        elastic->ptr   = malloc(elastic->size * sizeof(*elastic->ptr));

        for (int i = 0; i < elastic->size; i++)
        {
            elastic->ptr[i].size = 0;
            elastic->ptr[i].seq = -1;
        }

        // Startup the buffer
        elastic->write = elastic->delay;

        // Startup the clock
        elastic->clock = average_acquire_samples(st, elastic);

        // Align Writer (buffer->delay + seq) & Reader
        elastic_realign_forward(elastic, seq, pdu_seq, avg, seq);

        log_debug("Elastic buffer created. Program: %d, Size %d bytes, Read %d pos, Write: %d pos", program, elastic->size, elastic->read, elastic->write);
    }

    if(!dec->output_buffer)
    {
        dec->output_buffer = malloc(OUTPUT_BUFFER_LENGTH * sizeof(*dec->output_buffer));
        memset(dec->output_buffer, 0, OUTPUT_BUFFER_LENGTH * sizeof(*dec->output_buffer));

        // FFT decode delay
        dec->write = ((st->radio->mode == NRSC5_MODE_FM ? FFTCP_FM : FFTCP_AM) * 8 / 135) * AUDIO_FRAME_CHANNELS;
    }

    // Re-sync (lost-synchronization with reader and writer)
    forward = compute_forward_sequence_position(elastic, seq);
    if (elastic_writable(elastic) < forward + nop)
    {
        elastic_realign_forward(elastic, forward, pdu_seq, avg, seq);
        log_debug("Elastic buffer realigned. Program: %d, Read %d pos, Write: %d pos", program, elastic->read, elastic->write);
    }
}

static unsigned int output_buffer_writeable(decoder_t *st)
{
    if (st->read > st->write)
        return (st->read - st->write) - 1;
    else
        return OUTPUT_BUFFER_LENGTH - (st->write - st->read) - 1;
}

unsigned int output_buffer_available(decoder_t *st)
{
    if (st->write >= st->read)
        return st->write - st->read;
    else
        return OUTPUT_BUFFER_LENGTH - (st->read - st->write);
}

static void output_buffer_write(decoder_t *dec, int16_t *buffer, unsigned int samples)
{
    if (output_buffer_writeable(dec) < samples)
    {
        log_error("Internal writer clock bug. Full of samples");
        return;
    }

    if (dec->write + samples > OUTPUT_BUFFER_LENGTH)
    {
        unsigned int len = OUTPUT_BUFFER_LENGTH - dec->write;
        memcpy(dec->output_buffer + dec->write, buffer, len * sizeof(*buffer));
        memcpy(dec->output_buffer, buffer + len, (samples - len) * sizeof(*buffer));
        dec->write = samples - len;
    }
    else
    {
        memcpy(dec->output_buffer + dec->write, buffer, samples * sizeof(*buffer));
        dec->write = (dec->write + samples) % OUTPUT_BUFFER_LENGTH;
    }
}

static void output_read_buffer(decoder_t *dec, int16_t *buffer, unsigned int samples)
{
    if (dec->read + samples > OUTPUT_BUFFER_LENGTH)
    {
        unsigned int first = OUTPUT_BUFFER_LENGTH - dec->read;
        memcpy(buffer, &dec->output_buffer[dec->read], first * sizeof(*buffer));
        memcpy(&buffer[first], dec->output_buffer, (samples - first) * sizeof(*buffer));
        dec->read = samples - first;
    }
    else
    {
        memcpy(buffer, &dec->output_buffer[dec->read], samples * sizeof(*buffer));
        dec->read = (dec->read + samples) % OUTPUT_BUFFER_LENGTH;
    }
}

void output_advance_elastic(output_t *st, int pos, unsigned int used)
{
    for (int i = 0; i < MAX_PROGRAMS; i++)
    {
        decoder_t *dec = &st->decoder[i];
        elastic_buffer_t *elastic = &dec->elastic_buffer;
        unsigned int sample_avg = average_acquire_samples(st, elastic);

        // Skip if no buffer
        if (!elastic->ptr)
            continue;

        if (elastic->pos == -1)
            elastic->pos = pos;

        // Packet clock
        elastic->clock += (int)used;

        // Decode packets based on average
        while (elastic->clock >= (int)sample_avg)
        {
            int16_t *audio;
            unsigned int decoded_frames;

            for (int j = 0; j < elastic->avg; j++)
            {
                elastic_decode_packet(st, i, &audio, &decoded_frames);
#ifdef USE_FAAD2
                output_buffer_write(dec, audio, decoded_frames);
#endif
            }
            elastic->clock -= (int)sample_avg;
        }

    }
}

void output_advance(output_t *st, unsigned int len)
{
    for (int i = 0; i < MAX_PROGRAMS; i++)
    {
        decoder_t *dec = &st->decoder[i];
        elastic_buffer_t *elastic = &dec->elastic_buffer;

        // Skip if no buffer
        if (elastic->write == 0)
            continue;

        unsigned int hd_samples;
        unsigned int delay_samples = 0;

        // Program started in the middle of the sample.
        // Insert silence to makeup for it. It takes time to generate samples
        if (elastic->pos > 0)
        {
            hd_samples = (len - elastic->pos);
            delay_samples += (len - hd_samples);
        }
        else
        {
            hd_samples = len;
        }

        unsigned int iq_upper = (hd_samples * 8) + dec->leftover;
        unsigned int audio_frames = (iq_upper / 135) * AUDIO_FRAME_CHANNELS;
        unsigned int silence_upper = (delay_samples * 8);
        unsigned int silence_frames = (silence_upper / 135) * AUDIO_FRAME_CHANNELS;
        unsigned int frame_len = audio_frames + silence_frames;

        int16_t* audio_frame = malloc(frame_len * sizeof(*audio_frame));
        memset(audio_frame, 0, silence_frames * sizeof(*audio_frame));

        if (output_buffer_available(dec) < audio_frames)
        {
            log_error("Internal reader clock bug. Missing samples. "
                      "Requested: %d Available: %d", audio_frames, output_buffer_available(dec));
            audio_frames = output_buffer_available(dec);
        }

        if (audio_frames == 0)
            return;

        output_read_buffer(dec, audio_frame + silence_frames, audio_frames);
        nrsc5_report_audio(st->radio, i, audio_frame, frame_len);

        free(audio_frame);

        // Reset
        elastic->pos = -1;
        dec->leftover = (iq_upper % 135) + (silence_upper % 135);
    }
}

void output_push(output_t *st, uint8_t *pkt, unsigned int len, unsigned int program, unsigned int stream_id, unsigned int seq)
{
    decoder_t *dec = &st->decoder[program];
    elastic_buffer_t *elastic = &dec->elastic_buffer;

    if (stream_id != 0)
        return; // TODO: Process enhanced stream

    if (elastic_writable(elastic) == 0)
    {
        log_error("elastic buffer full. skipped packet. bug?");
        return;
    }

    unsigned int forward = compute_forward_sequence_position(elastic, seq);
    unsigned int pos = (elastic->write + forward) % elastic->size;

    memcpy(elastic->ptr[pos].data, pkt, len);
    elastic->ptr[pos].size = len;
    elastic->ptr[pos].seq = seq;

    elastic->write = pos;
}

static void aas_free_lot(aas_file_t *file)
{
    free(file->name);
    if (file->fragments)
    {
        for (int i = 0; i < MAX_LOT_FRAGMENTS; i++)
            free(file->fragments[i]);
        free(file->fragments);
    }
    memset(file, 0, sizeof(*file));
}

static void aas_reset(output_t *st)
{
    for (int i = 0; i < MAX_PORTS; i++)
    {
        aas_port_t *port = &st->ports[i];
        if (port->port == 0)
            continue;
        if (port->type == AAS_TYPE_LOT)
        {
            for (int j = 0; j < MAX_LOT_FILES; j++)
                aas_free_lot(&port->lot_files[j]);
        }
    }

    for (int i = 0; i < MAX_SIG_SERVICES; i++)
    {
        free(st->services[i].name);
    }

    memset(st->ports, 0, sizeof(st->ports));
    memset(st->services, 0, sizeof(st->services));
}

void output_reset(output_t *st)
{
    aas_reset(st);

#ifdef USE_FAAD2
    for (int i = 0; i < MAX_PROGRAMS; i++)
    {
        decoder_t *dec = &st->decoder[i];
        elastic_buffer_t *buffer = &dec->elastic_buffer;

        if (dec->aacdec)
            NeAACDecClose(dec->aacdec);
        dec->aacdec = NULL;

        buffer->write = 0;
        buffer->read = 0;
        buffer->clock = 0;
        buffer->pos = -1;

        if (buffer->ptr)
            free(buffer->ptr);
        buffer->ptr = NULL;

        dec->leftover = 0;
        dec->write = 0;
        dec->read = 0;
        dec->delay = 0;

        if (dec->output_buffer)
            free(dec->output_buffer);
        dec->output_buffer = NULL;
    }
#endif
}

void output_init(output_t *st, nrsc5_t *radio)
{
    st->radio = radio;

#ifdef USE_FAAD2
    for (int i = 0; i < MAX_PROGRAMS; i++)
        st->decoder[i].aacdec = NULL;

    memset(st->silence, 0, sizeof(st->silence));
#endif

    memset(st->ports, 0, sizeof(st->ports));
    memset(st->services, 0, sizeof(st->services));

    output_reset(st);
}

void output_free(output_t *st)
{
    output_reset(st);
}

static unsigned int id3_length(uint8_t *buf)
{
    return ((buf[0] & 0x7f) << 21) | ((buf[1] & 0x7f) << 14) | ((buf[2] & 0x7f) << 7) | (buf[3] & 0x7f);
}

static char* id3_encode_utf8(uint8_t enc, uint8_t *buf, unsigned int len)
{
    char *text;

    if (enc == 0)
        return iso_8859_1_to_utf_8(buf, len);
    else if (enc == 1)
        return ucs_2_to_utf_8(buf, len);
    else
        log_warn("Invalid encoding: %d", enc);

    text = malloc(1);
    text[0] = 0;
    return text;
}

static char *id3_text(uint8_t *buf, unsigned int frame_len)
{
    if (frame_len > 0)
        return id3_encode_utf8(buf[0], buf + 1, frame_len - 1);
    else
        return id3_encode_utf8(0, NULL, 0);
}

static void output_id3(output_t *st, unsigned int program, uint8_t *buf, unsigned int len)
{
    char *title = NULL, *artist = NULL, *album = NULL, *genre = NULL, *ufid_owner = NULL, *ufid_id = NULL;
    uint32_t xhdr_mime = 0;
    int xhdr_param = -1, xhdr_lot = -1;
    nrsc5_id3_comment_t *comm = NULL;

    unsigned int off = 0, id3_len;
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_ID3;
    evt.id3.comments = NULL;

    if (len < 10 || memcmp(buf, "ID3\x03\x00", 5) || buf[5]) return;
    id3_len = id3_length(buf + 6) + 10;
    if (id3_len > len) return;
    off += 10;

    while (off + 10 <= id3_len)
    {
        uint8_t *tag = buf + off;
        uint8_t *data = tag + 10;
        unsigned int frame_len = ((unsigned int)tag[4] << 24) | (tag[5] << 16) | (tag[6] << 8) | tag[7];
        if (off + 10 + frame_len > id3_len)
            break;

        if (memcmp(tag, "TIT2", 4) == 0)
        {
            free(title);
            title = id3_text(data, frame_len);
        }
        else if (memcmp(tag, "TPE1", 4) == 0)
        {
            free(artist);
            artist = id3_text(data, frame_len);
        }
        else if (memcmp(tag, "TALB", 4) == 0)
        {
            free(album);
            album = id3_text(data, frame_len);
        }
        else if (memcmp(tag, "TCON", 4) == 0)
        {
            free(genre);
            genre = id3_text(data, frame_len);
        }
        else if (memcmp(tag, "UFID", 4) == 0)
        {
            uint8_t *delim = memchr(data, 0, frame_len);
            uint8_t *end = data + frame_len;

            if (delim)
            {
                free(ufid_owner);
                ufid_owner = strdup((char *)data);

                free(ufid_id);
                ufid_id = strndup((char *)delim + 1, end - delim - 1);
            }
        }
        else if (memcmp(tag, "COMR", 4) == 0)
        {
            int i;
            uint8_t *delim[4];
            uint8_t *pos = data + 1;
            uint8_t *end = data + frame_len;

            char *price, until[11], *url, *seller, *desc;
            int received_as;

            for (i = 0; i < 4; i++)
            {
                if (pos >= end)
                    break;
                if ((delim[i] = memchr(pos, 0, end - pos)) == NULL)
                    break;

                pos = delim[i] + 1;
                if (i == 0)
                    pos += 8;
                else if (i == 1)
                    pos += 1;
            }

            if (i == 4)
            {
                price = (char *) data + 1;
                sprintf(until, "%.4s-%.2s-%.2s", delim[0] + 1, delim[0] + 5, delim[0] + 7);
                url = (char *) delim[0] + 9;
                received_as = *(delim[1] + 1);
                seller = (char *) delim[1] + 2;
                desc = (char *) delim[2] + 1;
                log_debug("Commercial: price=%s until=%s url=\"%s\" seller=\"%s\" desc=\"%s\" received_as=%d",
                          price, until, url, seller, desc, received_as);
            }
        }
        else if (memcmp(tag, "COMM", 4) == 0)
        {
            if (frame_len < 5)
            {
                log_warn("bad COMM tag (frame_len %d)", frame_len);
            }
            else
            {
                uint8_t enc = data[0];
                uint8_t *delim = NULL;
                uint8_t *text = NULL;
                uint8_t *end = data + frame_len;

                if (enc == 0)
                {
                    delim = memchr(data + 4, 0, frame_len - 4);
                    if (delim)
                        text = delim + 1;
                }
                else if (enc == 1)
                {
                    unsigned int i;
            
                    for (i = 0; i < len - 1; i += 2)
                    {
                        if (buf[i] == 0 && buf[i + 1] == 0)
                        {
                            delim = buf + i;
                            text = buf + i + 2;
                            break;
                        }
                    }
                }      

                if (delim)
                {
                    nrsc5_id3_comment_t* prev = comm;

                    comm = calloc(1, sizeof(nrsc5_id3_comment_t));
                    comm->lang = strndup((char*) data + 1, 3);
                    comm->short_content_desc = id3_encode_utf8(enc, data + 4, delim - (data + 4));
                    comm->full_text = id3_encode_utf8(enc, text, end - text);

                    if (prev == NULL)
                        evt.id3.comments = comm;
                    else
                        prev->next = comm;
                }
            }
        }
        else if (memcmp(tag, "XHDR", 4) == 0)
        {
            uint8_t extlen;

            if (frame_len < 6)
            {
                log_warn("bad XHDR tag (frame_len %d)", frame_len);
            }
            else
            {
                xhdr_mime = data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t)data[3] << 24);
                xhdr_param = data[4];
                extlen = data[5];
                if (6u + extlen != frame_len)
                    log_warn("bad XHDR tag (frame_len %d, extlen %d)", frame_len, extlen);
                else if (xhdr_param == 0 && extlen == 2)
                    xhdr_lot = data[6] | (data[7] << 8);
                else if (xhdr_param == 1 && extlen == 0)
                    xhdr_lot = -1;
                else
                    log_warn("unhandled XHDR param (frame_len %d, param %d, extlen %d)", frame_len, xhdr_param, extlen);
            }
        }
        else
        {
            unsigned int i;
            char *hex = malloc(3 * frame_len + 1);
            for (i = 0; i < frame_len; i++)
                sprintf(hex + (3 * i), "%02X ", buf[off + 10 + i]);
            hex[3 * i - 1] = 0;
            log_debug("%c%c%c%c tag: %s", buf[off], buf[off+1], buf[off+2], buf[off+3], hex);
            free(hex);
        }

        off += 10 + frame_len;
    }

    evt.id3.program = program;
    evt.id3.title = title;
    evt.id3.artist = artist;
    evt.id3.album = album;
    evt.id3.genre = genre;
    evt.id3.ufid.owner = ufid_owner;
    evt.id3.ufid.id = ufid_id;
    evt.id3.xhdr.mime = xhdr_mime;
    evt.id3.xhdr.param = xhdr_param;
    evt.id3.xhdr.lot = xhdr_lot;

    nrsc5_report(st->radio, &evt);

    free(title);
    free(artist);
    free(album);
    free(genre);
    free(ufid_owner);
    free(ufid_id);

    for (comm = evt.id3.comments; comm != NULL; )
    {
        void *p = comm;

        free(comm->lang);
        free(comm->short_content_desc);
        free(comm->full_text);

        comm = comm->next;
        free(p);
    }
}

static void parse_sig(output_t *st, uint8_t *buf, unsigned int len)
{
    int port_idx = 0, service_idx = 0, component_idx = 0;
    uint8_t *p = buf;
    sig_service_t *service = NULL;

    if (st->services[0].type != SIG_SERVICE_NONE)
    {
        // We assume that the SIG will never change, and only process it once.
        return;
    }

    memset(st->ports, 0, sizeof(st->ports));
    memset(st->services, 0, sizeof(st->services));

    while (p < buf + len)
    {
        uint8_t type = *p++;
        switch (type & 0xF0)
        {
        case 0x40:
        {
            if (service_idx == MAX_SIG_SERVICES)
            {
                log_warn("Too many SIG services");
                goto done;
            }

            service = &st->services[service_idx++];
            service->type = type == 0x40 ? SIG_SERVICE_AUDIO : SIG_SERVICE_DATA;
            service->number = p[0] | (p[1] << 8);
            component_idx = 0;

            p += 3;
            break;
        }
        case 0x60:
        {
            // length (1-byte) value (length - 1)
            uint8_t l = *p++;
            if (service == NULL)
            {
                log_warn("Invalid SIG data (%02X)", type);
                goto done;
            }
            else if (type == 0x69)
            {
                service->name = iso_8859_1_to_utf_8(p + 1, l - 2);
            }
            else if (type == 0x67)
            {
                sig_component_t *comp;

                if (component_idx == MAX_SIG_COMPONENTS)
                {
                    log_warn("Too many SIG components");
                    goto done;
                }

                if (port_idx == MAX_PORTS)
                {
                    log_warn("Too many AAS ports");
                    goto done;
                }

                comp = &service->component[component_idx++];
                comp->type = SIG_COMPONENT_DATA;
                comp->id = p[0];
                comp->data.port = p[1] | (p[2] << 8);
                comp->data.service_data_type = p[3] | (p[4] << 8);
                comp->data.type = p[5];
                comp->data.mime = p[8] | (p[9] << 8) | (p[10] << 16) | ((uint32_t)p[11] << 24);

                aas_port_t *port = &st->ports[port_idx++];
                port->port = comp->data.port;
                port->type = comp->data.type;
                port->mime = comp->data.mime;
                port->service_number = service->number;
            }
            else if (type == 0x66)
            {
                sig_component_t *comp;

                if (component_idx == MAX_SIG_COMPONENTS)
                {
                    log_warn("Too many SIG components");
                    goto done;
                }

                comp = &service->component[component_idx++];
                comp->type = SIG_COMPONENT_AUDIO;
                comp->id = p[0];
                comp->audio.port = p[1];
                comp->audio.type = p[2];
                comp->audio.mime = p[7] | (p[8] << 8) | (p[9] << 16) | ((uint32_t)p[10] << 24);
            }
            p += l - 1;
            break;
        }
        default:
            log_warn("unexpected byte %02X", *p);
            goto done;
        }
    }

done:
    nrsc5_report_sig(st->radio, st->services, service_idx);
}

static aas_port_t *find_port(output_t *st, uint16_t port_id)
{
    unsigned int i;
    for (i = 0; i < MAX_PORTS; i++)
    {
        if (st->ports[i].port == port_id)
            return &st->ports[i];
    }
    return NULL;
}

static aas_file_t *find_lot(aas_port_t *port, unsigned int lot)
{
    for (int i = 0; i < MAX_LOT_FILES; i++)
    {
        if (port->lot_files[i].timestamp == 0)
            continue;
        if (port->lot_files[i].lot == lot)
            return &port->lot_files[i];
    }
    return NULL;
}

static aas_file_t *find_free_lot(aas_port_t *port)
{
    unsigned int min_timestamp = UINT_MAX;
    unsigned int min_idx = 0;
    aas_file_t *file;

    for (int i = 0; i < MAX_LOT_FILES; i++)
    {
        unsigned int timestamp = port->lot_files[i].timestamp;
        if (timestamp == 0)
            return &port->lot_files[i];
        if (timestamp < min_timestamp)
        {
            min_timestamp = timestamp;
            min_idx = i;
        }
    }

    file = &port->lot_files[min_idx];
    aas_free_lot(file);
    return file;
}

static void process_port(output_t *st, uint16_t port_id, uint16_t seq, uint8_t *buf, unsigned int len)
{
    static unsigned int counter = 1;
    aas_port_t *port;

    if (st->services[0].type == SIG_SERVICE_NONE)
    {
        // Wait until we receive SIG data.
        return;
    }

    port = find_port(st, port_id);
    if (port == NULL)
    {
        log_debug("missing port %04X", port_id);
        return;
    }

    switch (port->type)
    {
    case AAS_TYPE_STREAM:
    {
        nrsc5_report_stream(st->radio, port_id, seq, len, port->mime, buf);
        break;
    }
    case AAS_TYPE_PACKET:
    {
        nrsc5_report_packet(st->radio, port_id, seq, len, port->mime, buf);
        break;
    }
    case AAS_TYPE_LOT:
    {
        if (len < 8)
        {
            log_warn("bad fragment (port %04X, len %d)", port_id, len);
            return;
        }
        uint8_t hdrlen = buf[0];
        // uint8_t repeat = buf[1];
        uint16_t lot = buf[2] | (buf[3] << 8);
        uint32_t seq = buf[4] | (buf[5] << 8) | (buf[6] << 16) | ((uint32_t)buf[7] << 24);
        if (hdrlen < 8 || hdrlen > len)
        {
            log_warn("wrong header len (port %04X, len %d, hdrlen %d)", port_id, len, hdrlen);
            return;
        }
        buf += 8;
        len -= 8;
        hdrlen -= 8;

        if (seq >= MAX_LOT_FRAGMENTS)
        {
            log_warn("sequence too large (%d)", seq);
            return;
        }

        aas_file_t *file = find_lot(port, lot);
        if (file == NULL)
        {
            file = find_free_lot(port);
            file->lot = lot;
            file->fragments = calloc(MAX_LOT_FRAGMENTS, sizeof(uint8_t*));
        }
        file->timestamp = counter++;

        if (seq == 0)
        {
            if (hdrlen < 16)
            {
                log_warn("header is too short (port %04X, len %d, hdrlen %d)", port_id, len, hdrlen);
                return;
            }

            uint32_t version = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24);
            if (version != 1)
                log_warn("unknown LOT version: %d", version);

            file->expiry_utc.tm_year = ((buf[7] << 4) | (buf[6] >> 4)) - 1900;
            file->expiry_utc.tm_mon = (buf[6] & 0xf) - 1;
            file->expiry_utc.tm_mday = (buf[5] >> 3);
            file->expiry_utc.tm_hour = ((buf[5] & 0x7) << 2) | (buf[4] >> 6);
            file->expiry_utc.tm_min = (buf[4] & 0x3f);

            file->size = buf[8] | (buf[9] << 8) | (buf[10] << 16) | ((uint32_t)buf[11] << 24);
            file->mime = buf[12] | (buf[13] << 8) | (buf[14] << 16) | ((uint32_t)buf[15] << 24);
            buf += 16;
            len -= 16;
            hdrlen -= 16;

            // Everything after the fixed header is the filename.
            free(file->name);
            file->name = strndup((const char *)buf, hdrlen);
            buf += hdrlen;
            len -= hdrlen;
            hdrlen = 0;

            log_debug("File %s, size %d, lot %d, port %04X, mime %08X", file->name, file->size, file->lot, port->port, file->mime);
        }

        if (hdrlen != 0)
        {
            log_warn("unexpected hdrlen (port %04X, hdrlen %d)", port_id, hdrlen);
            break;
        }

        if (!file->fragments[seq])
        {
            uint8_t *fragment = calloc(LOT_FRAGMENT_SIZE, 1);
            if (len > LOT_FRAGMENT_SIZE)
            {
                log_warn("fragment too large (%d)", len);
                break;
            }
            memcpy(fragment, buf, len);
            file->fragments[seq] = fragment;
        }

        if (file->size)
        {
            int complete = 1;
            int num_fragments = (file->size + LOT_FRAGMENT_SIZE - 1) / LOT_FRAGMENT_SIZE;
            for (int i = 0; i < num_fragments; i++)
            {
                if (file->fragments[i] == NULL)
                {
                    complete = 0;
                    break;
                }
            }
            if (complete)
            {
                uint8_t *data = malloc(num_fragments * LOT_FRAGMENT_SIZE);
                for (int i = 0; i < num_fragments; i++)
                    memcpy(data + i * LOT_FRAGMENT_SIZE, file->fragments[i], LOT_FRAGMENT_SIZE);
                nrsc5_report_lot(st->radio, port->port, file->lot, file->size, file->mime, file->name, data, &file->expiry_utc);
                free(data);
                aas_free_lot(file);
            }
        }
        break;
    }
    default:
        log_info("unknown port type %d", port->type);
        break;
    }
}

void output_aas_push(output_t *st, uint8_t *buf, unsigned int len)
{
    uint16_t port = buf[0] | (buf[1] << 8);
    uint16_t seq = buf[2] | (buf[3] << 8);
    if (port == 0x5100 || (port >= 0x5201 && port <= 0x5207))
    {
        // PSD ports
        output_id3(st, port & 0x7, buf + 4, len - 4);
    }
    else if (port == 0x20)
    {
        // Station Information Guide
        parse_sig(st, buf + 4, len - 4);
    }
    else if (port >= 0x401 && port <= 0x50FF)
    {
        process_port(st, port, seq, buf + 4, len - 4);
    }
    else
    {
        log_warn("unknown AAS port %04X, seq %04X, length %d", port, seq, len);
    }
}
