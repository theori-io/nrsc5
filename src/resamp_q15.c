#include <arm_neon.h>
#include <assert.h>
#include <liquid/liquid.h>

#include "resamp_q15.h"

#define WINDOW_SIZE 2048

typedef struct {
    int32_t r, i;
} cint32_t;

static inline cint32_t cf_to_cq31(float complex x)
{
    cint32_t cq31;
    cq31.r = crealf(x) * 2147483647.0f;
    cq31.i = cimagf(x) * 2147483647.0f;
    return cq31;
}

static inline float complex cq31_to_cf(cint32_t cq31)
{
    return CMPLXF((float)cq31.r / 2147483647.0f, (float)cq31.i / 2147483647.0f);
}

typedef struct {
    // number of filters
    unsigned int nf;

    // coefficients
    int32_t * h;
    unsigned int h_len;
    unsigned int h_sub_len;

    // input window
    cint32_t * window;
    unsigned int idx;
} *firpfb_q31;

typedef struct {
    // number of filters
    unsigned int nf;

    // coefficients
    int16_t * h;
    unsigned int h_len;
    unsigned int h_sub_len;

    // input window
    cint16_t * window;
    unsigned int idx;
} *firpfb_q15;

struct resamp_q15 {
    // resampling properties/states
    float rate;         // resampling rate (ouput/input)
    float del;          // fractional delay step

    // floating-point phase
    float tau;          // accumulated timing phase, 0 <= tau < 1
    float bf;           // soft filterbank index, bf = tau*npfb = b + mu
    int b;              // base filterbank index, 0 <= b < npfb
    float mu;           // fractional filterbank interpolation value, 0 <= mu < 1
    cint32_t y0;        // filterbank output at index b
    cint32_t y1;        // filterbank output at index b+1

    // polyphase filter bank
    unsigned int npfb;
    firpfb_q31 pfb;

    enum {
        RESAMP_STATE_BOUNDARY, // boundary between input samples
        RESAMP_STATE_INTERP,   // regular interpolation
    } state;
};

firpfb_q31 firpfb_q31_create(unsigned int nf, const float *_h, unsigned int h_len)
{
    firpfb_q31 q;

    q = malloc(sizeof(*q));
    q->nf = nf;
    q->h_len = h_len;
    q->h_sub_len = h_len / nf;
    assert(q->h_sub_len * q->nf == q->h_len);
    assert(q->h_sub_len == 16);

    q->h = malloc(sizeof(int32_t) * 2 * q->h_len);
    q->window = calloc(sizeof(cint32_t), WINDOW_SIZE);
    q->idx = q->h_sub_len - 1;

    // reverse order so we can push into the window
    // duplicate for neon
    for (int i = 0; i < nf; ++i)
    {
        for (int j = 0; j < q->h_sub_len; ++j)
        {
            q->h[(i * q->h_sub_len + j) * 2] = roundf(_h[(q->h_sub_len - 1 - j) * q->nf + i] * 2147483647.0);
            q->h[(i * q->h_sub_len + j) * 2 + 1] = roundf(_h[(q->h_sub_len - 1 - j) * q->nf + i] * 2147483647.0);
        }
    }

    return q;
}

void firpfb_q31_push(firpfb_q31 q, cint32_t x)
{
    if (q->idx == WINDOW_SIZE)
    {
        for (int i = 0; i < q->h_sub_len - 1; i++)
            q->window[i] = q->window[q->idx - q->h_sub_len + i];
        q->idx = q->h_sub_len - 1;
    }
    q->window[q->idx++] = x;
}

static cint32_t dotprod_q31(cint32_t *a, int32_t *b, int n)
{
#if 0
    int64x2_t s1 = vqdmull_s32(vld1_s32((int32_t *)&a[0]), vld1_s32(&b[0*2]));
    int64x2_t s2 = vqdmull_s32(vld1_s32((int32_t *)&a[1]), vld1_s32(&b[1*2]));
    int64x2_t s3 = vqdmull_s32(vld1_s32((int32_t *)&a[2]), vld1_s32(&b[2*2]));
    int64x2_t s4 = vqdmull_s32(vld1_s32((int32_t *)&a[3]), vld1_s32(&b[3*2]));
    int64x2_t sum = vqaddq_s64(vqaddq_s64(s1, s2), vqaddq_s64(s3, s4));

    s1 = vqdmull_s32(vld1_s32((int32_t *)&a[4]), vld1_s32(&b[4*2]));
    s2 = vqdmull_s32(vld1_s32((int32_t *)&a[5]), vld1_s32(&b[5*2]));
    s3 = vqdmull_s32(vld1_s32((int32_t *)&a[6]), vld1_s32(&b[6*2]));
    s4 = vqdmull_s32(vld1_s32((int32_t *)&a[7]), vld1_s32(&b[7*2]));
    sum = vqaddq_s64(vqaddq_s64(s1, s2), sum);
    sum = vqaddq_s64(vqaddq_s64(s3, s4), sum);

    cint32_t result[2];
    vst1_s32((int32_t*)&result, vshrn_n_s64(sum, 32));

    return result[0];
#endif

    int32x4_t s1 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[0]), vld1q_s32(&b[0*2]));
    int32x4_t s2 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[2]), vld1q_s32(&b[2*2]));
    int32x4_t s3 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[4]), vld1q_s32(&b[4*2]));
    int32x4_t s4 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[6]), vld1q_s32(&b[6*2]));
    int32x4_t sum = vqaddq_s32(vqaddq_s32(s1, s2), vqaddq_s32(s3, s4));

    s1 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[8]), vld1q_s32(&b[8*2]));
    s2 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[10]), vld1q_s32(&b[10*2]));
    s3 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[12]), vld1q_s32(&b[12*2]));
    s4 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[14]), vld1q_s32(&b[14*2]));
    sum = vqaddq_s32(vqaddq_s32(s1, s2), sum);
    sum = vqaddq_s32(vqaddq_s32(s3, s4), sum);

#if 0
    s1 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[16]), vld1q_s32(&b[16*2]));
    s2 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[18]), vld1q_s32(&b[18*2]));
    s3 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[20]), vld1q_s32(&b[20*2]));
    s4 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[22]), vld1q_s32(&b[22*2]));
    sum = vqaddq_s32(vqaddq_s32(s1, s2), sum);
    sum = vqaddq_s32(vqaddq_s32(s3, s4), sum);

    s1 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[24]), vld1q_s32(&b[24*2]));
    s2 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[26]), vld1q_s32(&b[26*2]));
    s3 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[28]), vld1q_s32(&b[28*2]));
    s4 = vqrdmulhq_s32(vld1q_s32((int32_t *)&a[30]), vld1q_s32(&b[30*2]));
    sum = vqaddq_s32(vqaddq_s32(s1, s2), sum);
    sum = vqaddq_s32(vqaddq_s32(s3, s4), sum);
#endif

    cint32_t result[2];
    vst1q_s32((int32_t*)&result, sum);
    result[0].r += result[1].r;
    result[0].i += result[1].i;

    return result[0];
}

void firpfb_q31_execute(firpfb_q31 q, unsigned int f, cint32_t *y)
{
    *y = dotprod_q31(&q->window[q->idx - q->h_sub_len], &q->h[f * 2 * q->h_sub_len], q->h_sub_len);
}

firpfb_q15 firpfb_q15_create(unsigned int nf, const float *_h, unsigned int h_len)
{
    firpfb_q15 q;

    q = malloc(sizeof(*q));
    q->nf = nf;
    q->h_len = h_len;
    q->h_sub_len = h_len / nf;
    assert(q->h_sub_len * q->nf == q->h_len);
    assert(q->h_sub_len == 16);

    q->h = malloc(sizeof(int16_t) * 2 * q->h_len);
    q->window = calloc(sizeof(cint16_t), WINDOW_SIZE);
    q->idx = q->h_sub_len - 1;

    // reverse order so we can push into the window
    // duplicate for neon
    for (int i = 0; i < nf; ++i)
    {
        for (int j = 0; j < q->h_sub_len; ++j)
        {
            q->h[(i * q->h_sub_len + j) * 2] = _h[(q->h_sub_len - 1 - j) * q->nf + i] * 32767.0f;
            q->h[(i * q->h_sub_len + j) * 2 + 1] = _h[(q->h_sub_len - 1 - j) * q->nf + i] * 32767.0f;
        }
    }

    return q;
}

void firpfb_q15_push(firpfb_q15 q, cint16_t x)
{
    if (q->idx == WINDOW_SIZE)
    {
        for (int i = 0; i < q->h_sub_len - 1; i++)
            q->window[i] = q->window[q->idx - q->h_sub_len+ i];
        q->idx = q->h_sub_len - 1;
    }
    q->window[q->idx++] = x;
}

static cint16_t dotprod_q15(cint16_t *a, int16_t *b, int n)
{
    int16x8_t s1 = vqrdmulhq_s16(vld1q_s16((int16_t *)&a[0]), vld1q_s16(&b[0*2]));
    int16x8_t s2 = vqrdmulhq_s16(vld1q_s16((int16_t *)&a[4]), vld1q_s16(&b[4*2]));
    int16x8_t s3 = vqrdmulhq_s16(vld1q_s16((int16_t *)&a[8]), vld1q_s16(&b[8*2]));
    int16x8_t s4 = vqrdmulhq_s16(vld1q_s16((int16_t *)&a[12]), vld1q_s16(&b[12*2]));
    int16x8_t sum = vqaddq_s16(vqaddq_s16(s1, s2), vqaddq_s16(s3, s4));

#if 0
    s1 = vqrdmulhq_s16(vld1q_s16((int16_t *)&a[16]), vld1q_s16(&b[16*2]));
    s2 = vqrdmulhq_s16(vld1q_s16((int16_t *)&a[20]), vld1q_s16(&b[20*2]));
    s3 = vqrdmulhq_s16(vld1q_s16((int16_t *)&a[24]), vld1q_s16(&b[24*2]));
    s4 = vqrdmulhq_s16(vld1q_s16((int16_t *)&a[28]), vld1q_s16(&b[28*2]));
    sum = vqaddq_s16(vqaddq_s16(s1, s2), sum);
    sum = vqaddq_s16(vqaddq_s16(s3, s4), sum);
#endif

    int16x4x2_t sum2 = vuzp_s16(vget_high_s16(sum), vget_low_s16(sum));
    int16x4_t sum3 = vpadd_s16(sum2.val[0], sum2.val[1]);
    sum3 = vpadd_s16(sum3, sum3);

    cint16_t result[2];
    vst1_s16((int16_t*)&result, sum3);

    return result[0];
}

void firpfb_q15_execute(firpfb_q15 q, unsigned int f, cint16_t *y)
{
    *y = dotprod_q15(&q->window[q->idx - q->h_sub_len], &q->h[f * 2 * q->h_sub_len], q->h_sub_len);
}

resamp_q15 resamp_q15_create(unsigned int m, float fc, float As, unsigned npfb)
{
    resamp_q15 q;

    q = malloc(sizeof(*q));
    q->rate = 1.0;
    q->del = 1.0;
    q->tau = 0;
    q->b = 0;
    q->mu = 0;
    q->npfb = npfb;
    q->state = RESAMP_STATE_INTERP;

    // design filter
    unsigned int n = 2 * m * npfb + 1;
    float hf[n];
    liquid_firdes_kaiser(n, fc / npfb, As, 0.0f, hf);

    // normalize filter coefficients by DC gain
    float gain=0.0f;
    for (unsigned int i = 0; i < n; ++i)
        gain += hf[i];
    gain = q->npfb / gain;

    // apply gain
    for (unsigned int i = 0; i < n; ++i)
        hf[i] *= gain;

    // construct pfb
    q->pfb = firpfb_q31_create(q->npfb, hf, n - 1);

    return q;
}

void resamp_q15_set_rate(resamp_q15 q, float rate)
{
    // set internal rate
    q->rate = rate;

    // set output stride
    q->del = 1.0f / rate;
}

static void update_timing_state(resamp_q15 q)
{
    // update high-resolution timing phase
    q->tau += q->del;

    // convert to high-resolution filterbank index
    q->bf  = q->tau * (float)(q->npfb);

    // split into integer filterbank index and fractional interpolation
    q->b   = (int)floorf(q->bf);      // base index
    q->mu  = q->bf - (float)(q->b);   // fractional index
}

void resamp_q15_execute(resamp_q15 q, const cint16_t * _x, float complex * y, unsigned int * pn)
{
    cint32_t x;
    x.r = (int32_t)_x->r << 16;
    x.i = (int32_t)_x->i << 16;
    // push new sample
    firpfb_q31_push(q->pfb, x);

    // number of output samples
    unsigned int n = 0;

    while (q->b < q->npfb)
    {
        if (q->state == RESAMP_STATE_INTERP)
        {
            // compute output at base index
            firpfb_q31_execute(q->pfb, q->b, &q->y0);

            // check to see if base index is last filter in the bank
            if (q->b == q->npfb - 1)
            {
                q->state = RESAMP_STATE_BOUNDARY;
                q->b = q->npfb;
            }
            else
            {
                // compute output at incremented base index
                firpfb_q31_execute(q->pfb, q->b + 1, &q->y1);

                // linear interpolation
                y[n++] = (1.0f - q->mu)*cq31_to_cf(q->y0) + q->mu*cq31_to_cf(q->y1);

                update_timing_state(q);
            }
        }
        else
        {
            firpfb_q31_execute(q->pfb, 0, &q->y1);
            y[n++] = (1.0f - q->mu)*cq31_to_cf(q->y0) + q->mu*cq31_to_cf(q->y1);
            update_timing_state(q);
            q->state = RESAMP_STATE_INTERP;
        }
    }

    // decrement timing phase by one sample
    q->tau -= 1.0f;
    q->bf -= q->npfb;
    q->b -= q->npfb;

    *pn = n;
}
