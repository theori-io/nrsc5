#pragma once

#include <stdint.h>

typedef struct
{
    char country_code[3];
    int fcc_facility_id;
    char short_name[8];
} pids_t;

void pids_frame_push(pids_t *st, uint8_t *bits);
void pids_init(pids_t *st);
