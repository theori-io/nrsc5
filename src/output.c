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
#include <sys/time.h>

#include "defines.h"
#include "output.h"
#include "private.h"
#include "unicode.h"

void output_push(output_t *st, uint8_t *pkt, unsigned int len, unsigned int program, unsigned int stream_id)
{
    nrsc5_report_hdc(st->radio, program, pkt, len);

    if (stream_id != 0)
        return; // TODO: Process enhanced stream

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
        nrsc5_report_audio(st->radio, program, buffer, info.samples);
#endif
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
        switch (port->type)
        {
        case AAS_TYPE_STREAM:
            free(port->stream.data);
            break;
        case AAS_TYPE_LOT:
            for (int j = 0; j < MAX_LOT_FILES; j++)
                aas_free_lot(&port->lot.files[j]);
            break;
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
        if (st->aacdec[i])
            NeAACDecClose(st->aacdec[i]);
        st->aacdec[i] = NULL;
    }
#endif
}

void output_init(output_t *st, nrsc5_t *radio)
{
    st->radio = radio;
#ifdef USE_FAAD2
    for (int i = 0; i < MAX_PROGRAMS; i++)
        st->aacdec[i] = NULL;
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

static char *id3_text(uint8_t *buf, unsigned int frame_len)
{
    char *text;

    if (frame_len > 0)
    {
        if (buf[0] == 0)
            return iso_8859_1_to_utf_8(buf + 1, frame_len - 1);
        else if (buf[0] == 1)
            return ucs_2_to_utf_8(buf + 1, frame_len - 1);
        else
            log_warn("Invalid encoding: %d", buf[0]);
    }

    text = malloc(1);
    text[0] = 0;
    return text;
}

static void output_id3(output_t *st, unsigned int program, uint8_t *buf, unsigned int len)
{
    char *title = NULL, *artist = NULL, *album = NULL, *genre = NULL, *ufid_owner = NULL, *ufid_id = NULL;
    uint32_t xhdr_mime = 0;
    int xhdr_param = -1, xhdr_lot = -1;

    unsigned int off = 0, id3_len;
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_ID3;

    if (len < 10 || memcmp(buf, "ID3\x03\x00", 5) || buf[5]) return;
    id3_len = id3_length(buf + 6) + 10;
    if (id3_len > len) return;
    off += 10;

    while (off + 10 <= id3_len)
    {
        uint8_t *tag = buf + off;
        uint8_t *data = tag + 10;
        unsigned int frame_len = id3_length(tag + 4);
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
                char *name = malloc(l - 1);
                memcpy(name, p + 1, l - 2);
                name[l - 2] = 0;
                service->name = name;
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
        if (port->lot.files[i].timestamp == 0)
            continue;
        if (port->lot.files[i].lot == lot)
            return &port->lot.files[i];
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
        unsigned int timestamp = port->lot.files[i].timestamp;
        if (timestamp == 0)
            return &port->lot.files[i];
        if (timestamp < min_timestamp)
        {
            min_timestamp = timestamp;
            min_idx = i;
        }
    }

    file = &port->lot.files[min_idx];
    aas_free_lot(file);
    return file;
}

static void process_port(output_t *st, uint16_t port_id, uint8_t *buf, unsigned int len)
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
        uint8_t frame_type;

        if (port->stream.data == NULL)
            port->stream.data = malloc(MAX_STREAM_BYTES);

        if (port->mime == NRSC5_MIME_HERE_IMAGE)
            frame_type = 0xF7;
        else
            frame_type = 0x0F;

        while (len)
        {
            uint8_t x = *buf++;
            len--;

            // Wait until we find start of a packet. This is either:
            //   - FF 0F
            //   - FF F7 FF F7
            if (port->stream.prev[0] == 0xFF && x == frame_type &&
                    (frame_type != 0xF7 || (port->stream.prev[1] == frame_type && port->stream.prev[2] == 0xFF)))
            {
                if (port->stream.type != 0 && port->stream.idx > 0)
                {
                    port->stream.idx--;
                    log_debug("Stream data: port=%04X type=%d size=%d size2=%d", port_id, port->stream.type, port->stream.idx, (port->stream.data[0] << 8) | port->stream.data[1]);
                }
                port->stream.idx = 0;
                port->stream.prev[0] = 0;
                port->stream.prev[1] = 0;
                port->stream.prev[2] = 0;
                port->stream.type = x;
            }
            else
            {
                if (port->stream.type != 0)
                    port->stream.data[port->stream.idx++] = x;
                port->stream.prev[2] = port->stream.prev[1];
                port->stream.prev[1] = port->stream.prev[0];
                port->stream.prev[0] = x;
            }

            if (port->stream.idx == MAX_STREAM_BYTES)
            {
                log_info("stream packet overflow (%04X)", port_id);
                port->stream.type = 0;
            }
        }
        break;
    }
    case AAS_TYPE_PACKET:
    {
        if (len < 4)
        {
            log_warn("bad packet (port %04X, len %d)", port_id, len);
            break;
        }
        log_debug("Packet data: port=%04X size=%d", port_id, len);
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

            // uint32_t == 1
            // uint32_t xxx
            // uint32_t size
            // uint32_t mimeHash
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
                nrsc5_report_lot(st->radio, port->port, file->lot, file->size, file->mime, file->name, data);
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
        process_port(st, port, buf + 4, len - 4);
    }
    else
    {
        log_warn("unknown AAS port %04X, seq %04X, length %d", port, seq, len);
    }
}
