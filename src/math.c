#include "defines.h"

#ifdef USE_FAST_MATH
float complex fast_cexpf(float x)
{
    static int init = 0;
    static float complex tbl[4096];

    if (!init)
    {
        for (int i = 0; i < 4096; ++i)
            tbl[i] = cexpf(I * 2 * M_PI * i / 4096);
        init = 1;
    }

    return tbl[(int)truncf(x / (2 * M_PI / 4096)) & 4095];
}
#else
float complex fast_cexpf(float x)
{
    return cexpf(I * x);
}
#endif
