/*
 * Viterbi decoder for convolutional codes - ARM NEON
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Andrew Wesie <awesie@gmail.com>
 *
 * Based on conv_sse.h written by Tom Tsou <tom.tsou@ettus.com>.
 */

#include <stdint.h>
#include <arm_neon.h>

#define NEON_DEINTERLEAVE_K7(M0,M1,M2,M3,M4,M5,M6,M7, \
                M8,M9,M10,M11,M12,M13,M14,M15) \
{ \
    int16x8x2_t tmp; \
    tmp = vuzpq_s16(M0, M1); \
    M8 = tmp.val[0]; M9 = tmp.val[1]; \
    tmp = vuzpq_s16(M2, M3); \
    M10 = tmp.val[0]; M11 = tmp.val[1]; \
    tmp = vuzpq_s16(M4, M5); \
    M12 = tmp.val[0]; M13 = tmp.val[1]; \
    tmp = vuzpq_s16(M6, M7); \
    M14 = tmp.val[0]; M15 = tmp.val[1]; \
}
#define NEON_BRANCH_METRIC_N4(M0,M1,M2,M3,M4,M5) \
{ \
    M0 = vmulq_s16(M4, M0); \
    M1 = vmulq_s16(M4, M1); \
    M2 = vmulq_s16(M4, M2); \
    M3 = vmulq_s16(M4, M3); \
    int16x4_t t1 = vpadd_s16(vpadd_s16(vget_low_s16(M0), vget_high_s16(M0)), vpadd_s16(vget_low_s16(M1), vget_high_s16(M1))); \
    int16x4_t t2 = vpadd_s16(vpadd_s16(vget_low_s16(M2), vget_high_s16(M2)), vpadd_s16(vget_low_s16(M3), vget_high_s16(M3))); \
    M5 = vcombine_s16(t1, t2); \
}
#define NEON_BUTTERFLY(M0,M1,M2,M3,M4) \
{ \
    M3 = vqaddq_s16(M0, M2); \
    M4 = vqsubq_s16(M1, M2); \
    M0 = vqsubq_s16(M0, M2); \
    M1 = vqaddq_s16(M1, M2); \
    M2 = vmaxq_s16(M3, M4); \
    M3 = vreinterpretq_s16_u16(vcgtq_s16(M3, M4)); \
    M4 = vmaxq_s16(M0, M1); \
    M1 = vreinterpretq_s16_u16(vcgtq_s16(M0, M1)); \
}
#define NEON_NORMALIZE_K7(M0,M1,M2,M3,M4,M5,M6,M7,M8,M9,M10,M11) \
{ \
    M8 = vminq_s16(M0, M1); \
    M9 = vminq_s16(M2, M3); \
    M10 = vminq_s16(M4, M5); \
    M11 = vminq_s16(M6, M7); \
    M8 = vminq_s16(M8, M9); \
    M10 = vminq_s16(M10, M11); \
    M8 = vminq_s16(M8, M10); \
    int16x4_t t = vpmin_s16(vget_low_s16(M8), vget_high_s16(M8)); \
    t = vpmin_s16(t, t); \
    t = vpmin_s16(t, t); \
    M8 = vdupq_lane_s16(t, 0); \
    M0 = vqsubq_s16(M0, M8); \
    M1 = vqsubq_s16(M1, M8); \
    M2 = vqsubq_s16(M2, M8); \
    M3 = vqsubq_s16(M3, M8); \
    M4 = vqsubq_s16(M4, M8); \
    M5 = vqsubq_s16(M5, M8); \
    M6 = vqsubq_s16(M6, M8); \
    M7 = vqsubq_s16(M7, M8); \
}
__always_inline static void _neon_metrics_k7_n4(const int16_t *val, const int16_t *out,
					int16_t *sums, int16_t *paths, int norm)
{
    int16x8_t m0, m1, m2, m3, m4, m5, m6, m7;
    int16x8_t m8, m9, m10, m11, m12, m13, m14, m15;
    int16x4_t input;

	/* (PMU) Load accumulated path matrics */
    m0 = vld1q_s16(&sums[0]);
    m1 = vld1q_s16(&sums[8]);
    m2 = vld1q_s16(&sums[16]);
    m3 = vld1q_s16(&sums[24]);
    m4 = vld1q_s16(&sums[32]);
    m5 = vld1q_s16(&sums[40]);
    m6 = vld1q_s16(&sums[48]);
    m7 = vld1q_s16(&sums[56]);

	/* (PMU) Deinterleave into even and odd packed registers */
	NEON_DEINTERLEAVE_K7(m0, m1, m2, m3 ,m4 ,m5, m6, m7,
			    m8, m9, m10, m11, m12, m13, m14, m15)

	/* (BMU) Load and expand 8-bit input out to 16-bits */
    input = vld1_s16(val);
    m7 = vcombine_s16(input, input);

	/* (BMU) Load and compute branch metrics */
    m0 = vld1q_s16(&out[0]);
    m1 = vld1q_s16(&out[8]);
    m2 = vld1q_s16(&out[16]);
    m3 = vld1q_s16(&out[24]);

	NEON_BRANCH_METRIC_N4(m0, m1, m2, m3, m7, m4)

    m0 = vld1q_s16(&out[32]);
    m1 = vld1q_s16(&out[40]);
    m2 = vld1q_s16(&out[48]);
    m3 = vld1q_s16(&out[56]);

	NEON_BRANCH_METRIC_N4(m0, m1, m2, m3, m7, m5)

    m0 = vld1q_s16(&out[64]);
    m1 = vld1q_s16(&out[72]);
    m2 = vld1q_s16(&out[80]);
    m3 = vld1q_s16(&out[88]);

	NEON_BRANCH_METRIC_N4(m0, m1, m2, m3, m7, m6)

    m0 = vld1q_s16(&out[96]);
    m1 = vld1q_s16(&out[104]);
    m2 = vld1q_s16(&out[112]);
    m3 = vld1q_s16(&out[120]);

	NEON_BRANCH_METRIC_N4(m0, m1, m2, m3, m7, m7)

	/* (PMU) Butterflies: 0-15 */
	NEON_BUTTERFLY(m8, m9, m4, m0, m1)
	NEON_BUTTERFLY(m10, m11, m5, m2, m3)

    vst1q_s16(&paths[0], m0);
    vst1q_s16(&paths[8], m2);
    vst1q_s16(&paths[32], m9);
    vst1q_s16(&paths[40], m11);

	/* (PMU) Butterflies: 17-31 */
	NEON_BUTTERFLY(m12, m13, m6, m0, m2)
	NEON_BUTTERFLY(m14, m15, m7, m9, m11)

    vst1q_s16(&paths[16], m0);
    vst1q_s16(&paths[24], m9);
    vst1q_s16(&paths[48], m13);
    vst1q_s16(&paths[56], m15);

	if (norm)
		NEON_NORMALIZE_K7(m4, m1, m5, m3, m6, m2,
				 m7, m11, m0, m8, m9, m10)

    vst1q_s16(&sums[0], m4);
    vst1q_s16(&sums[8], m5);
    vst1q_s16(&sums[16], m6);
    vst1q_s16(&sums[24], m7);
    vst1q_s16(&sums[32], m1);
    vst1q_s16(&sums[40], m3);
    vst1q_s16(&sums[48], m2);
    vst1q_s16(&sums[56], m11);
}

static inline void gen_metrics_k7_n3(const int8_t *val, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm)
{
	const int16_t _val[4] = { val[0], val[1], val[2], 0 };

	_neon_metrics_k7_n4(_val, out, sums, paths, norm);
}
