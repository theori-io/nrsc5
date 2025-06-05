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
#include "here_images.h"

void output_align(output_t *st, unsigned int program, unsigned int stream_id, unsigned int offset)
{
    elastic_buffer_t *elastic = &st->elastic[program][stream_id];
    elastic->audio_offset = offset;
}

void output_push(output_t *st, uint8_t *pkt, unsigned int len, unsigned int program, unsigned int stream_id, unsigned int seq)
{
    elastic_buffer_t *elastic = &st->elastic[program][stream_id];

    if (stream_id != 0)
        return; // TODO: Process enhanced stream

    if (elastic->packets[seq].size != 0)
       log_warn("Packet %d already exists in elastic buffer for program %d, stream %d. Overwriting.", seq, program, stream_id);

    memcpy(elastic->packets[seq].data, pkt, len);
    elastic->packets[seq].size = len;
}

void output_advance(output_t *st)
{
    unsigned int program, frame;
    unsigned int audio_frames = (st->radio->mode == NRSC5_MODE_FM ? 2 : 4);

    for (program = 0; program < MAX_PROGRAMS; program++)
    {
        elastic_buffer_t *elastic = &st->elastic[program][0]; // TODO: Process enhanced stream

        if (elastic->audio_offset == -1)
            continue;

        for (frame = 0; frame < audio_frames; frame++)
        {
            unsigned int len = elastic->packets[elastic->audio_offset].size;
            uint8_t *pkt = elastic->packets[elastic->audio_offset].data;
#ifdef USE_FAAD2
            int produced_audio = 0;
#endif

            if (len > 0)
            {
                nrsc5_report_hdc(st->radio, program, pkt, len);

#ifdef USE_FAAD2
                void *buffer;
                NeAACDecFrameInfo info;

                if (!st->aacdec[program])
                {
                    unsigned long samprate = 22050;
                    NeAACDecInitHDC(&st->aacdec[program], &samprate);
                }

                buffer = NeAACDecDecode(st->aacdec[program], &info, pkt, len);
                if (info.error > 0)
                    log_error("Decode error: %s", NeAACDecGetErrorMessage(info.error));

                if (info.error == 0 && info.samples > 0)
                {
                    nrsc5_report_audio(st->radio, program, buffer, info.samples);
                    produced_audio = 1;
                }
#endif

                elastic->packets[elastic->audio_offset].size = 0;
            }
            else
            {
#ifdef USE_FAAD2
                // Reset decoder. Missing packets.
                if (st->aacdec[program])
                {
                    NeAACDecClose(st->aacdec[program]);
                    st->aacdec[program] = NULL;
                }                
#endif
            }

#ifdef USE_FAAD2
            if (!produced_audio)
                nrsc5_report_audio(st->radio, program, st->silence, NRSC5_AUDIO_FRAME_SAMPLES * 2);
#endif
    
            elastic->audio_offset = (elastic->audio_offset + 1) % ELASTIC_BUFFER_LEN;
        }
    }
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
    for (int i = 0; i < MAX_SIG_SERVICES; i++)
    {
        sig_service_t *service = &st->services[i];
        free(service->name);

        for (int j = 0; j < MAX_SIG_COMPONENTS; j++)
        {
            sig_component_t *component = &service->component[j];

            if (component->type == SIG_COMPONENT_DATA)
                for (int k = 0; k < MAX_LOT_FILES; k++)
                    aas_free_lot(&component->data.lot_files[k]);
        }
    }

    memset(st->services, 0, sizeof(st->services));
    nrsc5_clear_sig(st->radio);
}

void output_reset(output_t *st)
{
    aas_reset(st);

    for (int i = 0; i < MAX_PROGRAMS; i++)
    {
        for (int j = 0; j < MAX_STREAMS; j++)
        {
            for (int k = 0; k < ELASTIC_BUFFER_LEN; k++)
            {
                st->elastic[i][j].packets[k].size = 0;
            }
            st->elastic[i][j].audio_offset = -1;
        }
#ifdef USE_FAAD2
        if (st->aacdec[i])
            NeAACDecClose(st->aacdec[i]);
        st->aacdec[i] = NULL;
#endif
    }

    here_images_reset(&st->here_images);
}

void output_init(output_t *st, nrsc5_t *radio)
{
    st->radio = radio;
#ifdef USE_FAAD2
    for (int i = 0; i < MAX_PROGRAMS; i++)
        st->aacdec[i] = NULL;
    memset(st->silence, 0, sizeof(st->silence));
#endif

    memset(st->services, 0, sizeof(st->services));
    here_images_init(&st->here_images, radio);

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

static int find_component(sig_service_t *service, uint8_t component_id)
{
    int component_idx;

    for (component_idx = 0; component_idx < MAX_SIG_COMPONENTS; component_idx++)
    {
        if (service->component[component_idx].type == SIG_COMPONENT_NONE)
            break; // reached a free slot in the component list

        if (service->component[component_idx].id == component_id)
        {
            log_warn("duplicate SIG component: service %d, component %d", service->number, component_id);
            break;
        }
    }

    return component_idx;
}

static void parse_sig(output_t *st, uint8_t *buf, unsigned int len)
{
    uint8_t *p = buf;
    sig_service_t *service = NULL;

    if (st->services[0].type != SIG_SERVICE_NONE)
    {
        // We assume that the SIG will never change, and only process it once.
        return;
    }

    memset(st->services, 0, sizeof(st->services));

    while (p < buf + len)
    {
        uint8_t type = *p++;
        switch (type & 0xF0)
        {
        case 0x40:
        {
            uint16_t service_number = p[0] | (p[1] << 8);
            int service_idx;

            for (service_idx = 0; service_idx < MAX_SIG_SERVICES; service_idx++)
            {
                if (st->services[service_idx].type == SIG_SERVICE_NONE)
                    break; // reached a free slot in the service list

                if (st->services[service_idx].number == service_number)
                {
                    log_warn("duplicate SIG service: %d", service_number);
                    free(st->services[service_idx].name);
                    memset(&st->services[service_idx], 0, sizeof(st->services[service_idx]));
                    break;
                }
            }

            if (service_idx == MAX_SIG_SERVICES)
            {
                log_warn("Too many SIG services");
                goto done;
            }

            service = &st->services[service_idx];
            service->type = type == 0x40 ? SIG_SERVICE_AUDIO : SIG_SERVICE_DATA;
            service->number = service_number;

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
                uint8_t component_id = p[0];
                int component_idx = find_component(service, component_id);

                if (component_idx == MAX_SIG_COMPONENTS)
                {
                    log_warn("Too many SIG components");
                    goto done;
                }

                comp = &service->component[component_idx];
                comp->type = SIG_COMPONENT_DATA;
                comp->id = component_id;
                comp->data.port = p[1] | (p[2] << 8);
                comp->data.service_data_type = p[3] | (p[4] << 8);
                comp->data.type = p[5];
                comp->data.mime = p[8] | (p[9] << 8) | (p[10] << 16) | ((uint32_t)p[11] << 24);
            }
            else if (type == 0x66)
            {
                sig_component_t *comp;
                uint8_t component_id = p[0];
                int component_idx = find_component(service, component_id);

                if (component_idx == MAX_SIG_COMPONENTS)
                {
                    log_warn("Too many SIG components");
                    goto done;
                }

                comp = &service->component[component_idx];
                comp->type = SIG_COMPONENT_AUDIO;
                comp->id = component_id;
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
    nrsc5_report_sig(st->radio, st->services);
}

static sig_component_t *find_port(output_t *st, uint16_t port_id)
{
    unsigned int i, j;
    for (i = 0; i < MAX_SIG_SERVICES; i++)
    {
        sig_service_t *service = &st->services[i];
        if (service->type == SIG_SERVICE_NONE)
            break;

        for (j = 0; j < MAX_SIG_COMPONENTS; j++)
        {
            sig_component_t *component = &service->component[j];
            if (component->type == SIG_COMPONENT_NONE)
                break;

            if ((component->type == SIG_COMPONENT_DATA) && (component->data.port == port_id))
                return component;
        }
    }
    return NULL;
}

static aas_file_t *find_lot(sig_component_t *component, unsigned int lot)
{
    for (int i = 0; i < MAX_LOT_FILES; i++)
    {
        if (component->data.lot_files[i].timestamp == 0)
            continue;
        if (component->data.lot_files[i].lot == lot)
            return &component->data.lot_files[i];
    }
    return NULL;
}

static aas_file_t *find_free_lot(sig_component_t *component)
{
    unsigned int min_timestamp = UINT_MAX;
    unsigned int min_idx = 0;
    aas_file_t *file;

    for (int i = 0; i < MAX_LOT_FILES; i++)
    {
        unsigned int timestamp = component->data.lot_files[i].timestamp;
        if (timestamp == 0)
            return &component->data.lot_files[i];
        if (timestamp < min_timestamp)
        {
            min_timestamp = timestamp;
            min_idx = i;
        }
    }

    file = &component->data.lot_files[min_idx];
    aas_free_lot(file);
    return file;
}

static void process_port(output_t *st, uint16_t port_id, uint16_t seq, uint8_t *buf, unsigned int len)
{
    static unsigned int counter = 1;
    sig_component_t *component;

    if (st->services[0].type == SIG_SERVICE_NONE)
    {
        // Wait until we receive SIG data.
        return;
    }

    component = find_port(st, port_id);
    if (component == NULL)
    {
        log_debug("port %04X not defined in SIG table", port_id);
        return;
    }

    switch (component->data.type)
    {
    case NRSC5_AAS_TYPE_STREAM:
    {
        nrsc5_report_stream(st->radio, port_id, seq, len, buf, component->service_ext, component->component_ext);
        if (component->data.mime == NRSC5_MIME_HERE_IMAGE)
            here_images_push(&st->here_images, seq, len, buf);
        break;
    }
    case NRSC5_AAS_TYPE_PACKET:
    {
        nrsc5_report_packet(st->radio, port_id, seq, len, buf, component->service_ext, component->component_ext);
        break;
    }
    case NRSC5_AAS_TYPE_LOT:
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

        aas_file_t *file = find_lot(component, lot);
        if (file == NULL)
        {
            file = find_free_lot(component);
            file->lot = lot;
            file->fragments = calloc(MAX_LOT_FRAGMENTS, sizeof(uint8_t*));
        }
        file->timestamp = counter++;

        if (hdrlen > 0)
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

            log_debug("File %s, size %d, lot %d, port %04X, mime %08X", file->name, file->size, file->lot, component->data.port, file->mime);
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
                nrsc5_report_lot(st->radio, component->data.port, file->lot, file->size, file->mime,
                                 file->name, data, &file->expiry_utc,
                                 component->service_ext, component->component_ext);
                free(data);
                aas_free_lot(file);
            }
        }
        break;
    }
    default:
        log_info("unknown port type %d", component->data.type);
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
