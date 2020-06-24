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
RTLTCP_DEFINE(set_offset_tuning, 0x0a)

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
