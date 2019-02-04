#include <stdlib.h>

#include "unicode.h"

char *iso_8859_1_to_utf_8(uint8_t *buf, unsigned int len)
{
    unsigned int i, j;
    char *out = malloc(len * 2 + 1);

    j = 0;
    for (i = 0; i < len; i++)
    {
        uint8_t ch = buf[i];

        if (ch < 0x80)
        {
            out[j++] = ch;
        }
        else
        {
            out[j++] = 0xc0 | (ch >> 6);
            out[j++] = 0x80 | (ch & 0x3f);
        }
    }

    out[j] = 0;
    return out;
}

char *ucs_2_to_utf_8(uint8_t *buf, unsigned int len)
{
    unsigned int i = 0, j = 0;
    unsigned int big_endian = 0;
    char *out = malloc((len / 2) * 3 + 1);

    if (len >= 2)
    {
        if ((buf[0] == 0xfe) && (buf[1] == 0xff))
        {
            big_endian = 1;
            i += 2;
        }
        else if ((buf[0] == 0xff) && (buf[1] == 0xfe))
        {
            big_endian = 0;
            i += 2;
        }
    }

    for (; i < len; i += 2)
    {
        uint16_t ch;

        if (big_endian)
            ch = (buf[i] << 8) | buf[i+1];
        else
            ch = buf[i] | (buf[i+1] << 8);

        if (ch < 0x80)
        {
            out[j++] = ch;
        }
        else if (ch < 0x800)
        {
            out[j++] = 0xc0 | (ch >> 6);
            out[j++] = 0x80 | (ch & 0x3f);
        }
        else
        {
            out[j++] = 0xe0 | (ch >> 12);
            out[j++] = 0x80 | ((ch >> 6) & 0x3f);
            out[j++] = 0x80 | (ch & 0x3f);
        }
    }

    out[j] = 0;
    return out;
}
