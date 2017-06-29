#pragma once

#include "defines.h"

typedef struct resamp_q15 * resamp_q15;

resamp_q15 resamp_q15_create(unsigned int m, float fc, float As, unsigned npfb);
void resamp_q15_set_rate(resamp_q15 q, float rate);
void resamp_q15_execute(resamp_q15 q, const cint16_t * x, float complex * y, unsigned int * pn);
