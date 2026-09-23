#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "firdecim_cf32.h"

#define WINDOW_SIZE 2048
#define FIRDECIM_NTAPS 32

struct firdecim_cf32 {
    unsigned int idx;
    float * taps;
    float complex * window;
};

firdecim_cf32 firdecim_cf32_create(const float taps[FIRDECIM_NTAPS])
{
    firdecim_cf32 q;

    q = malloc(sizeof(*q));
    q->taps = malloc(sizeof(float) * FIRDECIM_NTAPS);
    q->window = calloc(WINDOW_SIZE, sizeof(float complex));
    firdecim_cf32_reset(q);

    // reverse order so we can push into the window
    for (unsigned int i = 0; i < FIRDECIM_NTAPS; ++i)
    {
        q->taps[i] = taps[FIRDECIM_NTAPS - 1 - i];
    }

    return q;
}

void firdecim_cf32_free(firdecim_cf32 q)
{
    free(q->taps);
    free(q->window);
    free(q);
}

void firdecim_cf32_reset(firdecim_cf32 q)
{
    q->idx = FIRDECIM_NTAPS - 1;
}

static void firdecim_cf32_push(firdecim_cf32 q, complex float x)
{
    if (q->idx == WINDOW_SIZE)
    {
        unsigned int keep = FIRDECIM_NTAPS - 1;
        memmove(q->window, &q->window[q->idx - keep], keep * sizeof(float complex));
        q->idx = keep;
    }
    q->window[q->idx++] = x;
}

static float complex dotprod_32(const float complex *restrict a, const float *restrict b)
{
    // compute each independently to avoid processor dependencies; sum after
    float complex sum0 = (a[1] + a[31]) * b[1];
    float complex sum1 = (a[2] + a[30]) * b[2];
    float complex sum2 = (a[3] + a[29]) * b[3];
    float complex sum3 = (a[4] + a[28]) * b[4];
    float complex sum4 = (a[5] + a[27]) * b[5];
    float complex sum5 = (a[6] + a[26]) * b[6];
    float complex sum6 = (a[7] + a[25]) * b[7];
    float complex sum7 = (a[8] + a[24]) * b[8];
    float complex sum8 = (a[9] + a[23]) * b[9];
    float complex sum9 = (a[10] + a[22]) * b[10];
    float complex sum10 = (a[11] + a[21]) * b[11];
    float complex sum11 = (a[12] + a[20]) * b[12];
    float complex sum12 = (a[13] + a[19]) * b[13];
    float complex sum13 = (a[14] + a[18]) * b[14];
    float complex sum14 = (a[15] + a[17]) * b[15];
    float complex sum15 = a[16] * b[16];

    return (((sum0 + sum1) + (sum2 + sum3)) + ((sum4 + sum5) + (sum6 + sum7))) +
        (((sum8 + sum9) + (sum10 + sum11)) + ((sum12 + sum13) + (sum14 + sum15)));
}

void fir_cf32_execute(firdecim_cf32 q, const float complex *restrict x, float complex *restrict y)
{
    firdecim_cf32_push(q, x[0]);
    *y = dotprod_32(&q->window[q->idx - FIRDECIM_NTAPS], q->taps);
}
