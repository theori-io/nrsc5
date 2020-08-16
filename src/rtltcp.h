#pragma once

#include <stdint.h>

typedef struct rtltcp_t rtltcp_t;

#define RTLTCP_DEFINE(name, opc) \
    int rtltcp_##name(rtltcp_t *, unsigned int param);
RTLTCP_DEFINE(set_center_freq, 0x01)
RTLTCP_DEFINE(set_sample_rate, 0x02)
RTLTCP_DEFINE(set_tuner_gain_mode, 0x03)
RTLTCP_DEFINE(set_tuner_gain, 0x04)
RTLTCP_DEFINE(set_freq_correction, 0x05)
RTLTCP_DEFINE(set_direct_sampling, 0x09)
RTLTCP_DEFINE(set_offset_tuning, 0x0a)
RTLTCP_DEFINE(set_bias_tee, 0x0e)
#undef RTLTCP_DEFINE

rtltcp_t *rtltcp_open(int socket);
void rtltcp_close(rtltcp_t *);
int rtltcp_read(rtltcp_t *, uint8_t *buf, size_t cnt);
int rtltcp_get_tuner_gains(rtltcp_t *, int *gains);
int rtltcp_reset_buffer(rtltcp_t *, size_t cnt);
