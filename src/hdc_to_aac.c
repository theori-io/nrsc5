/*
** This code is heavily based on code from FAAD2, and includes some code
** verbatim. It is licensed under the GPL version 2 or later, same as the
** original license.
**
** NB This code does not allow you to directly play AAC or HDC files. It does
** not contain any code to reconstruct the audio. Its only purpose is to parse
** the file structure, and dump it back out as a conformant AAC file.
**
** Author: Andrew Wesie
**
**
**
** The original copyright notice and license, which is likely applicable to
** this code as well:
**
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** The "appropriate copyright message" mentioned in section 2c of the GPLv2
** must read: "Code from FAAD2 is copyright (c) Nero AG, www.nero.com"
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
**
*/
#include <stdint.h>
#include <stdio.h>

#include "bitreader.h"
#include "bitwriter.h"

#define ERR(x,...) fprintf(stderr, x, ##__VA_ARGS__)

/* Bitstream */
#define LEN_SE_ID 3
#define LEN_TAG   4
#define LEN_BYTE  8

#define EXT_FIL            0
#define EXT_FILL_DATA      1
#define EXT_DATA_ELEMENT   2
#define EXT_DYNAMIC_RANGE 11
#define EXT_SBR_DATA     13
#define ANC_DATA           0

/* Syntax elements */
#define ID_SCE 0x0
#define ID_CPE 0x1
#define ID_CCE 0x2
#define ID_LFE 0x3
#define ID_DSE 0x4
#define ID_PCE 0x5
#define ID_FIL 0x6
#define ID_END 0x7

#define ONLY_LONG_SEQUENCE   0x0
#define LONG_START_SEQUENCE  0x1
#define EIGHT_SHORT_SEQUENCE 0x2
#define LONG_STOP_SEQUENCE   0x3

#define ZERO_HCB       0
#define FIRST_PAIR_HCB 5
#define ESC_HCB        11
#define QUAD_LEN       4
#define PAIR_LEN       2
#define NOISE_HCB      13
#define INTENSITY_HCB2 14
#define INTENSITY_HCB  15

typedef struct
{
    uint8_t id;
    uint8_t common_window;
    uint8_t max_sfb;

    uint8_t num_swb;
    uint8_t num_window_groups;
    uint8_t num_windows;
    uint8_t window_shape;
    uint8_t window_sequence;
    uint8_t window_group_length[8];
    uint8_t scale_factor_grouping;
    uint16_t sect_sfb_offset[8][15*8];
    uint16_t swb_offset[52];
    uint16_t swb_offset_max;

    uint8_t sect_cb[8][15*8];
    uint16_t sect_start[8][15*8];
    uint16_t sect_end[8][15*8];
    uint8_t sfb_cb[8][8*15];
    uint8_t num_sec[8]; /* number of sections in a group */

    struct {
        uint8_t present;
        uint8_t n_filt[8];
        uint8_t coef_res[8];
        uint8_t length[8];
        uint8_t order[8];
        uint8_t direction[8];
        uint8_t coef_compress[8];
        uint8_t coef[8][32];
    } tns;
} ics_t;

/* 1st step table */
typedef struct
{
    uint8_t offset;
    uint8_t extra_bits;
} hcb;

/* 2nd step table with quadruple data */
typedef struct
{
    uint8_t bits;
    int8_t x;
    int8_t y;
} hcb_2_pair;

typedef struct
{
    uint8_t bits;
    int8_t x;
    int8_t y;
    int8_t v;
    int8_t w;
} hcb_2_quad;

/* binary search table */
typedef struct
{
    uint8_t is_leaf;
    int8_t data[4];
} hcb_bin_quad;

typedef struct
{
    uint8_t is_leaf;
    int8_t data[2];
} hcb_bin_pair;

#include "codebooks/hcb_1.h"
#include "codebooks/hcb_2.h"
#include "codebooks/hcb_3.h"
#include "codebooks/hcb_4.h"
#include "codebooks/hcb_5.h"
#include "codebooks/hcb_6.h"
#include "codebooks/hcb_7.h"
#include "codebooks/hcb_8.h"
#include "codebooks/hcb_9.h"
#include "codebooks/hcb_10.h"
#include "codebooks/hcb_11.h"
#include "codebooks/hcb_sf.h"

static hcb *hcb_table[] = {
    0, hcb1_1, hcb2_1, 0, hcb4_1, 0, hcb6_1, 0, hcb8_1, 0, hcb10_1, hcb11_1
};

static hcb_2_quad *hcb_2_quad_table[] = {
    0, hcb1_2, hcb2_2, 0, hcb4_2, 0, 0, 0, 0, 0, 0, 0
};

static hcb_2_pair *hcb_2_pair_table[] = {
    0, 0, 0, 0, 0, 0, hcb6_2, 0, hcb8_2, 0, hcb10_2, hcb11_2
};

static hcb_bin_pair *hcb_bin_table[] = {
    0, 0, 0, 0, 0, hcb5, 0, hcb7, 0, hcb9, 0, 0
};

static uint8_t hcbN[] = { 0, 5, 5, 0, 5, 0, 5, 0, 5, 0, 6, 5 };

static void huffman_scale_factor(bitreader_t *br, bitwriter_t *bw)
{
    uint16_t offset = 0;

    while (hcb_sf[offset][1])
    {
        uint8_t b = br_readbits(br, 1);
        bw_addbits(bw, b, 1);
        offset += hcb_sf[offset][b];
    }
}

static void huffman_sign_bits(bitreader_t *br, bitwriter_t *bw, uint8_t len)
{
    bw_addbits(bw, br_readbits(br, len), len);
}

static void huffman_getescape(bitreader_t *br, bitwriter_t *bw, int16_t sp)
{
    if (sp != 16)
        return;
    uint8_t i;
    for (i = 4; ; i++)
    {
        uint8_t b = br_readbits(br, 1);
        bw_addbits(bw, b, 1);
        if (b == 0)
            break;
    }

    bw_addbits(bw, br_readbits(br, i), i);
}

static uint8_t huffman_2step_quad(bitreader_t *br, bitwriter_t *bw, uint8_t cb)
{
    uint32_t cw;
    uint16_t offset = 0;
    uint8_t extra_bits;

    cw = br_peekbits(br, hcbN[cb]);
    offset = hcb_table[cb][cw].offset;
    extra_bits = hcb_table[cb][cw].extra_bits;

    if (extra_bits)
    {
        bw_addbits(bw, br_readbits(br, hcbN[cb]), hcbN[cb]);
        offset += (uint16_t)br_peekbits(br, extra_bits);
        bw_addbits(bw, br_readbits(br, hcb_2_quad_table[cb][offset].bits - hcbN[cb]), hcb_2_quad_table[cb][offset].bits - hcbN[cb]);
    } else {
        bw_addbits(bw, br_readbits(br, hcb_2_quad_table[cb][offset].bits), hcb_2_quad_table[cb][offset].bits);
    }

    uint8_t cnt = 0;
    if (hcb_2_quad_table[cb][offset].x) cnt++;
    if (hcb_2_quad_table[cb][offset].y) cnt++;
    if (hcb_2_quad_table[cb][offset].v) cnt++;
    if (hcb_2_quad_table[cb][offset].w) cnt++;
    return cnt;
}

static void huffman_2step_quad_sign(bitreader_t *br, bitwriter_t *bw, uint8_t cb)
{
    uint8_t sp = huffman_2step_quad(br, bw, cb);
    huffman_sign_bits(br, bw, sp);
}

static uint8_t huffman_2step_pair(bitreader_t *br, bitwriter_t *bw, uint8_t cb, int16_t sp[2])
{
    uint32_t cw;
    uint16_t offset = 0;
    uint8_t extra_bits;

    cw = br_peekbits(br, hcbN[cb]);
    offset = hcb_table[cb][cw].offset;
    extra_bits = hcb_table[cb][cw].extra_bits;

    if (extra_bits)
    {
        bw_addbits(bw, br_readbits(br, hcbN[cb]), hcbN[cb]);
        offset += (uint16_t)br_peekbits(br, extra_bits);
        bw_addbits(bw, br_readbits(br, hcb_2_pair_table[cb][offset].bits - hcbN[cb]), hcb_2_pair_table[cb][offset].bits - hcbN[cb]);
    } else {
        bw_addbits(bw, br_readbits(br, hcb_2_pair_table[cb][offset].bits), hcb_2_pair_table[cb][offset].bits);
    }

    if (sp)
    {
        sp[0] = hcb_2_pair_table[cb][offset].x;
        sp[1] = hcb_2_pair_table[cb][offset].y;
    }

    uint8_t cnt = 0;
    if (hcb_2_pair_table[cb][offset].x) cnt++;
    if (hcb_2_pair_table[cb][offset].y) cnt++;
    return cnt;
}

static void huffman_2step_pair_sign(bitreader_t *br, bitwriter_t *bw, uint8_t cb, int16_t sp[2])
{
    uint8_t cnt = huffman_2step_pair(br, bw, cb, sp);
    huffman_sign_bits(br, bw, cnt);
}

static uint8_t huffman_binary_quad(bitreader_t *br, bitwriter_t *bw, uint8_t cb)
{
    uint16_t offset = 0;

    while (!hcb3[offset].is_leaf)
    {
        uint8_t b = br_readbits(br, 1);
        bw_addbits(bw, b, 1);
        offset += hcb3[offset].data[b];
    }

    uint8_t cnt = 0;
    if (hcb3[offset].data[0]) cnt++;
    if (hcb3[offset].data[1]) cnt++;
    if (hcb3[offset].data[2]) cnt++;
    if (hcb3[offset].data[3]) cnt++;
    return cnt;
}

static void huffman_binary_quad_sign(bitreader_t *br, bitwriter_t *bw, uint8_t cb)
{
    uint8_t sp = huffman_binary_quad(br, bw, cb);
    huffman_sign_bits(br, bw, sp);
}

static uint8_t huffman_binary_pair(bitreader_t *br, bitwriter_t *bw, uint8_t cb)
{
    uint16_t offset = 0;

    while (!hcb_bin_table[cb][offset].is_leaf)
    {
        uint8_t b = br_readbits(br, 1);
        bw_addbits(bw, b, 1);
        offset += hcb_bin_table[cb][offset].data[b];
    }

    uint8_t cnt = 0;
    if (hcb_bin_table[cb][offset].data[0]) cnt++;
    if (hcb_bin_table[cb][offset].data[1]) cnt++;
    return cnt;
}

static void huffman_binary_pair_sign(bitreader_t *br, bitwriter_t *bw, uint8_t cb)
{
    uint8_t sp = huffman_binary_pair(br, bw, cb);
    huffman_sign_bits(br, bw, sp);
}

static void huffman_spectral_data(bitreader_t *br, bitwriter_t *bw, uint8_t cb)
{
    switch (cb)
    {
    case 1:
    case 2:
         huffman_2step_quad(br, bw, cb);
         break;
    case 3:
         huffman_binary_quad_sign(br, bw, cb);
         break;
    case 4:
         huffman_2step_quad_sign(br, bw, cb);
         break;
    case 5:
         huffman_binary_pair(br, bw, cb);
         break;
    case 6:
         huffman_2step_pair(br, bw, cb, NULL);
         break;
    case 7:
    case 9:
         huffman_binary_pair_sign(br, bw, cb);
         break;
    case 8:
    case 10:
         huffman_2step_pair_sign(br, bw, cb, NULL);
         break;
    case 12:
         huffman_2step_pair(br, bw, 11, NULL);
         break;
    case 11: {
             int16_t sp[2];
             huffman_2step_pair_sign(br, bw, 11, sp);
             huffman_getescape(br, bw, sp[0]);
             huffman_getescape(br, bw, sp[1]);
             break;
         }
    }
}

static void parse_sbr_header(bitreader_t *br, bitwriter_t *bw, bitwriter_t *sbrbw)
{
    bw_addbits(sbrbw, br_readbits(br, 1), 1);

    bw_addbits(sbrbw, br_readbits(br, 4), 4);
    bw_addbits(sbrbw, br_readbits(br, 4), 4);
    bw_addbits(sbrbw, br_readbits(br, 3), 3);
    bw_addbits(sbrbw, br_readbits(br, 2), 2);
    uint8_t extra1 = br_readbits(br, 1);
    bw_addbits(sbrbw, extra1, 1);
    uint8_t extra2 = br_readbits(br, 1);
    bw_addbits(sbrbw, extra2, 1);

    if (extra1)
    {
        bw_addbits(sbrbw, br_readbits(br, 2), 2);
        bw_addbits(sbrbw, br_readbits(br, 1), 1);
        bw_addbits(sbrbw, br_readbits(br, 2), 2);
    }

    if (extra2)
    {
        bw_addbits(sbrbw, br_readbits(br, 2), 2);
        bw_addbits(sbrbw, br_readbits(br, 2), 2);
        bw_addbits(sbrbw, br_readbits(br, 1), 1);
        bw_addbits(sbrbw, br_readbits(br, 1), 1);
    }
}

static void parse_sbr_single_channel_element(bitreader_t *br, bitwriter_t *bw, bitwriter_t *sbrbw)
{
    uint8_t b = br_readbits(br, 1);
    bw_addbits(sbrbw, b, 1);
    if (b)
    {
        bw_addbits(sbrbw, br_readbits(br, 4), 4);
    }

    br_readbits(br, 1); // FIXME HDC specific?

    // XXX I'm lazy copy remaining bits verbatim
    int remaining = br->bits + (br->end - br->buf)*8;
    while (remaining--)
        bw_addbits(sbrbw, br_readbits(br, 1), 1);
}

static void parse_sbr_channel_pair_element(bitreader_t *br, bitwriter_t *bw, bitwriter_t *sbrbw)
{
    uint8_t b = br_readbits(br, 1);
    bw_addbits(sbrbw, b, 1);
    if (b)
    {
        bw_addbits(sbrbw, br_readbits(br, 4), 4);
        bw_addbits(sbrbw, br_readbits(br, 4), 4);
    }

    // XXX I'm lazy copy remaining bits verbatim
    int remaining = br->bits + (br->end - br->buf)*8;
    while (remaining--)
        bw_addbits(sbrbw, br_readbits(br, 1), 1);
}

static void parse_sbr(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    bw_addbits(bw, ID_FIL, LEN_SE_ID);

    uint8_t sbr[272];
    bitreader_t sbrbr;
    bitwriter_t sbrbw;
    br_init(&sbrbr, sbr, 272);
    bw_init(&sbrbw, sbr);

    bw_addbits(&sbrbw, EXT_SBR_DATA, 4);
    uint8_t header_flag = br_readbits(br, 1);
    bw_addbits(&sbrbw, header_flag, 1);
    if (header_flag)
        parse_sbr_header(br, bw, &sbrbw);

    if (ics->id == ID_SCE)
        parse_sbr_single_channel_element(br, bw, &sbrbw);
    else
        parse_sbr_channel_pair_element(br, bw, &sbrbw);

    int bytes = bw_flush(&sbrbw);

    if (bytes < 15)
    {
        bw_addbits(bw, bytes, 4);
    }
    else
    {
        bw_addbits(bw, 15, 4);
        bw_addbits(bw, bytes + 1 - 15, 8);
    }

    for (int i = 0; i < bytes*8; ++i)
        bw_addbits(bw, br_readbits(&sbrbr, 1), 1);
}

static void parse_fil(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    if (br_readbits(br, 3) == 6 && br_readbits(br, 1))
    {
        parse_sbr(br, bw, ics);
    }
}

static const uint8_t num_swb_1024_window[] =
{
    41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40
};

static const uint8_t num_swb_128_window[] =
{
    12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15
};

static const uint16_t swb_offset_1024_96[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56,
    64, 72, 80, 88, 96, 108, 120, 132, 144, 156, 172, 188, 212, 240,
    276, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024
};

static const uint16_t swb_offset_128_96[] =
{
    0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128
};

static const uint16_t swb_offset_1024_64[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56,
    64, 72, 80, 88, 100, 112, 124, 140, 156, 172, 192, 216, 240, 268,
    304, 344, 384, 424, 464, 504, 544, 584, 624, 664, 704, 744, 784, 824,
    864, 904, 944, 984, 1024
};

static const uint16_t swb_offset_128_64[] =
{
    0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128
};

static const uint16_t swb_offset_1024_48[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72,
    80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292,
    320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736,
    768, 800, 832, 864, 896, 928, 1024
};

static const uint16_t swb_offset_128_48[] =
{
    0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128
};

static const uint16_t swb_offset_1024_32[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72,
    80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292,
    320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736,
    768, 800, 832, 864, 896, 928, 960, 992, 1024
};

static const uint16_t swb_offset_1024_24[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68,
    76, 84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220,
    240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552, 600, 652, 704,
    768, 832, 896, 960, 1024
};

static const uint16_t swb_offset_128_24[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128
};

static const uint16_t swb_offset_1024_16[] =
{
    0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 100, 112, 124,
    136, 148, 160, 172, 184, 196, 212, 228, 244, 260, 280, 300, 320, 344,
    368, 396, 424, 456, 492, 532, 572, 616, 664, 716, 772, 832, 896, 960, 1024
};

static const uint16_t swb_offset_128_16[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128
};

static const uint16_t swb_offset_1024_8[] =
{
    0, 12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 132, 144, 156, 172,
    188, 204, 220, 236, 252, 268, 288, 308, 328, 348, 372, 396, 420, 448,
    476, 508, 544, 580, 620, 664, 712, 764, 820, 880, 944, 1024
};

static const uint16_t swb_offset_128_8[] =
{
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60, 72, 88, 108, 128
};

static const uint16_t *swb_offset_1024_window[] =
{
    swb_offset_1024_96,      /* 96000 */
    swb_offset_1024_96,      /* 88200 */
    swb_offset_1024_64,      /* 64000 */
    swb_offset_1024_48,      /* 48000 */
    swb_offset_1024_48,      /* 44100 */
    swb_offset_1024_32,      /* 32000 */
    swb_offset_1024_24,      /* 24000 */
    swb_offset_1024_24,      /* 22050 */
    swb_offset_1024_16,      /* 16000 */
    swb_offset_1024_16,      /* 12000 */
    swb_offset_1024_16,      /* 11025 */
    swb_offset_1024_8        /* 8000  */
};

static const  uint16_t *swb_offset_128_window[] =
{
    swb_offset_128_96,       /* 96000 */
    swb_offset_128_96,       /* 88200 */
    swb_offset_128_64,       /* 64000 */
    swb_offset_128_48,       /* 48000 */
    swb_offset_128_48,       /* 44100 */
    swb_offset_128_48,       /* 32000 */
    swb_offset_128_24,       /* 24000 */
    swb_offset_128_24,       /* 22050 */
    swb_offset_128_16,       /* 16000 */
    swb_offset_128_16,       /* 12000 */
    swb_offset_128_16,       /* 11025 */
    swb_offset_128_8         /* 8000  */
};

static void gen_ics_info(bitwriter_t *bw, ics_t *ics)
{
    bw_addbits(bw, 0, 1); // reserved
    bw_addbits(bw, ics->window_sequence, 2);
    bw_addbits(bw, ics->window_shape, 1);

    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE)
    {
        bw_addbits(bw, ics->max_sfb, 4);
        bw_addbits(bw, ics->scale_factor_grouping, 7);
    }
    else
    {
        bw_addbits(bw, ics->max_sfb, 6);
    }

    if (ics->window_sequence != EIGHT_SHORT_SEQUENCE)
    {
        bw_addbits(bw, 0, 1); // predictor_data_present
    }
}

static void parse_ics_info(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    uint8_t i, g;
    uint8_t sf_index = 7; // 22050

    br_readbits(br, 1); // reserved
    ics->window_shape = br_readbits(br, 1); // window shape
    ics->window_sequence = br_readbits(br, 2); // window sequence

    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE)
    {
        ics->max_sfb = br_readbits(br, 4); // max_sfb
        ics->scale_factor_grouping = br_readbits(br, 7); // scale_factor_grouping
    }
    else
    {
        ics->max_sfb = br_readbits(br, 6); // max_sfb
    }

    switch (ics->window_sequence)
    {
    case ONLY_LONG_SEQUENCE:
    case LONG_START_SEQUENCE:
    case LONG_STOP_SEQUENCE:
        ics->num_windows = 1;
        ics->num_window_groups = 1;
        ics->window_group_length[ics->num_window_groups-1] = 1;
        ics->num_swb = num_swb_1024_window[sf_index];
        for (i = 0; i < ics->num_swb; i++)
        {
            ics->sect_sfb_offset[0][i] = swb_offset_1024_window[sf_index][i];
            ics->swb_offset[i] = swb_offset_1024_window[sf_index][i];
        }
        ics->sect_sfb_offset[0][ics->num_swb] = 1024;
        ics->swb_offset[ics->num_swb] = 1024;
        ics->swb_offset_max = 1024;
        break;
    case EIGHT_SHORT_SEQUENCE:
        ics->num_windows = 8;
        ics->num_window_groups = 1;
        ics->window_group_length[ics->num_window_groups-1] = 1;
        ics->num_swb = num_swb_128_window[sf_index];

        for (i = 0; i < ics->num_swb; i++)
            ics->swb_offset[i] = swb_offset_128_window[sf_index][i];
        ics->swb_offset[ics->num_swb] = 128;
        ics->swb_offset_max = 128;

        for (i = 0; i < ics->num_windows-1; i++) {
            if (ics->scale_factor_grouping & (1 << (6-i)))
            {
                ics->window_group_length[ics->num_window_groups-1] += 1;
            } else {
                ics->num_window_groups += 1;
                ics->window_group_length[ics->num_window_groups-1] = 1;
            }
        }

        for (g = 0; g < ics->num_window_groups; g++)
        {
            uint16_t width;
            uint8_t sect_sfb = 0;
            uint16_t offset = 0;

            for (i = 0; i < ics->num_swb; i++)
            {
                if (i+1 == ics->num_swb)
                {
                    width = 128 - swb_offset_128_window[sf_index][i];
                } else {
                    width = swb_offset_128_window[sf_index][i+1] -
                        swb_offset_128_window[sf_index][i];
                }
                width *= ics->window_group_length[g];
                ics->sect_sfb_offset[g][sect_sfb++] = offset;
                offset += width;
            }
            ics->sect_sfb_offset[g][sect_sfb] = offset;
        }
        break;
    }
}

static void parse_tns_data(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    uint8_t w, i, start_coef_bits, coef_bits;
    uint8_t length_bits = 6;
    uint8_t order_bits = 5;

    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE)
    {
        length_bits = 4;
        order_bits = 3;
    }

    for (w = 0; w < ics->num_windows; w++)
    {
        if (ics->window_sequence == EIGHT_SHORT_SEQUENCE)
        {
            ics->tns.n_filt[w] = br_readbits(br, 1);
        }
        else
        {
            ics->tns.n_filt[w] = 1;
        }

        if (ics->tns.n_filt[w])
        {
            ics->tns.coef_res[w] = br_readbits(br, 1);
            if (ics->tns.coef_res[w])
            {
                start_coef_bits = 4;
            }
            else
            {
                start_coef_bits = 3;
            }

            ics->tns.length[w] = br_readbits(br, length_bits); // length
            ics->tns.order[w] = br_readbits(br, order_bits);
            if (ics->tns.order[w])
            {
                ics->tns.direction[w] = br_readbits(br, 1); // direction
                ics->tns.coef_compress[w] = br_readbits(br, 1);

                coef_bits = start_coef_bits - ics->tns.coef_compress[w];
                for (i = 0; i < ics->tns.order[w]; i++)
                {
                    ics->tns.coef[w][i] = br_readbits(br, coef_bits);
                }
            }
        }
    }
}

static void parse_section_data(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    uint8_t g;
    uint8_t sect_esc_val, sect_bits;
    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE)
        sect_bits = 3;
    else
        sect_bits = 5;
    sect_esc_val = (1<<sect_bits) - 1;

    for (g = 0; g < ics->num_window_groups; g++)
    {
        uint8_t k = 0;
        uint8_t i = 0;

        while (k < ics->max_sfb)
        {
            uint8_t sfb;
            uint8_t sect_len_incr;
            uint16_t sect_len = 0;
            uint8_t sect_cb_bits = 4;

            ics->sect_cb[g][i] = br_readbits(br, sect_cb_bits);
            bw_addbits(bw, ics->sect_cb[g][i], sect_cb_bits);

            sect_len_incr = br_readbits(br, sect_bits);
            bw_addbits(bw, sect_len_incr, sect_bits);

            sect_len += sect_len_incr;
            while (sect_len_incr == sect_esc_val)
            {
                sect_len_incr = br_readbits(br, sect_bits);
                bw_addbits(bw, sect_len_incr, sect_bits);
                sect_len += sect_len_incr;
            }

            ics->sect_start[g][i] = k;
            ics->sect_end[g][i] = k + sect_len;

            for (sfb = k; sfb < k + sect_len; sfb++)
            {
                ics->sfb_cb[g][sfb] = ics->sect_cb[g][i];
            }

            k += sect_len;
            i++;
        }
        ics->num_sec[g] = i;
    }
}

static void parse_scale_factors(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    uint8_t g, sfb;
    int8_t noise_pcm_flag = 1;

    for (g = 0; g < ics->num_window_groups; g++)
    {
        for (sfb = 0; sfb < ics->max_sfb; sfb++)
        {
            switch (ics->sfb_cb[g][sfb])
            {
            case ZERO_HCB: /* zero book */
                break;
            case INTENSITY_HCB: /* intensity books */
            case INTENSITY_HCB2:
                huffman_scale_factor(br, bw);
                break;
            case NOISE_HCB:
                if (noise_pcm_flag)
                {
                    noise_pcm_flag = 0;
                    bw_addbits(bw, br_readbits(br, 9), 9);
                }
                else
                {
                    huffman_scale_factor(br, bw);
                }
                break;
            default:
                huffman_scale_factor(br, bw);
                break;
            }
        }
    }
}

static void gen_tns_data(bitwriter_t *bw, ics_t *ics)
{
    uint8_t w, i, start_coef_bits, coef_bits;
    uint8_t n_filt_bits = 2;
    uint8_t length_bits = 6;
    uint8_t order_bits = 5;

    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE)
    {
        n_filt_bits = 1;
        length_bits = 4;
        order_bits = 3;
    }

    for (w = 0; w < ics->num_windows; w++)
    {
        bw_addbits(bw, ics->tns.n_filt[w], n_filt_bits);
        if (ics->tns.n_filt[w])
        {
            bw_addbits(bw, ics->tns.coef_res[w], 1);
            if (ics->tns.coef_res[w])
            {
                start_coef_bits = 4;
            } else {
                start_coef_bits = 3;
            }

            bw_addbits(bw, ics->tns.length[w], length_bits);
            bw_addbits(bw, ics->tns.order[w], order_bits);
            if (ics->tns.order[w])
            {
                bw_addbits(bw, ics->tns.direction[w], 1);
                bw_addbits(bw, ics->tns.coef_compress[w], 1);
                coef_bits = start_coef_bits - ics->tns.coef_compress[w];
                for(i = 0; i < ics->tns.order[w]; i++)
                {
                    bw_addbits(bw, ics->tns.coef[w][i], coef_bits);
                }
            }
        }
    }
}

static void parse_side_info(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    bw_addbits(bw, br_readbits(br, 8), 8); // global gain

    if (!ics->common_window)
        gen_ics_info(bw, ics);

    parse_section_data(br, bw, ics);
    parse_scale_factors(br, bw, ics);

    bw_addbits(bw, 0, 1); // pulse_data_present
    bw_addbits(bw, ics->tns.present, 1); // tns_data_present
    if (ics->tns.present)
        gen_tns_data(bw, ics);
    bw_addbits(bw, 0, 1); // gain_control_data_present
}

static void parse_spectral_data(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    uint8_t i, g;
    uint16_t inc, k;
    uint8_t sect_cb;
    for(g = 0; g < ics->num_window_groups; g++)
    {
        for (i = 0; i < ics->num_sec[g]; i++)
        {
            sect_cb = ics->sect_cb[g][i];

            inc = (sect_cb >= FIRST_PAIR_HCB) ? 2 : 4;

            switch (sect_cb)
            {
            case ZERO_HCB:
            case NOISE_HCB:
            case INTENSITY_HCB:
            case INTENSITY_HCB2:
                break;
            default:
                for (k = ics->sect_sfb_offset[g][ics->sect_start[g][i]];
                     k < ics->sect_sfb_offset[g][ics->sect_end[g][i]]; k += inc)
                {
                    huffman_spectral_data(br, bw, sect_cb);
                }
                break;
            }
        }
    }
}

static void parse_individual_channel_stream(bitreader_t *br, bitwriter_t *bw, ics_t *ics)
{
    parse_side_info(br, bw, ics);
    parse_spectral_data(br, bw, ics);
}

static void parse_sce(bitreader_t *br, bitwriter_t *bw)
{
    ics_t ics = {
        .id = ID_SCE
    };

    bw_addbits(bw, ics.id, LEN_SE_ID);
    bw_addbits(bw, 0, LEN_TAG);

    parse_ics_info(br, bw, &ics);

    ics.tns.present = br_readbits(br, 1);
    if (ics.tns.present)
        parse_tns_data(br, bw, &ics);

    parse_individual_channel_stream(br, bw, &ics);
    parse_fil(br, bw, &ics);
}

static void parse_cpe(bitreader_t *br, bitwriter_t *bw)
{
    ics_t ics1 = {
        .id = ID_CPE,
        .common_window = 1
    };
    ics_t ics2;

    bw_addbits(bw, ics1.id, LEN_SE_ID);
    bw_addbits(bw, 0, LEN_TAG);
    bw_addbits(bw, ics1.common_window, 1); // common window

    parse_ics_info(br, bw, &ics1);
    gen_ics_info(bw, &ics1);

    uint8_t ms_mask_present = br_readbits(br, 2);
    bw_addbits(bw, ms_mask_present, 2);
    if (ms_mask_present == 1)
    {
        uint8_t g, sfb;
        for (g = 0; g < ics1.num_window_groups; g++)
        {
            for (sfb = 0; sfb < ics1.max_sfb; sfb++)
            {
                bw_addbits(bw, br_readbits(br, 1), 1);
            }
        }
    }
    ics2 = ics1;

    ics1.tns.present = br_readbits(br, 1);
    if (ics1.tns.present)
        parse_tns_data(br, bw, &ics1);
    ics2.tns.present = br_readbits(br, 1);
    if (ics2.tns.present)
        parse_tns_data(br, bw, &ics2);

    parse_individual_channel_stream(br, bw, &ics1);
    parse_individual_channel_stream(br, bw, &ics2);
    parse_fil(br, bw, &ics1);
}

static void parse_packet(bitreader_t *br, bitwriter_t *bw)
{
    uint8_t n = br_readbits(br, 3);
    if (n == 0 || n == 1)
    {
        parse_sce(br, bw);
    }
    else if (n == 2)
    {
        parse_cpe(br, bw);
    }
    else
    {
        ERR("unknown type");
    }

    bw_addbits(bw, ID_END, LEN_SE_ID);
}

void hdc_to_aac(bitreader_t *br, bitwriter_t *bw)
{
    parse_packet(br, bw);
}
