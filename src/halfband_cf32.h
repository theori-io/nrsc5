#pragma once

#include "defines.h"

typedef struct halfband_cf32 * halfband_cf32;

halfband_cf32 halfband_cf32_create();
void halfband_cf32_free(halfband_cf32);
void halfband_cf32_reset(halfband_cf32);
void halfband_cf32_execute(halfband_cf32 q, const float complex *x, float complex *y);
