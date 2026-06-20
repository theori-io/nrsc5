#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "firdecim_cf32.h"

#define WINDOW_SIZE 2048

struct firdecim_cf32
{
    float *taps;
    unsigned int ntaps;
    float complex *window;
    unsigned int idx;
};

firdecim_cf32 firdecim_cf32_create(const float *taps, unsigned int ntaps)
{
    firdecim_cf32 filter;

    filter = malloc(sizeof(*filter));
    filter->ntaps = (ntaps == 32) ? 32 : 15;
    filter->taps = malloc(sizeof(float) * filter->ntaps);
    filter->window = calloc(WINDOW_SIZE, sizeof(float complex));
    firdecim_cf32_reset(filter);

    // reverse order so we can push into the window
    if (ntaps == 32)
    {
        for (unsigned int i = 0; i < filter->ntaps; ++i)
        {
            filter->taps[i] = taps[filter->ntaps - 1 - i];
        }
    }
    else
    {
        // Halfband filter: input is 4 non-zero coefficients.
        // Map them to the even indices 0, 2, 4, 6 for dotprod_halfband_15
        for (unsigned int i = 0; i < ntaps; ++i)
        {
            filter->taps[i * 2] = taps[ntaps - 1 - i];
        }
    }

    return filter;
}

void firdecim_cf32_free(firdecim_cf32 filter)
{
    free(filter->taps);
    free(filter->window);
    free(filter);
}

void firdecim_cf32_reset(firdecim_cf32 filter)
{
    filter->idx = filter->ntaps - 1;
}

static void push(firdecim_cf32 filter, float complex x)
{
    if (filter->idx == WINDOW_SIZE)
    {
        memmove(filter->window, &filter->window[filter->idx - filter->ntaps + 1], (filter->ntaps - 1) * sizeof(float complex));
        filter->idx = filter->ntaps - 1;
    }
    filter->window[filter->idx++] = x;
}

static float complex dotprod_32(const float complex *a, const float *b)
{
    float complex sum = 0;
    int i;

    for (i = 1; i < 16; i++)
    {
        sum += (a[i] + a[32-i]) * b[i];
    }
    sum += a[16] * b[16];

    return sum;
}

static float complex dotprod_halfband_15(const float complex *a, const float *b)
{
    float complex sum = a[7]; // Center tap (coefficient is implicitly 1.0)
    int i;

    for (i = 0; i < 7; i += 2)
    {
        sum += (a[i] + a[14-i]) * b[i];
    }

    return sum;
}

void fir_cf32_execute(firdecim_cf32 filter, const float complex *x, float complex *y)
{
    push(filter, x[0]);
    *y = dotprod_32(&filter->window[filter->idx - filter->ntaps], filter->taps);
}

void halfband_cf32_execute(firdecim_cf32 filter, const float complex *x, float complex *y)
{
    push(filter, x[0]);
    *y = dotprod_halfband_15(&filter->window[filter->idx - filter->ntaps], filter->taps);
    push(filter, x[1]);
}
