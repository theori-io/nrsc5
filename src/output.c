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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <zlib.h>

#include "defines.h"
#include "output.h"
#include "private.h"
#include "unicode.h"
#include "here_images.h"
#include "navteq.h"
#include "ttn_tpeg.h"
#include "here_tpeg.h"

void output_align(output_t *st, unsigned int program, unsigned int stream_id, unsigned int offset)
{
    elastic_buffer_t *elastic = &st->elastic[program][stream_id];
    elastic->audio_offset = offset;
}

static int is_complete_pkt(const packet_t* pkt)
{
    return pkt->shape == PACKET_FULL;
}

static int is_crc_ok(const packet_t* pkt)
{
    return !(pkt->flags & PACKET_FLAG_CRC_ERROR);
}

void output_push(output_t *st, const packet_ref_t* ref)
{
    elastic_buffer_t *elastic = &st->elastic[ref->program][ref->stream_id];
    packet_t* pkt = &elastic->packets[ref->seq];

    if (ref->stream_id != 0)
        return; // TODO: Process enhanced stream

    if (pkt->shape == PACKET_FULL)
        log_warn("Packet %d already exists in elastic buffer for program %d, stream %d. Overwriting.", ref->seq, ref->program, ref->stream_id);

    if (ref->shape == PACKET_HALF_BACK && pkt->shape == PACKET_HALF_FRONT)
    {
        pkt->flags |= ref->flags;
        pkt->shape = PACKET_FULL;

        if (is_crc_ok(pkt))
        {
            memcpy(pkt->data + pkt->size, ref->data, ref->size);
            pkt->size += ref->size;
        }
        else
        {
            pkt->size = 0;
        }
    }
    else
    {
        if (ref->shape == PACKET_HALF_BACK)
            return;

        pkt->flags = ref->flags;
        pkt->shape = ref->shape;

        if (is_crc_ok(pkt))
        {
            memcpy(pkt->data, ref->data, ref->size);
            pkt->size = ref->size;
        }
        else
        {
            pkt->size = 0;
        }
    }
}

static void pkt_reset(packet_t* pkt)
{
    pkt->size = 0;
    pkt->flags = PACKET_FLAG_NONE;
    pkt->shape = PACKET_NONE;
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
            packet_t* pkt = &elastic->packets[elastic->audio_offset];
#ifdef USE_FAAD2
            int produced_audio = 0;
#endif

            if (is_complete_pkt(pkt))
            {
                nrsc5_report_hdc(st->radio, program, pkt);
            }

            if (is_complete_pkt(pkt) && is_crc_ok(pkt))
            {
#ifdef USE_FAAD2
                void *buffer;
                NeAACDecFrameInfo info;

                if (!st->aacdec[program])
                {
                    NeAACDecInitHDC(&st->aacdec[program]);
                }

                buffer = NeAACDecDecode(st->aacdec[program], &info, pkt->data, pkt->size);
                if (info.error > 0)
                    log_error("Decode error: %s", NeAACDecGetErrorMessage(info.error));

                if (info.error == 0 && info.samples > 0)
                {
                    nrsc5_report_audio(st->radio, program, buffer, info.samples);
                    produced_audio = 1;
                }
#endif
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

            pkt_reset(pkt);

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
    for (unsigned int i = 0; i < MAX_SIG_COMPONENTS; i++)
    {
        free(st->ttn_buffers[i]);
        st->ttn_buffers[i] = NULL;
        st->ttn_buffer_sizes[i] = 0;
        st->ttn_buffer_ports[i] = 0;
    }
    free(st->here_buffer);
    st->here_buffer = NULL;
    st->here_buffer_size = 0;
    for (unsigned int i = 0; i < st->ttn_city_count; i++)
        free((void *)st->ttn_cities[i].name);
    free(st->ttn_cities);
    st->ttn_cities = NULL;
    st->ttn_city_count = 0;
    st->ttn_city_database_version = 0;
    st->ttn_city_database_timestamp = 0;
    for (unsigned int i = 0; i < st->ttn_weather_city_count; i++)
        free((void *)st->ttn_weather_cities[i].name);
    free(st->ttn_weather_cities);
    st->ttn_weather_cities = NULL;
    st->ttn_weather_city_count = 0;
    free(st->sig_bytes);
    st->sig_bytes = NULL;
    st->sig_len = 0;

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
    st->lot_lru_counter = 1;

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
                pkt_reset(&st->elastic[i][j].packets[k]);
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

static uint8_t* memchr_enc(const int enc, uint8_t *buf, const unsigned int len)
{
    if (enc == 0)
    {
        return memchr(buf, 0, len);
    }
    else if (enc == 1)
    {
        for (unsigned int i = 0; i < len - 1; i += 2)
        {
            if (buf[i] == 0 && buf[i + 1] == 0)
            {
                return buf + i;
            }
        }
        return NULL;
    }
    else
        log_warn("Invalid encoding: %d", enc);
    return NULL;
}

static int parse_digits(const char *p, size_t n)
{
    int value = 0;

    for (size_t i = 0; i < n; i++)
    {
        if (p[i] < '0' || p[i] > '9')
            return -1;

        value = value * 10 + (p[i] - '0');
    }

    return value;
}

static void output_id3(output_t *st, unsigned int program, uint8_t *buf, unsigned int len)
{
    char *title = NULL, *artist = NULL, *album = NULL, *genre = NULL, *ufid_owner = NULL, *ufid_id = NULL;
    uint32_t xhdr_mime = 0;
    int xhdr_param = -1, xhdr_lot = -1;
    nrsc5_id3_comment_t *comm = NULL;
    char *price = NULL, *url = NULL, *seller = NULL, *desc = NULL;
    struct tm until = { 0 };
    uint8_t received_as = 0;

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
            uint8_t *pos = data;
            uint8_t *end = data + frame_len;
            if (pos + 1 > end)
            {
                log_warn("bad COMR tag (frame_len %d)", frame_len);
                break;
            }
            uint8_t enc = data[0];
            pos += 1;

            uint8_t *delim[4];
            uint8_t *delim_end[4];
            int year, mon, mday;
            int i;

            for (i = 0; i < 4; i++)
            {
                int delim_enc = (i >= 2) ? enc : 0;
                int delim_len = (delim_enc == 1) ? 2 : 1;
                delim[i] = memchr_enc(delim_enc, pos, end - pos);
                if (delim[i] == NULL)
                {
                    log_warn("bad COMR tag (frame_len %d)", frame_len);
                    break;
                }
                delim_end[i] = delim[i] + delim_len;
                pos = delim_end[i];

                if (i == 0)
                {
                    if (pos + 8 > end)
                    {
                        log_warn("bad COMR tag (frame_len %d)", frame_len);
                        break;
                    }

                    year = parse_digits((char *) delim[0] + 1, 4) - 1900;
                    mon = parse_digits((char *) delim[0] + 5, 2) - 1;
                    mday = parse_digits((char *) delim[0] + 7, 2);

                    if (year < 0 || mon < 0 || mday < 0)
                    {
                        log_warn("Failed to parse valid_until on COMR tag.");
                        break;
                    }

                    pos += 8;
                }
                else if (i == 1)
                {
                    if (pos + 1 > end)
                    {
                        log_warn("bad COMR tag (frame_len %d)", frame_len);
                        break;
                    }

                    pos += 1;
                }
            }

            if (i == 4)
            {
                price = (char *) data + 1;
                until.tm_year = year;
                until.tm_mon = mon;
                until.tm_mday = mday;
                url = (char *) delim_end[0] + 8;
                received_as = *(delim_end[1]);
                seller = id3_encode_utf8(enc, delim_end[1] + 1, delim[2] - (delim_end[1] + 1));
                desc = id3_encode_utf8(enc, delim_end[2], delim[3] - delim_end[2]);
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
                uint8_t *delim = memchr_enc(enc, data + 4, frame_len - 4);
                uint8_t *end = data + frame_len;

                if (delim)
                {
                    nrsc5_id3_comment_t* prev = comm;
                    uint8_t* end_text = delim + (enc == 1 ? 2 : 1);

                    comm = calloc(1, sizeof(nrsc5_id3_comment_t));
                    comm->lang = strndup((char*) data + 1, 3);
                    comm->short_content_desc = id3_encode_utf8(enc, data + 4, delim - (data + 4));
                    comm->full_text = id3_encode_utf8(enc, end_text, end - end_text);

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
    evt.id3.commercial.price = price;
    evt.id3.commercial.contact_url = url;
    evt.id3.commercial.seller = seller;
    evt.id3.commercial.description = desc;
    evt.id3.commercial.received_as = received_as;
    evt.id3.commercial.valid_until = &until;

    nrsc5_report(st->radio, &evt);

    free(title);
    free(artist);
    free(album);
    free(genre);
    free(ufid_owner);
    free(ufid_id);
    free(seller);
    free(desc);

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

    if (st->sig_bytes)
    {
        if ((len == st->sig_len) && (memcmp(buf, st->sig_bytes, len) == 0))
        {
            // previously parsed SIG table has not changed
            return;
        }
        else
        {
            aas_reset(st);
        }
    }

    st->sig_bytes = (uint8_t *) malloc(len);
    memcpy(st->sig_bytes, buf, len);
    st->sig_len = len;

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

static void process_navteq(output_t *st, sig_component_t *component, uint16_t seq,
                           const uint8_t *buf, unsigned int len)
{
    unsigned int count = 0;
    uint8_t generation;
    int is_terminal;

    if (navteq_decode_digital_traffic(buf, len, &generation, &is_terminal, NULL, &count))
    {
        nrsc5_navteq_digital_traffic_entry_t *entries;

        entries = malloc(count * sizeof(*entries));
        if (entries == NULL)
        {
            log_warn("unable to allocate NAVTEQ Digital Traffic entries");
            return;
        }
        if (navteq_decode_digital_traffic(buf, len, &generation, &is_terminal, entries, &count))
            nrsc5_report_navteq_digital_traffic(st->radio, seq, generation, is_terminal,
                                                count, entries, component->service_ext,
                                                component->component_ext);
        free(entries);
    }
    else if (navteq_decode_alternate_frequencies(buf, len, NULL, &count))
    {
        nrsc5_navteq_alternate_frequency_entry_t *entries;

        entries = malloc(count * sizeof(*entries));
        if (entries == NULL)
        {
            log_warn("unable to allocate NAVTEQ alternate-frequency entries");
            return;
        }
        if (navteq_decode_alternate_frequencies(buf, len, entries, &count))
            nrsc5_report_navteq_alternate_frequencies(st->radio, seq, count, entries,
                                                      component->service_ext,
                                                      component->component_ext);
        free(entries);
    }
}

static void ttn_city_database_cb(const nrsc5_ttn_city_database_t *database, void *opaque)
{
    output_t *st = opaque;
    nrsc5_ttn_city_t *cities;
    int unchanged = database->count == st->ttn_city_count
                 && database->database_version == st->ttn_city_database_version
                 && database->timestamp == st->ttn_city_database_timestamp;

    if (unchanged)
        for (unsigned int i = 0; i < database->count; i++)
        {
            const nrsc5_ttn_city_t *old = &st->ttn_cities[i];
            const nrsc5_ttn_city_t *new = &database->cities[i];
            if (old->city_id != new->city_id
                || old->provider_city_id != new->provider_city_id
                || old->latitude != new->latitude
                || old->longitude != new->longitude
                || strcmp(old->name ? old->name : "", new->name ? new->name : "") != 0)
            {
                unchanged = 0;
                break;
            }
        }
    if (unchanged)
        return;

    cities = calloc(database->count, sizeof(*cities));
    if (database->count && !cities)
        return;
    for (unsigned int i = 0; i < database->count; i++)
    {
        cities[i] = database->cities[i];
        cities[i].name = strdup(database->cities[i].name);
        if (!cities[i].name)
        {
            for (unsigned int j = 0; j < i; j++) free((void *)cities[j].name);
            free(cities);
            return;
        }
    }
    for (unsigned int i = 0; i < st->ttn_city_count; i++)
        free((void *)st->ttn_cities[i].name);
    free(st->ttn_cities);
    st->ttn_cities = cities;
    st->ttn_city_count = database->count;
    st->ttn_city_database_version = database->database_version;
    st->ttn_city_database_timestamp = database->timestamp;
    nrsc5_report_ttn_city_database(st->radio, database);
}

static void ttn_weather_cb(unsigned int timestamp, unsigned int count,
                            const nrsc5_ttn_weather_city_t *cities, void *opaque)
{
    output_t *st = opaque;
    nrsc5_ttn_weather_city_t *copy = calloc(count, sizeof(*copy));
    if (count && !copy)
        return;
    for (unsigned int i = 0; i < count; i++)
    {
        if (cities[i].city.name)
        {
            char *name = strdup(cities[i].city.name);
            unsigned int j;
            if (!name)
                continue;
            for (j = 0; j < st->ttn_weather_city_count; j++)
                if (st->ttn_weather_cities[j].city_id == cities[i].city.city_id)
                    break;
            if (j == st->ttn_weather_city_count)
            {
                nrsc5_ttn_city_t *grown = realloc(st->ttn_weather_cities,
                                                  (j + 1) * sizeof(*grown));
                if (grown)
                {
                    st->ttn_weather_cities = grown;
                    memset(&st->ttn_weather_cities[j], 0,
                           sizeof(st->ttn_weather_cities[j]));
                    st->ttn_weather_city_count++;
                }
            }
            if (j < st->ttn_weather_city_count)
            {
                free((void *)st->ttn_weather_cities[j].name);
                st->ttn_weather_cities[j] = cities[i].city;
                st->ttn_weather_cities[j].name = name;
                name = NULL;
            }
            free(name);
        }
    }
    for (unsigned int i = 0; i < count; i++)
    {
        copy[i] = cities[i];
        if (!copy[i].city.name)
        {
            for (unsigned int j = 0; j < st->ttn_weather_city_count; j++)
                if (st->ttn_weather_cities[j].city_id == copy[i].city.city_id)
                {
                    copy[i].city = st->ttn_weather_cities[j];
                    break;
                }
            if (!copy[i].city.name)
            for (unsigned int j = 0; j < st->ttn_city_count; j++)
                if (st->ttn_cities[j].city_id == copy[i].city.city_id)
                {
                    copy[i].city = st->ttn_cities[j];
                    break;
                }
        }
    }
    nrsc5_report_ttn_weather(st->radio, timestamp, count, copy);
    free(copy);
}

static void ttn_service_network_cb(const nrsc5_ttn_service_network_t *info, void *opaque)
{
    output_t *st = opaque;
    nrsc5_report_ttn_service_network(st->radio, info);
}

static void ttn_tec_cb(const nrsc5_ttn_tec_t *event, void *opaque)
{
    nrsc5_report_ttn_tec(((output_t *)opaque)->radio, event);
}

static void process_ttn_tec(output_t *st, const uint8_t *payload, size_t size);

static void here_tfp_sink(const nrsc5_here_tfp_t *flow, void *opaque)
{
    output_t *st = opaque;
    nrsc5_here_tfp_t resolved = *flow;

    if (st->radio->location_table)
    {
        int found = nrsc5_location_table_lookup(st->radio->location_table,
                                                flow->country_code,
                                                flow->location_table_number,
                                                flow->location,
                                                &resolved.resolved_location);
        resolved.location_resolved = found == 1;
    }
    nrsc5_report_here_tfp(st->radio, &resolved);
}

/* HERE TPEG arrives as SFW transport frames ('FF 0F', length-prefixed) inside
 * the stream. Each frame is SID(3) + EncID(1) + zlib stream; inflated bodies
 * contain the component frames decoded by here_decode_frame(). */
static uint16_t tpeg_crc(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFF;
    for (size_t i = 0; i < size; i++)
    {
        uint32_t tmp = ((crc << 8) & 0xFFFF) | (crc >> 8);
        crc = (tmp ^ data[i]) & 0xFFFF;
        crc ^= ((crc & 0x00FF) >> 4);
        tmp = ((crc & 0x00FF) << 8) | ((crc & 0x00FF) >> 8);
        crc = ((crc ^ (tmp << 4)) ^ ((crc & 0x00FF) << 5)) & 0xFFFF;
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

static int process_here_frame(output_t *st, const uint8_t *frame)
{
    uint16_t frame_len = ((uint16_t) frame[2] << 8) | frame[3];
    uint16_t stored_crc = ((uint16_t) frame[4] << 8) | frame[5];
    uint8_t head[19];
    size_t verify = frame_len < 11 ? frame_len : 11;
    uint8_t type = frame[6];
    head[0] = 0xff;
    head[1] = 0x0f;
    head[2] = frame[2];
    head[3] = frame[3];
    head[4] = type;
    memcpy(head + 5, frame + 7, verify);
    if (tpeg_crc(head, 5 + verify) != stored_crc)
        return 0;
    if (type != 1 || frame_len < 5)
        return 1;

    // SID(3) + EncID(1): skip both, inflate remainder
    {
        z_stream zs = {0};
        uint8_t out[70000];
        if (inflateInit(&zs) != Z_OK)
            return 1;
        zs.next_in = (uint8_t *) frame + 11;
        zs.avail_in = frame_len - 4;
        zs.next_out = out;
        zs.avail_out = sizeof(out);
        if (inflate(&zs, Z_FINISH) == Z_STREAM_END)
            here_decode_frame(out, sizeof(out) - zs.avail_out, here_tfp_sink, st);
        inflateEnd(&zs);
    }
    return 1;
}

static void process_here_stream(output_t *st, const uint8_t *data, unsigned int size)
{
    uint8_t *grown = realloc(st->here_buffer, st->here_buffer_size + size);
    if (!grown)
        return;
    st->here_buffer = grown;
    memcpy(st->here_buffer + st->here_buffer_size, data, size);
    st->here_buffer_size += size;

    while (st->here_buffer_size >= 7)
    {
        size_t incomplete = SIZE_MAX;
        size_t i;
        int processed = 0;

        for (i = 0; i + 7 <= st->here_buffer_size; i++)
        {
            size_t frame_size;

            if (st->here_buffer[i] != 0xff || st->here_buffer[i + 1] != 0x0f)
                continue;
            frame_size = 7 + ((size_t) st->here_buffer[i + 2] << 8)
                         + st->here_buffer[i + 3];
            if (i + frame_size > st->here_buffer_size)
            {
                if (incomplete == SIZE_MAX)
                    incomplete = i;
                continue;
            }
            if (!process_here_frame(st, st->here_buffer + i))
                continue;

            st->here_buffer_size -= i + frame_size;
            memmove(st->here_buffer, st->here_buffer + i + frame_size,
                    st->here_buffer_size);
            processed = 1;
            break;
        }
        if (processed)
            continue;
        if (incomplete != SIZE_MAX)
        {
            st->here_buffer_size -= incomplete;
            memmove(st->here_buffer, st->here_buffer + incomplete,
                    st->here_buffer_size);
        }
        else
        {
            st->here_buffer[0] = st->here_buffer[st->here_buffer_size - 1];
            st->here_buffer_size = st->here_buffer[0] == 0xff;
        }
        return;
    }
}


static void process_ttn_envelope(output_t *st, const uint8_t *payload, size_t size)
{
    if (ttn_tpeg2_decode(payload, size, ttn_city_database_cb, ttn_weather_cb,
                         ttn_service_network_cb, st) != 0)
        process_ttn_tec(st, payload, size);
}

static void process_ttn_tec(output_t *st, const uint8_t *payload, size_t size)
{
    if (ttn_tpeg1_decode(payload, size, st->radio->location_table, ttn_tec_cb, st) != 0)
        log_debug("unable to decode TTN TPEG-1 TEC envelope");
}

static void process_ttn_stream(output_t *st, uint16_t port, const uint8_t *data, unsigned int size)
{
    unsigned int slot;
    for (slot = 0; slot < MAX_SIG_COMPONENTS; slot++)
        if (st->ttn_buffer_ports[slot] == port || st->ttn_buffer_ports[slot] == 0)
            break;
    if (slot == MAX_SIG_COMPONENTS)
        return;
    if (st->ttn_buffer_ports[slot] == 0)
        st->ttn_buffer_ports[slot] = port;
    uint8_t *grown = realloc(st->ttn_buffers[slot], st->ttn_buffer_sizes[slot] + size);
    if (!grown)
        return;
    st->ttn_buffers[slot] = grown;
    memcpy(st->ttn_buffers[slot] + st->ttn_buffer_sizes[slot], data, size);
    st->ttn_buffer_sizes[slot] += size;
    while (st->ttn_buffer_sizes[slot] >= 7)
    {
        uint16_t frame_size;
        if (st->ttn_buffers[slot][0] != 0xff || st->ttn_buffers[slot][1] != 0x0f)
        {
            memmove(st->ttn_buffers[slot], st->ttn_buffers[slot] + 1,
                    --st->ttn_buffer_sizes[slot]);
            continue;
        }
        frame_size = ((uint16_t)st->ttn_buffers[slot][2] << 8) | st->ttn_buffers[slot][3];
        if (st->ttn_buffer_sizes[slot] < (size_t)frame_size + 7)
            break;
        process_ttn_envelope(st, st->ttn_buffers[slot] + 7, frame_size);
        st->ttn_buffer_sizes[slot] -= frame_size + 7;
        memmove(st->ttn_buffers[slot], st->ttn_buffers[slot] + frame_size + 7,
                st->ttn_buffer_sizes[slot]);
    }
}

static void process_port(output_t *st, uint16_t port_id, uint16_t seq, uint8_t *buf, unsigned int len)
{
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
        nrsc5_report_stream(st->radio, seq, len, buf, component->service_ext, component->component_ext);
        if (component->data.mime == NRSC5_MIME_HERE_IMAGE)
            here_images_push(&st->here_images, seq, len, buf);
        if (component->data.mime == NRSC5_MIME_TTN_TPEG_1
            || component->data.mime == NRSC5_MIME_TTN_TPEG_2
            || component->data.mime == NRSC5_MIME_TTN_TPEG_3)
            process_ttn_stream(st, port_id, buf, len);
        if (component->data.mime == NRSC5_MIME_HERE_TPEG)
            process_here_stream(st, buf, len);
        break;
    }
    case NRSC5_AAS_TYPE_PACKET:
    {
        nrsc5_report_packet(st->radio, seq, len, buf, component->service_ext, component->component_ext);
        if (component->data.mime == NRSC5_MIME_NAVTEQ)
            process_navteq(st, component, seq, buf, len);
        if (component->data.mime == NRSC5_MIME_TTN_TPEG_1)
            process_ttn_tec(st, buf, len);
        else if (component->data.mime == NRSC5_MIME_TTN_TPEG_2
                 || component->data.mime == NRSC5_MIME_TTN_TPEG_3)
            process_ttn_envelope(st, buf, len);
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
        uint8_t repeat = buf[1];
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
        file->timestamp = st->lot_lru_counter++;

        int new_data = 0;

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

            int year = ((buf[7] << 4) | (buf[6] >> 4)) - 1900;
            int mon = (buf[6] & 0xf) - 1;
            int mday = (buf[5] >> 3);
            int hour = ((buf[5] & 0x7) << 2) | (buf[4] >> 6);
            int min = (buf[4] & 0x3f);

            uint32_t size = buf[8] | (buf[9] << 8) | (buf[10] << 16) | ((uint32_t)buf[11] << 24);
            uint32_t mime = buf[12] | (buf[13] << 8) | (buf[14] << 16) | ((uint32_t)buf[15] << 24);
            buf += 16;
            len -= 16;
            hdrlen -= 16;
            // Everything after the fixed header is the filename.

            if (file->name)
            {
                if ((hdrlen != strlen(file->name))
                    || (memcmp(buf, file->name, hdrlen) != 0)
                    || (size != file->size)
                    || (mime != file->mime)
                    || (year != file->expiry_utc.tm_year)
                    || (mon != file->expiry_utc.tm_mon)
                    || (mday != file->expiry_utc.tm_mday)
                    || (hour != file->expiry_utc.tm_hour)
                    || (min != file->expiry_utc.tm_min))
                {
                    // Reset, since metadata has changed
                    aas_free_lot(file);
                    file->lot = lot;
                    file->fragments = calloc(MAX_LOT_FRAGMENTS, sizeof(uint8_t*));
                    file->timestamp = st->lot_lru_counter;
                    new_data = 1;
                }
            }
            else
            {
                // Metadata received for the first time
                new_data = 1;
            }

            free(file->name);
            file->name = strndup((const char *)buf, hdrlen);
            file->size = size;
            file->mime = mime;
            file->expiry_utc.tm_year = year;
            file->expiry_utc.tm_mon = mon;
            file->expiry_utc.tm_mday = mday;
            file->expiry_utc.tm_hour = hour;
            file->expiry_utc.tm_min = min;

            buf += hdrlen;
            len -= hdrlen;
            hdrlen = 0;

            if (new_data)
            {
                nrsc5_report_lot(st->radio, NRSC5_EVENT_LOT_HEADER, file->lot,
                                 file->size, file->mime, file->name, NULL, &file->expiry_utc,
                                 component->service_ext, component->component_ext);
            }
        }

        int is_duplicate = 1;
        if (!file->fragments[seq])
        {
            new_data = 1;
            is_duplicate = 0;
            uint8_t *fragment = calloc(LOT_FRAGMENT_SIZE, 1);
            if (len > LOT_FRAGMENT_SIZE)
            {
                log_warn("fragment too large (%d)", len);
                break;
            }
            memcpy(fragment, buf, len);
            file->fragments[seq] = fragment;
            file->bytes_so_far += len;
        }
        nrsc5_report_lot_fragment(st->radio, file->lot, seq, repeat, is_duplicate, len, file->bytes_so_far, buf,
                                  component->service_ext, component->component_ext);

        if (new_data && file->size)
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
                nrsc5_report_lot(st->radio, NRSC5_EVENT_LOT, file->lot, file->size, file->mime,
                                 file->name, data, &file->expiry_utc,
                                 component->service_ext, component->component_ext);
                free(data);
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
    if (len < 4)
    {
        log_warn("AAS packet too short (length %d)", len);
        return;
    }
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
