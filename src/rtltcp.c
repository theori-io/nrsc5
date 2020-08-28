#include <stdlib.h>
#include <string.h>
#ifdef __MINGW32__
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <rtl-sdr.h>
#include "defines.h"
#include "rtltcp.h"

struct rtltcp_t
{
    int socket;
    uint32_t tuner_type;
    uint32_t gain_count;
};

typedef struct {
    unsigned char cmd;
    uint32_t param;
} __attribute__((packed)) command_t;

typedef struct {
    char magic[4];
    uint32_t tuner_type;
    uint32_t tuner_gain_count;
} dongle_info_t;

#define RTLTCP_DEFINE(name, opc) \
    int rtltcp_##name(rtltcp_t *st, unsigned int param) \
    { \
        command_t cmd = { .cmd = opc, .param = htonl(param) }; \
        return send(st->socket, (char *)&cmd, sizeof(cmd), 0) == -1 ? errno : 0; \
    }

RTLTCP_DEFINE(set_center_freq, 0x01)
RTLTCP_DEFINE(set_sample_rate, 0x02)
RTLTCP_DEFINE(set_tuner_gain_mode, 0x03)
RTLTCP_DEFINE(set_tuner_gain, 0x04)
RTLTCP_DEFINE(set_freq_correction, 0x05)
RTLTCP_DEFINE(set_direct_sampling, 0x09)
RTLTCP_DEFINE(set_offset_tuning, 0x0a)
RTLTCP_DEFINE(set_bias_tee, 0x0e)

#undef RTLTCP_DEFINE

rtltcp_t *rtltcp_open(int socket)
{
    dongle_info_t dongle_info;
    rtltcp_t *st = calloc(1, sizeof(*st));
    st->socket = socket;

    if (rtltcp_read(st, (void *)&dongle_info, sizeof(dongle_info)) != sizeof(dongle_info))
        goto error;

    if (memcmp(dongle_info.magic, "RTL0", 4) != 0)
        goto error;

    st->tuner_type = htonl(dongle_info.tuner_type);
    st->gain_count = htonl(dongle_info.tuner_gain_count);
    return st;
error:
    free(st);
    return NULL;
}

void rtltcp_close(rtltcp_t *st)
{
#ifdef __MINGW32__
    closesocket(st->socket);
#else
    close(st->socket);
#endif
    free(st);
}

int rtltcp_read(rtltcp_t *st, uint8_t *buf, size_t cnt)
{
    int offset = 0;
    while (cnt > 0)
    {
        int err = recv(st->socket, (char *)buf + offset, cnt, 0);
        if (err < 0)
            return err;
        else if (err == 0)
            break;

        cnt -= err;
        offset += err;
    }
    return offset;
}

// from https://github.com/steve-m/librtlsdr/blob/master/src/librtlsdr.c
int rtltcp_get_tuner_gains(rtltcp_t *st, int *gains)
{
    /* all gain values are expressed in tenths of a dB */
    const int e4k_gains[] = { -10, 15, 40, 65, 90, 115, 140, 165, 190, 215,
        240, 290, 340, 420 };
    const int fc0012_gains[] = { -99, -40, 71, 179, 192 };
    const int fc0013_gains[] = { -99, -73, -65, -63, -60, -58, -54, 58, 61,
        63, 65, 67, 68, 70, 71, 179, 181, 182,
        184, 186, 188, 191, 197 };
    const int fc2580_gains[] = { 0 /* no gain values */ };
    const int r82xx_gains[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157,
        166, 197, 207, 229, 254, 280, 297, 328,
        338, 364, 372, 386, 402, 421, 434, 439,
        445, 480, 496 };
    const int unknown_gains[] = { 0 /* no gain values */ };

    const int *ptr = NULL;
    int len = 0;

    switch (st->tuner_type) {
        case RTLSDR_TUNER_E4000:
            ptr = e4k_gains; len = sizeof(e4k_gains);
            break;
        case RTLSDR_TUNER_FC0012:
            ptr = fc0012_gains; len = sizeof(fc0012_gains);
            break;
        case RTLSDR_TUNER_FC0013:
            ptr = fc0013_gains; len = sizeof(fc0013_gains);
            break;
        case RTLSDR_TUNER_FC2580:
            ptr = fc2580_gains; len = sizeof(fc2580_gains);
            break;
        case RTLSDR_TUNER_R820T:
        case RTLSDR_TUNER_R828D:
            ptr = r82xx_gains; len = sizeof(r82xx_gains);
            break;
        default:
            log_error("Unknown tuner type: %d", st->tuner_type);
            ptr = unknown_gains; len = sizeof(unknown_gains);
            break;
    }

    if (!gains)
    {
        /* no buffer provided, just return the count */
        return len / sizeof(int);
    }
    else
    {
        if (len)
            memcpy(gains, ptr, len);

        return len / sizeof(int);
    }
}

int rtltcp_reset_buffer(rtltcp_t *st, size_t cnt)
{
    char buf[1024];
    unsigned int recvd = 0;
    int flags = 0;
#ifdef __MINGW32__
    unsigned long mode = 1;
    ioctlsocket(st->socket, FIONBIO, &mode);
#else
    flags |= MSG_DONTWAIT;
#endif
    // first, clear pending data
    while (1)
    {
        int err = recv(st->socket, buf, sizeof(buf), flags);
        if (err <= 0)
            break;
        recvd += err;
    }
#ifdef __MINGW32__
    mode = 0;
    ioctlsocket(st->socket, FIONBIO, &mode);
#endif
    // if we read an odd-number of bytes, read one more byte
    if (recvd & 1)
        recv(st->socket, buf, 1, 0);

    // then, read cnt bytes
    while (cnt > 0)
    {
        int to_read = sizeof(buf);
        if (cnt < to_read)
            to_read = cnt;

        to_read = recv(st->socket, buf, to_read, 0);
        if (to_read <= 0)
            return 1;
        cnt -= to_read;
    }
    return 0;
}
