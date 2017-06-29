#include "defines.h"

#ifdef USE_FAST_MATH
float complex cexpf_tbl[4096];

void math_init()
{
    for (int i = 0; i < 4096; ++i)
        cexpf_tbl[i] = cexpf(I * 2 * M_PI * i / 4096);
}
#else
void math_init()
{
}
#endif
