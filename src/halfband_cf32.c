#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "halfband_cf32.h"

#define WINDOW_SIZE 2048
#define HALFBAND_NTAPS 15

struct halfband_cf32 {
    unsigned int idx;
    float complex * window;
};

halfband_cf32 halfband_cf32_create()
{
    halfband_cf32 q;

    q = malloc(sizeof(*q));
    q->window = calloc(WINDOW_SIZE, sizeof(float complex));
    halfband_cf32_reset(q);
    return q;
}

void halfband_cf32_free(halfband_cf32 q)
{
    free(q->window);
    free(q);
}

void halfband_cf32_reset(halfband_cf32 q)
{
    q->idx = HALFBAND_NTAPS - 1;
}

static void halfband_cf32_push(halfband_cf32 q, complex float x)
{
    if (q->idx == WINDOW_SIZE)
    {
        unsigned int keep = HALFBAND_NTAPS - 1;
        memmove(q->window, &q->window[q->idx - keep], keep * sizeof(float complex));
        q->idx = keep;
    }
    q->window[q->idx++] = x;
}

/*
 * GNU Radio Filter Design Tool
 * FIR, Low Pass, Kaiser Window
 * Sample rate: 1488375
 * End of pass band: 372094
 * Start of stop band: 530000
 * Stop band attenuation: 40
 */
static float complex dotprod_halfband_4(const float complex *restrict a)
{
    // compute each independently to avoid processor dependencies; sum after
    float complex sum0 = (a[0] + a[14]) * -0.00410953676328063f;
    float complex sum2 = (a[2] + a[12]) * 0.032919470220804214f;
    float complex sum4 = (a[4] + a[10]) * -0.13481467962265015f;
    float complex sum6 = (a[6] + a[8]) * 0.6062333583831787f;

    return (sum0 + sum2) + (sum4 + sum6) + a[7];
}

void halfband_cf32_execute(halfband_cf32 q, const float complex *restrict x, float complex *restrict y)
{
    halfband_cf32_push(q, x[0]);
    *y = dotprod_halfband_4(&q->window[q->idx - HALFBAND_NTAPS]);
    halfband_cf32_push(q, x[1]);
}
