#include <assert.h>
#include <stdint.h>

#ifdef HAVE_NEON
#include <arm_neon.h>
#endif

#include "firdecim_q15.h"

#define WINDOW_SIZE 2048

struct firdecim_q15 {
    unsigned int decim;
    int16_t * taps;
    unsigned int ntaps;
    cint16_t * window;
    unsigned int idx;
};

firdecim_q15 firdecim_q15_create(unsigned int decim, const float * taps, unsigned int ntaps)
{
    firdecim_q15 q;

    q = malloc(sizeof(*q));
    q->decim = decim;
    q->ntaps = ntaps;
    q->taps = malloc(sizeof(int16_t) * ntaps * 2);
    q->window = calloc(sizeof(cint16_t), WINDOW_SIZE);
    q->idx = ntaps - 1;

    assert(decim == 2);
    assert(ntaps == 32);

    // reverse order so we can push into the window
    // duplicate for neon
    for (int i = 0; i < ntaps; ++i)
    {
        q->taps[i*2] = taps[ntaps - 1 - i] * 32767.0f;
        q->taps[i*2+1] = taps[ntaps - 1 - i] * 32767.0f;
    }

    return q;
}

static void push(firdecim_q15 q, cint16_t x)
{
    if (q->idx == WINDOW_SIZE)
    {
        for (int i = 0; i < 31; i++)
            q->window[i] = q->window[q->idx - 32 + i];
        q->idx = q->ntaps - 1;
    }
    q->window[q->idx++] = x;
}

#ifdef HAVE_NEON
static cint16_t dotprod(cint16_t *a, int16_t *b, int n)
{
    int16x8_t s1 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[0]), vld1q_s16(&b[0*2]));
    int16x8_t s2 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[4]), vld1q_s16(&b[4*2]));
    int16x8_t s3 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[8]), vld1q_s16(&b[8*2]));
    int16x8_t s4 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[12]), vld1q_s16(&b[12*2]));
    int16x8_t suma = vqaddq_s16(vqaddq_s16(s1, s2), vqaddq_s16(s3, s4));

    s1 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[16]), vld1q_s16(&b[16*2]));
    s2 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[20]), vld1q_s16(&b[20*2]));
    s3 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[24]), vld1q_s16(&b[24*2]));
    s4 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[28]), vld1q_s16(&b[28*2]));
    int16x8_t sumb = vqaddq_s16(vqaddq_s16(s1, s2), vqaddq_s16(s3, s4));

    int16x8_t sum = vqaddq_s16(suma, sumb);
    int16x4x2_t sum2 = vuzp_s16(vget_high_s16(sum), vget_low_s16(sum));
    int16x4_t sum3 = vpadd_s16(sum2.val[0], sum2.val[1]);
    sum3 = vpadd_s16(sum3, sum3);

    cint16_t result[2];
    vst1_s16((int16_t*)&result, sum3);

    return result[0];
}
#else
static cint16_t dotprod(cint16_t *a, int16_t *b, int n)
{
    cint16_t sum = { 0 };
    for (int i = 0; i < n; ++i)
    {
        sum.r += (a[i].r * b[i * 2]) >> 15;
        sum.i += (a[i].i * b[i * 2]) >> 15;
    }
    return sum;
}
#endif

void firdecim_q15_execute(firdecim_q15 q, const cint16_t *x, cint16_t *y)
{
    push(q, x[0]);
    *y = dotprod(&q->window[q->idx - 32], q->taps, 32);
    push(q, x[1]);
}
