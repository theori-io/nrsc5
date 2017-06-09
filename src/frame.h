#pragma once

#include <stdint.h>

typedef struct
{
    struct input_t *input;
    uint8_t *buffer;
    uint8_t *pdu;
    unsigned int pdu_idx;
    unsigned int pci;
    int ready;
    unsigned int program;
} frame_t;

void frame_push(frame_t *st, uint8_t *bits);
void frame_reset(frame_t *st);
void frame_set_program(frame_t *st, unsigned int program);
void frame_init(frame_t *st, struct input_t *input);
