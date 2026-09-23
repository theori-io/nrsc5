#pragma once

#include "defines.h"

typedef struct firdecim_cf32 * firdecim_cf32;

firdecim_cf32 firdecim_cf32_create(const float * taps);
void firdecim_cf32_free(firdecim_cf32);
void firdecim_cf32_reset(firdecim_cf32);
void fir_cf32_execute(firdecim_cf32 q, const float complex *x, float complex *y);
