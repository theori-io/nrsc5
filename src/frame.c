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

#include <string.h>

#include "defines.h"
#include "frame.h"
#include "input.h"
#include "reed-solomon.h"

typedef struct
{
    unsigned int codec;
    unsigned int pdu_seq;
    unsigned int pfirst;
    unsigned int plast;
    unsigned int seq;
    unsigned int nop;
    unsigned int la_location;
    unsigned int hef;
    uint16_t *locations;
} frame_header_t;

static const uint8_t crc8_tab[] = {
    0, 0x31, 0x62, 0x53, 0xC4, 0xF5, 0xA6, 0x97, 0xB9,
    0x88, 0xDB, 0xEA, 0x7D, 0x4C, 0x1F, 0x2E, 0x43, 0x72,
    0x21, 0x10, 0x87, 0xB6, 0xE5, 0xD4, 0xFA, 0xCB, 0x98,
    0xA9, 0x3E, 0xF, 0x5C, 0x6D, 0x86, 0xB7, 0xE4, 0xD5,
    0x42, 0x73, 0x20, 0x11, 0x3F, 0xE, 0x5D, 0x6C, 0xFB,
    0xCA, 0x99, 0xA8, 0xC5, 0xF4, 0xA7, 0x96, 1, 0x30,
    0x63, 0x52, 0x7C, 0x4D, 0x1E, 0x2F, 0xB8, 0x89, 0xDA,
    0xEB, 0x3D, 0xC, 0x5F, 0x6E, 0xF9, 0xC8, 0x9B, 0xAA,
    0x84, 0xB5, 0xE6, 0xD7, 0x40, 0x71, 0x22, 0x13, 0x7E,
    0x4F, 0x1C, 0x2D, 0xBA, 0x8B, 0xD8, 0xE9, 0xC7, 0xF6,
    0xA5, 0x94, 3, 0x32, 0x61, 0x50, 0xBB, 0x8A, 0xD9,
    0xE8, 0x7F, 0x4E, 0x1D, 0x2C, 2, 0x33, 0x60, 0x51,
    0xC6, 0xF7, 0xA4, 0x95, 0xF8, 0xC9, 0x9A, 0xAB, 0x3C,
    0xD, 0x5E, 0x6F, 0x41, 0x70, 0x23, 0x12, 0x85, 0xB4,
    0xE7, 0xD6, 0x7A, 0x4B, 0x18, 0x29, 0xBE, 0x8F, 0xDC,
    0xED, 0xC3, 0xF2, 0xA1, 0x90, 7, 0x36, 0x65, 0x54,
    0x39, 8, 0x5B, 0x6A, 0xFD, 0xCC, 0x9F, 0xAE, 0x80,
    0xB1, 0xE2, 0xD3, 0x44, 0x75, 0x26, 0x17, 0xFC, 0xCD,
    0x9E, 0xAF, 0x38, 9, 0x5A, 0x6B, 0x45, 0x74, 0x27,
    0x16, 0x81, 0xB0, 0xE3, 0xD2, 0xBF, 0x8E, 0xDD, 0xEC,
    0x7B, 0x4A, 0x19, 0x28, 6, 0x37, 0x64, 0x55, 0xC2,
    0xF3, 0xA0, 0x91, 0x47, 0x76, 0x25, 0x14, 0x83, 0xB2,
    0xE1, 0xD0, 0xFE, 0xCF, 0x9C, 0xAD, 0x3A, 0xB, 0x58,
    0x69, 4, 0x35, 0x66, 0x57, 0xC0, 0xF1, 0xA2, 0x93,
    0xBD, 0x8C, 0xDF, 0xEE, 0x79, 0x48, 0x1B, 0x2A, 0xC1,
    0xF0, 0xA3, 0x92, 5, 0x34, 0x67, 0x56, 0x78, 0x49,
    0x1A, 0x2B, 0xBC, 0x8D, 0xDE, 0xEF, 0x82, 0xB3, 0xE0,
    0xD1, 0x46, 0x77, 0x24, 0x15, 0x3B, 0xA, 0x59, 0x68,
    0xFF, 0xCE, 0x9D, 0xAC
};

static uint8_t crc8(uint8_t *pkt, unsigned int cnt)
{
    unsigned int i, crc = 0xFF;
    for (i = 0; i < cnt; ++i)
        crc = crc8_tab[crc ^ pkt[i]];
    return crc;
}

static int fix_header(uint8_t *buf)
{
    uint8_t hdr[255];
    int corrections;
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, buf, 96);
    corrections = (int)rs_decode(hdr);
    if (corrections >= 0)
    {
        if (corrections)
            log_debug("RS corrected %d symbols", corrections);
        memcpy(buf, hdr, 96);
        return 1;
    }
    else
    {
        return 0;
    }
}

static void parse_header(uint8_t *buf, frame_header_t *hdr)
{
    hdr->codec = buf[8] & 0xf;
    hdr->pdu_seq = (buf[8] >> 6) | ((buf[9] & 1) << 2);
    hdr->pfirst = (buf[11] >> 1) & 1;
    hdr->plast = (buf[11] >> 2) & 1;
    hdr->seq = (buf[11] >> 3) | ((buf[12] & 1) << 5);
    hdr->nop = (buf[12] >> 1) & 0x3f;
    hdr->la_location = buf[13];
    hdr->hef = buf[12] & 0x80;
    hdr->locations = (uint16_t *)&buf[14];
}

static int find_program(uint8_t *buf, int program)
{
    unsigned int i;

    for (i = 0; i < 18269 - 96; )
    {
        unsigned int start = i;
        frame_header_t hdr;

        if (!fix_header(&buf[i]))
        {
            log_debug("failed to fix header");
            return -1;
        }

        if (start == 0 && program == 0)
            return start;

        parse_header(&buf[i], &hdr);
        i += 14 + sizeof(uint16_t) * hdr.nop;

        // inspect first extended header
        if (hdr.hef)
        {
            // skip class indication flag
            if (buf[i] >> 4 == 8)
                ++i;
            // compare program id flag
            if (((buf[i] >> 4) & 7) == 1 && program == ((buf[i] >> 1) & 7))
                return start;
        }

        // skip remaining bytes using last location
        i = start + hdr.locations[hdr.nop - 1] + 1;
    }

    return -1;
}

void frame_process(frame_t *st)
{
    int offset;
    unsigned int i, j, hef, seq;
    uint8_t *buf;
    frame_header_t hdr;

    offset = find_program(st->buffer, st->program);
    if (offset == -1)
    {
        log_error("unable to find program, or corrupted.");
        return;
    }

    buf = &st->buffer[offset];
    parse_header(buf, &hdr);

    if (hdr.codec != 0)
        log_warn("unknown codec field (%d)", hdr.codec);

    log_debug("pdu_seq: %d, seq: %d, nop: %d", hdr.pdu_seq, hdr.seq, hdr.nop);

    i = 14 + sizeof(uint16_t) * hdr.nop;
    // skip extended headers
    for (hef = hdr.hef; hef; ++i)
        hef = buf[i] >> 7;
    // TODO: parse psd [i, la_location]
    i = hdr.la_location + 1;
    seq = hdr.seq;
    for (j = 0; j < hdr.nop; ++j)
    {
        unsigned int cnt = hdr.locations[j] - i;
        unsigned int crc = buf[i + cnt];

        if (crc8(&buf[i], cnt) != crc)
        {
            log_warn("crc mismatch!");
            st->ready = 0;
            break;
        }

        if (j == 0 && hdr.pfirst)
        {
            if (st->pdu_idx)
            {
                memcpy(&st->pdu[st->pdu_idx], &buf[i], cnt);
                if (st->ready)
                    input_pdu_push(st->input, st->pdu, cnt + st->pdu_idx);
            }
            else
            {
                log_debug("ignoring partial pdu");
            }
        }
        else if (j == hdr.nop - 1 && hdr.plast)
        {
            memcpy(st->pdu, &buf[i], cnt);
            st->pdu_idx = cnt;

            if (seq == 0)
                st->ready = 1;
            seq = (seq + 1) % 64;
        }
        else
        {
            if (seq == 0)
                st->ready = 1;
            seq = (seq + 1) % 64;
            if (st->ready)
                input_pdu_push(st->input, &buf[i], cnt);
        }
        
        i += cnt + 1;
    }
}

void frame_push(frame_t *st, uint8_t *bits)
{
    const unsigned int start = 146152 - 30000 + 24, offset = 1248, hbits = 24;
    unsigned int i, j = 0, h = 0, header = 0, val = 0;
    uint8_t *ptr = st->buffer;
    for (i = 0; i < 146176; ++i)
    {
        // swap bit order
        uint8_t bit = bits[((i>>3)<<3) + 7 - (i & 7)];
        if (i >= start && ((i - start) % offset) == 0 && h < hbits)
        {
            header |= bit << h;
            ++h;
        }
        else
        {
            val |= bit << (7 - j);
            if (++j == 8)
            {
                *ptr++ = val;
                val = 0;
                j = 0;
            }
        }
    }
    
    // log_debug("PCI %x", header);

    st->pci = header;
    frame_process(st);
}

void frame_reset(frame_t *st)
{
    st->pdu_idx = 0;
    st->pci = 0;
    st->ready = 0;
}

void frame_set_program(frame_t *st, unsigned int program)
{
    st->program = program;
}

void frame_init(frame_t *st, input_t *input)
{
    st->input = input;
    st->buffer = malloc(146152);
    st->pdu = malloc(0x10000);

    rs_init();

    frame_reset(st);
}
