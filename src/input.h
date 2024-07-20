#pragma once

#include <stdint.h>
#include <complex.h>

#include <nrsc5.h>

#include "acquire.h"
#include "decode.h"
#include "defines.h"
#include "firdecim_q15.h"
#include "frame.h"
#include "output.h"
#include "sync.h"

#define INPUT_BUF_LEN (FFTCP_FM * 512)
#define AM_DECIM_STAGES 5

enum { SYNC_STATE_NONE, SYNC_STATE_COARSE, SYNC_STATE_FINE };

typedef struct input_t
{
    nrsc5_t *radio;
    output_t *output;

    firdecim_q15 decim[AM_DECIM_STAGES];
    cint16_t stages[AM_DECIM_STAGES][2];
    cint16_t buffer[INPUT_BUF_LEN];
    unsigned int avail, used, skip, offset;
    unsigned int sync_state;

    acquire_t acq;
    decode_t decode;
    frame_t frame;
    sync_t sync;
} input_t;

void input_init(input_t *st, nrsc5_t *radio, output_t *output);
void input_set_mode(input_t *st);
void input_reset(input_t *st);
void input_free(input_t *st);
void input_set_sync_state(input_t *st, unsigned int new_state);
void input_push_cu8(input_t *st, const uint8_t *buf, uint32_t len);
void input_push_cs16(input_t *st, const int16_t *buf, uint32_t len);
void input_set_skip(input_t *st, unsigned int skip);
