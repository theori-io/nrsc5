#pragma once

#include <stdint.h>

typedef struct
{
    unsigned int byte;
    unsigned int bits;
    uint8_t *buf, *begin;
} bitwriter_t;

static inline void bw_init(bitwriter_t *bw, uint8_t *buf)
{
    bw->byte = 0;
    bw->bits = 0;
    bw->buf = buf;
    bw->begin = buf;
}

static inline void bw_add1bit(bitwriter_t *bw, unsigned int bit)
{
    bw->byte = (bw->byte << 1) | (!!bit);
    if (++bw->bits == 8)
    {
        *bw->buf++ = bw->byte;
        bw->byte = 0;
        bw->bits = 0;
    }
}

static inline void bw_addbits(bitwriter_t *bw, unsigned int value, unsigned int bits)
{
    unsigned int i;
    for (i = 0; i < bits; ++i)
        bw_add1bit(bw, value & (1 << (bits - i - 1)));
}

static inline unsigned int bw_flush(bitwriter_t *bw)
{
    if (bw->bits)
        bw_addbits(bw, 0, 8 - bw->bits);
    return bw->buf - bw->begin;
}
