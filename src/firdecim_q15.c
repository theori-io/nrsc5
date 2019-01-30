#include "config.h"

#include <assert.h>
#include <stdint.h>

#ifdef HAVE_NEON
#include <arm_neon.h>
#endif

#ifdef HAVE_SSE2
#include <emmintrin.h>
#endif

#include "firdecim_q15.h"

#define WINDOW_SIZE 2048

struct firdecim_q15 {
    int16_t * taps;
    unsigned int ntaps;
    cint16_t * window;
    unsigned int idx;
};

firdecim_q15 firdecim_q15_create(const float * taps, unsigned int ntaps)
{
    firdecim_q15 q;

    q = malloc(sizeof(*q));
    q->ntaps = (ntaps == 32) ? 32 : 15;
    q->taps = malloc(sizeof(int16_t) * ntaps * 2);
    q->window = calloc(sizeof(cint16_t), WINDOW_SIZE);
    firdecim_q15_reset(q);

    // reverse order so we can push into the window
    // duplicate for neon
    for (unsigned int i = 0; i < ntaps; ++i)
    {
        q->taps[i*2] = taps[ntaps - 1 - i] * 32767.0f;
        q->taps[i*2+1] = taps[ntaps - 1 - i] * 32767.0f;
    }

    return q;
}

void firdecim_q15_free(firdecim_q15 q)
{
    free(q->taps);
    free(q->window);
    free(q);
}

void firdecim_q15_reset(firdecim_q15 q)
{
    q->idx = q->ntaps - 1;
}

static void push(firdecim_q15 q, cint16_t x)
{
    if (q->idx == WINDOW_SIZE)
    {
        for (unsigned int i = 0; i < q->ntaps - 1; i++)
            q->window[i] = q->window[q->idx - q->ntaps + 1 + i];
        q->idx = q->ntaps - 1;
    }
    q->window[q->idx++] = x;
}

#ifdef HAVE_NEON
static cint16_t dotprod_32(cint16_t *a, int16_t *b)
{
    int16x8_t s1 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[0]), vld1q_s16(&b[0*2]));
    int16x8_t s2 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[4]), vld1q_s16(&b[4*2]));
    int16x8_t s3 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[8]), vld1q_s16(&b[8*2]));
    int16x8_t s4 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[12]), vld1q_s16(&b[12*2]));
    int16x8_t sum = vqaddq_s16(vqaddq_s16(s1, s2), vqaddq_s16(s3, s4));

    s1 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[16]), vld1q_s16(&b[16*2]));
    s2 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[20]), vld1q_s16(&b[20*2]));
    s3 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[24]), vld1q_s16(&b[24*2]));
    s4 = vqdmulhq_s16(vld1q_s16((int16_t *)&a[28]), vld1q_s16(&b[28*2]));
    sum = vqaddq_s16(vqaddq_s16(s1, s2), sum);
    sum = vqaddq_s16(vqaddq_s16(s3, s4), sum);

    int16x4x2_t sum2 = vuzp_s16(vget_high_s16(sum), vget_low_s16(sum));
    int16x4_t sum3 = vpadd_s16(sum2.val[0], sum2.val[1]);
    sum3 = vpadd_s16(sum3, sum3);

    cint16_t result[2];
    vst1_s16((int16_t*)&result, sum3);

    return result[0];
}
#else
static cint16_t dotprod_32(cint16_t *a, int16_t *b)
{
    cint16_t sum = { 0 };
    int i;

    for (i = 1; i < 16; i++)
    {
        sum.r += ((a[i].r + a[32-i].r) * b[i * 2]) >> 15;
        sum.i += ((a[i].i + a[32-i].i) * b[i * 2]) >> 15;
    }
    sum.r += (a[i].r * b[i * 2]) >> 15;
    sum.i += (a[i].i * b[i * 2]) >> 15;

    return sum;
}
#endif

#ifdef HAVE_NEON
static cint16_t dotprod_halfband_4(cint16_t *a, int16_t *b)
{
    cint16_t pairs[4];
    int i;

    for (i = 0; i < 7; i += 2)
    {
        pairs[i/2].r = a[i].r + a[14-i].r;
        pairs[i/2].i = a[i].i + a[14-i].i;
    }

    int16x8_t prod = vqdmulhq_s16(vld1q_s16((int16_t *)pairs), vld1q_s16(b));
    int16x4x2_t prod2 = vuzp_s16(vget_high_s16(prod), vget_low_s16(prod));
    int16x4_t sum = vpadd_s16(prod2.val[0], prod2.val[1]);
    sum = vpadd_s16(sum, sum);

    cint16_t result[2];
    vst1_s16((int16_t*)&result, sum);

    result[0].r += a[7].r;
    result[0].i += a[7].i;
    return result[0];
}
#else
static cint16_t dotprod_halfband_4(cint16_t *a, int16_t *b)
{
    cint16_t sum = { 0 };
    int i;

    for (i = 0; i < 7; i += 2)
    {
        sum.r += ((a[i].r + a[14-i].r) * b[i]) >> 15;
        sum.i += ((a[i].i + a[14-i].i) * b[i]) >> 15;
    }
    sum.r += a[7].r;
    sum.i += a[7].i;

    return sum;
}
#endif

void fir_q15_execute(firdecim_q15 q, const cint16_t *x, cint16_t *y)
{
    push(q, x[0]);
    *y = dotprod_32(&q->window[q->idx - q->ntaps], q->taps);
}

void halfband_q15_execute(firdecim_q15 q, const cint16_t *x, cint16_t *y)
{
    push(q, x[0]);
    *y = dotprod_halfband_4(&q->window[q->idx - q->ntaps], q->taps);
    push(q, x[1]);
}
