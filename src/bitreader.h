#pragma once

#include <assert.h>
#include <stdint.h>

typedef struct
{
    unsigned int byte;
    unsigned int bits;
    uint8_t *buf;
    uint8_t *end;
} bitreader_t;

static inline void br_init(bitreader_t *br, uint8_t *buf, unsigned int length)
{
    br->byte = 0;
    br->bits = 0;
    br->buf = buf;
    br->end = buf + length;
}

static inline unsigned int br_read1bit(bitreader_t *br)
{
    if (br->bits == 0)
    {
        assert(br->buf != br->end);
        br->byte = *br->buf++;
        br->bits = 8;
    }
    br->byte <<= 1;
    br->bits--;
    return !!(br->byte & 0x100);
}

static inline unsigned int br_readbits(bitreader_t *br, unsigned int bits)
{
    unsigned int i, val = 0;
    for (i = 0; i < bits; ++i)
        val = (val << 1) | br_read1bit(br);
    return val;
}

static inline unsigned int br_peekbits(bitreader_t *br, unsigned int bits)
{
    bitreader_t tmp = *br;
    unsigned int val = br_readbits(br, bits);
    *br = tmp;
    return val;
}
