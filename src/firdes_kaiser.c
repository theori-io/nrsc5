/*
 * From the liquid-dsp project. Original copyright is below.
 *
 * Copyright (c) 2007 - 2015 Joseph Gaeddert
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <math.h>
#include "firdes.h"

static float lngammaf(float _z)
{
    float g;
    if (_z < 10.0f) {
        // Use recursive formula:
        //    gamma(z+1) = z * gamma(z)
        // therefore:
        //    log(Gamma(z)) = log(gamma(z+1)) - ln(z)
        return lngammaf(_z + 1.0f) - logf(_z);
    } else {
        // high value approximation
        g = 0.5*( logf(2*M_PI)-log(_z) );
        g += _z*( logf(_z+(1/(12.0f*_z-0.1f/_z)))-1);
    }
    return g;
}

// I_0(z) : Modified bessel function of the first kind (order zero)
#define NUM_BESSELI0_ITERATIONS 32
static float besseli0f(float _z)
{
    // TODO : use better low-signal approximation
    if (_z == 0.0f)
        return 1.0f;

    unsigned int k;
    float t, y=0.0f;
    for (k=0; k<NUM_BESSELI0_ITERATIONS; k++) {
        t = k * logf(0.5f*_z) - lngammaf((float)k + 1.0f);
        y += expf(2*t);
    }
    return y;
}

// compute sinc(x) = sin(pi*x) / (pi*x)
static float sincf(float _x)
{
    // _x ~ 0 approximation
    // from : http://mathworld.wolfram.com/SincFunction.html
    // sinc(z) = \prod_{k=1}^{\infty} { cos(\pi z / 2^k) }
    if (fabsf(_x) < 0.01f)
        return cosf(M_PI*_x/2.0f)*cosf(M_PI*_x/4.0f)*cosf(M_PI*_x/8.0f);

    return sinf(M_PI*_x)/(M_PI*_x);
}

static float kaiser(unsigned int _n,
             unsigned int _N,
             float        _beta,
             float        _mu)
{
    float t = (float)_n - (float)(_N-1)/2 + _mu;
    float r = 2.0f*t/(float)(_N);
    float a = besseli0f(_beta*sqrtf(1-r*r));
    float b = besseli0f(_beta);
    return a / b;
}

static float kaiser_beta_As(float _As)
{
    _As = fabsf(_As);
    float beta;
    if (_As > 50.0f)
        beta = 0.1102f*(_As - 8.7f);
    else if (_As > 21.0f)
        beta = 0.5842*powf(_As - 21, 0.4f) + 0.07886f*(_As - 21);
    else
        beta = 0.0f;

    return beta;
}

void firdes_kaiser(unsigned int _n,
                          float _fc,
                          float _As,
                          float _mu,
                          float *_h)
{
    // choose kaiser beta parameter (approximate)
    float beta = kaiser_beta_As(_As);

    float t, h1, h2;
    unsigned int i;
    for (i=0; i<_n; i++) {
        t = (float)i - (float)(_n-1)/2 + _mu;

        // sinc prototype
        h1 = sincf(2.0f*_fc*t);

        // kaiser window
        h2 = kaiser(i,_n,beta,_mu);

        //printf("t = %f, h1 = %f, h2 = %f\n", t, h1, h2);

        // composite
        _h[i] = h1*h2;
    }
}
