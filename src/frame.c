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

#define PCI_AUDIO 0x38D8D3
#define PCI_AUDIO_FIXED 0xE3634C
#define PCI_AUDIO_FIXED_OPP 0x8D8D33

#define BBM 0x42E23A7D
#define MAX_AAS_LEN 8212

typedef struct
{
    unsigned int codec;
    unsigned int stream_id;
    unsigned int pdu_seq;
    unsigned int pfirst;
    unsigned int plast;
    unsigned int seq;
    unsigned int nop;
    unsigned int la_location;
    unsigned int hef;
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

static const uint16_t fcs_tab[] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

/* Good final FCS value */
#define VALIDFCS16 0xf0b8

static uint8_t crc8(const uint8_t *pkt, unsigned int cnt)
{
    unsigned int i, crc = 0xFF;
    for (i = 0; i < cnt; ++i)
        crc = crc8_tab[crc ^ pkt[i]];
    return crc;
}

static uint16_t fcs16(const uint8_t *cp, int len)
{
    uint16_t crc = 0xFFFF;
    while (len--)
        crc = (crc >> 8) ^ fcs_tab[(crc ^ *cp++) & 0xFF];
    return (crc);
}

static int has_fixed(frame_t *st)
{
    return st->pci == PCI_AUDIO_FIXED || st->pci == PCI_AUDIO_FIXED_OPP;
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
    hdr->stream_id = (buf[8] >> 4) & 0x3;
    hdr->pdu_seq = (buf[8] >> 6) | ((buf[9] & 1) << 2);
    hdr->pfirst = (buf[11] >> 1) & 1;
    hdr->plast = (buf[11] >> 2) & 1;
    hdr->seq = (buf[11] >> 3) | ((buf[12] & 1) << 5);
    hdr->nop = (buf[12] >> 1) & 0x3f;
    hdr->la_location = buf[13];
    hdr->hef = buf[12] & 0x80;
}

static unsigned int calc_lc_bits(frame_header_t *hdr)
{
    switch(hdr->codec)
    {
    case 0:
        return 16;
    case 1:
    case 2:
    case 3:
        if (hdr->stream_id == 0)
            return 12;
        else
            return 16;
    case 10:
    case 13:
        return 12;
    default:
        log_warn("unknown codec field (%d)", hdr->codec);
        return 16;
    }
}

static unsigned int parse_location(uint8_t *buf, unsigned int lc_bits, unsigned int i)
{
    if (lc_bits == 16)
        return (buf[14 + 2*i + 1] << 8) | buf[14 + 2*i];
    else
    {
        if (i % 2 == 0)
            return ((buf[14 + i/2*3 + 1] & 0xf) << 8) | buf[14 + i/2*3];
        else
            return (buf[14 + i/2*3 + 2] << 4) | (buf[14 + i/2*3 + 1] >> 4);
    }
}

static int find_program(uint8_t *buf, int program, size_t length)
{
    unsigned int i = 0, lc_bits;

    while (i < length - 96)
    {
        unsigned int start = i;
        frame_header_t hdr;

        if (!fix_header(&buf[i]))
            return -1;

        parse_header(&buf[i], &hdr);
        lc_bits = calc_lc_bits(&hdr);
        i += 14 + ((lc_bits * hdr.nop) + 4) / 8;

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
        else
        {
            if (program == 0)
                return start;
        }

        // skip remaining bytes using last location
        i = start + parse_location(buf + start, lc_bits, hdr.nop - 1) + 1;
    }

    return -1;
}

static int unescape_hdlc(uint8_t *data, int length)
{
    uint8_t *p = data;

    for (int i = 0; i < length; i++)
    {
        if (data[i] == 0x7D)
            *p++ = data[++i] | 0x20;
        else
            *p++ = data[i];
    }

    return p - data;
}

static void aas_push(frame_t *st, uint8_t* psd, int length)
{
    length = unescape_hdlc(psd, length);

    if (length == 0)
    {
        // empty frames are used as padding
    }
    else if (fcs16(psd, length) != VALIDFCS16)
    {
        log_info("psd crc mismatch");
    }
    else if (psd[0] != 0x21)
    {
        log_warn("unknown AAS protocol %x", psd[0]);
    }
    else
    {
        // remove protocol and fcs fields
        input_aas_push(st->input, psd + 1, length - 2);
    }
}

// AAS transport uses HDLC-like framing
static void parse_psd(frame_t *st, uint8_t *data, size_t length)
{
    uint8_t *p;
    int index = 0;

    // find "flag"
    p = memchr(data, 0x7E, length);
    if (p)
    {
        index = p - data;

        if (st->psd_idx >= 0)
        {
            // complete current psd frame
            if (st->psd_idx)
            {
                if (index + st->psd_idx > 2048)
                    goto overflow;
                memcpy(&st->psd_buf[st->psd_idx], data, index);
                aas_push(st, st->psd_buf, index + st->psd_idx);
                st->psd_idx = 0;
            }
        }
        else
        {
            st->psd_idx = 0;
        }

        // skip the flag
        index++;

        // check for complete psd in one frame
        while (1)
        {
            p = memchr(&data[index], 0x7E, length - index);
            if (p == NULL)
                break;

            int cur_index = p - data;
            aas_push(st, &data[index], cur_index - index);
            index = cur_index + 1;
        }
    }

    if (st->psd_idx >= 0)
    {
        // store remaining bytes
        if (length - index + st->psd_idx > 2048)
        {
overflow:
            log_error("psd buffer overflow");
            st->psd_idx = 0;
            return;
        }

        memcpy(&st->psd_buf[st->psd_idx], &data[index], length - index);
        st->psd_idx += length - index;
    }
}

static void process_fixed_ccc(frame_t *st)
{
    const uint8_t *buf = st->ccc_buf;

    // padding
    if (st->ccc_idx == 0)
        return;

    // ignore new CCC packets (XXX they shouldn't change)
    if (st->fixed_ready)
        return;

    if (fcs16(buf, st->ccc_idx) != VALIDFCS16)
    {
        log_info("bad CCC checksum");
        return;
    }

    for (unsigned int i = 0; i < 4; i++)
    {
        fixed_subchannel_t *subch = &st->subchannel[i];
        subch->mode = 0;
        subch->length = 0;

        if (5 + i * 4 <= st->ccc_idx)
        {
            uint16_t mode = *(uint16_t *)&buf[1 + i * 4];
            uint16_t length = *(uint16_t *)&buf[3 + i * 4];
            log_info("Subchannel %d: mode=%d, length=%d", i, mode, length);

            if (mode == 0)
            {
                subch->mode = mode;
                subch->length = length;
                subch->block_idx = 0;
                subch->blocks = malloc(255 + 4);
                subch->idx = -1;
                subch->data = malloc(MAX_AAS_LEN);
            }
            else
            {
                log_warn("Subchannel mode %04X not supported", mode);
            }
        }
    }

    st->fixed_ready = 1;
}

/* FIXME: We only support mode=0 (no FEC, no interleaving) */
static void process_fixed_block(frame_t *st, int i)
{
    fixed_subchannel_t *subch = &st->subchannel[i];
    uint8_t *p = &subch->blocks[4];
    for (int j = 0; j < 255; j++)
    {
        uint8_t byte = p[j];
        // TODO eliminate duplication with parse_psd
        if (subch->idx >= 0)
        {
            if (byte == 0x7E)
            {
                aas_push(st, subch->data, subch->idx);
                subch->idx = 0;
            }
            else
            {
                subch->data[subch->idx++] = byte;
            }
        }
        else
        {
            if (byte == 0x7E) subch->idx = 0;
        }
    }
}

static void process_fixed_data(frame_t *st)
{
    unsigned int sync = st->buffer[PDU_LEN - 1];

    if (st->sync_count < 2)
    {
        unsigned int width = (sync & 0xF) * 2;
        if (st->sync_width == width)
            st->sync_count++;
        else
            st->sync_count = 0;
        st->sync_width = width;

        if (st->sync_count < 2)
            return;
    }

    for (unsigned int i = 0; i < st->sync_width; i++)
    {
        uint8_t byte = st->buffer[PDU_LEN - st->sync_width - 1 + i];
        // TODO eliminate duplication with parse_psd
        if (byte == 0x7E)
        {
            unescape_hdlc(st->ccc_buf, st->ccc_idx);
            process_fixed_ccc(st);
            st->ccc_idx = 0;
        }
        else
        {
            st->ccc_buf[st->ccc_idx++] = byte;
        }

        if (st->ccc_idx == sizeof(st->ccc_buf))
        {
            log_info("CCC overflow");
            st->ccc_idx = 0;
        }
    }

    // wait until we have subchannel information
    if (!st->fixed_ready)
        return;

    uint8_t *p = &st->buffer[PDU_LEN - st->sync_width - 1];
    for (int i = 3; i >= 0; i--)
    {
        fixed_subchannel_t *subch = &st->subchannel[i];
        int length = subch->length;

        if (length == 0)
            continue;

        p -= length;
        for (int j = 0; j < length; j++)
        {
            subch->blocks[subch->block_idx++] = p[j];
            if (subch->block_idx == 4 && *(uint32_t *)subch->blocks != BBM)
            {
                // mis-aligned, skip a byte
                memmove(subch->blocks, subch->blocks + 1, 3);
                subch->block_idx--;
            }

            if (subch->block_idx == 255 + 4)
            {
                // we have a complete block, deinterleave and process
                process_fixed_block(st, i);
                subch->block_idx = 0;
            }
        }
    }
}

void frame_process(frame_t *st, size_t length)
{
    int offset;
    unsigned int i, j, hef, seq, lc_bits;
    uint8_t *buf;
    frame_header_t hdr;

    if (has_fixed(st))
        process_fixed_data(st);

    offset = find_program(st->buffer, st->program, length);
    if (offset == -1)
        return;

    buf = &st->buffer[offset];
    parse_header(buf, &hdr);

    lc_bits = calc_lc_bits(&hdr);

    i = 14 + ((lc_bits * hdr.nop) + 4) / 8;
    // skip extended headers
    for (hef = hdr.hef; hef; ++i)
    {
        if (i >= length) return;
        hef = buf[i] >> 7;
    }

    if (hdr.la_location < i || hdr.la_location >= length) return;
    parse_psd(st, &buf[i], hdr.la_location-i+1);
    i = hdr.la_location + 1;
    seq = hdr.seq;
    for (j = 0; j < hdr.nop; ++j)
    {
        unsigned int location = parse_location(buf, lc_bits, j);
        if (location < i || location >= length) break;
        unsigned int cnt = location - i;
        if (crc8(&buf[i], cnt + 1) != 0)
        {
            log_warn("crc mismatch!");
            i += cnt + 1;
            continue;
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

void frame_push(frame_t *st, uint8_t *bits, size_t length)
{
    unsigned int start, offset;
    unsigned int i, j = 0, h = 0, header = 0, val = 0;
    uint8_t *ptr = st->buffer;

    switch (length)
    {
    case P1_FRAME_LEN:
        start = P1_FRAME_LEN - 30000;
        offset = 1248;
        break;
    case P3_FRAME_LEN:
        start = 120;
        offset = 184;
        break;
    default:
        log_error("Unknown frame length: %d", length);
    }

    for (i = 0; i < length; ++i)
    {
        // swap bit order
        uint8_t bit = bits[((i>>3)<<3) + 7 - (i & 7)];
        if (i >= start && ((i - start) % offset) == 0 && h < PCI_LEN)
        {
            header = (header << 1) | bit;
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
    frame_process(st, ptr - st->buffer);
}

void frame_reset(frame_t *st)
{
    st->pdu_idx = 0;
    st->pci = 0;
    st->ready = 0;
    st->psd_idx = -1;

    st->fixed_ready = 0;
    st->sync_width = 0;
    st->sync_count = 0;
    st->ccc_idx = 0;
}

void frame_set_program(frame_t *st, unsigned int program)
{
    st->program = program;
}

void frame_init(frame_t *st, input_t *input)
{
    st->input = input;
    st->buffer = malloc(PDU_LEN);
    st->pdu = malloc(0x10000);
    st->psd_buf = malloc(2048);

    rs_init();

    frame_reset(st);
}
