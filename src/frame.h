#pragma once

#include "defines.h"

#define MAX_AAS_LEN 8212
#define RS_BLOCK_LEN 255
#define RS_CODEWORD_LEN 96

typedef struct
{
    uint16_t mode;
    uint16_t length;
    unsigned int block_idx;
    uint8_t blocks[255 + 4];
    int idx;
    uint8_t data[MAX_AAS_LEN];
} fixed_subchannel_t;

typedef struct
{
    struct input_t *input;
    uint8_t buffer[MAX_PDU_LEN];
    uint8_t pdu[MAX_PROGRAMS][MAX_STREAMS][0x10000];
    unsigned int pdu_idx[MAX_PROGRAMS][MAX_STREAMS];
    unsigned int pci;
    unsigned int program;
    uint8_t psd_buf[MAX_PROGRAMS][MAX_AAS_LEN];
    int psd_idx[MAX_PROGRAMS];

    unsigned int sync_width;
    unsigned int sync_count;
    uint8_t ccc_buf[32];
    int ccc_idx;
    fixed_subchannel_t subchannel[4];
    int fixed_ready;
    void *rs_dec;
} frame_t;

void frame_push(frame_t *st, uint8_t *bits, size_t length);
void frame_reset(frame_t *st);
void frame_set_program(frame_t *st, unsigned int program);
void frame_init(frame_t *st, struct input_t *input);
void frame_free(frame_t *st);
