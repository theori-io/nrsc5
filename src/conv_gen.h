/*
 * Viterbi decoder for convolutional codes
 *
 * Copyright (C) 2015 Ettus Research LLC
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
 * Author: Tom Tsou <tom.tsou@ettus.com>
 */

#include <stdint.h>
#include <string.h>

/*
 * Add-Compare-Select (ACS-Butterfly)
 *
 * Compute 4 accumulated path metrics and 4 path selections. Note that path
 * selections are store as -1 and 0 rather than 0 and 1. This is to match
 * the output format of the sse packed compare instruction 'pmaxuw'.
 */
static void acs_butterfly(int state, int num_states,
			  int16_t metric, int16_t *sum,
			  int16_t *new_sum, int16_t *path)
{
	int state0, state1;
	int sum0, sum1, sum2, sum3;

	state0 = *(sum + (2 * state + 0));
	state1 = *(sum + (2 * state + 1));

	sum0 = state0 + metric;
	sum1 = state1 - metric;
	sum2 = state0 - metric;
	sum3 = state1 + metric;

	if (sum0 > sum1) {
		*new_sum = sum0;
		*path = -1;
	} else {
		*new_sum = sum1;
		*path = 0;
	}

	if (sum2 > sum3) {
		*(new_sum + num_states / 2) = sum2;
		*(path + num_states / 2) = -1;
	} else {
		*(new_sum + num_states / 2) = sum3;
		*(path + num_states / 2) = 0;
	}
}

/* Branch metrics unit N=3 */
static void _gen_branch_metrics_n3(int num_states, const int8_t *seq,
			    const int16_t *out, int16_t *metrics)
{
	int i;

	for (i = 0; i < num_states / 2; i++)
		metrics[i] = seq[0] * out[4 * i + 0] +
			     seq[1] * out[4 * i + 1] +
			     seq[2] * out[4 * i + 2];
}

/* Path metric unit */
static void _gen_path_metrics(int num_states, int16_t *sums,
		       int16_t *metrics, int16_t *paths, int norm)
{
	int i;
	int16_t min;
	int16_t new_sums[num_states];

	for (i = 0; i < num_states / 2; i++) {
		acs_butterfly(i, num_states, metrics[i],
			      sums, &new_sums[i], &paths[i]);
	}

	if (norm) {
		min = new_sums[0];
		for (i = 1; i < num_states; i++) {
			if (new_sums[i] < min)
				min = new_sums[i];
		}

		for (i = 0; i < num_states; i++)
			new_sums[i] -= min;
	}

	memcpy(sums, new_sums, num_states * sizeof(int16_t));
}

#if !defined(HAVE_SSE3) && !defined(HAVE_NEON)
static void gen_metrics_k7_n3(const int8_t *seq, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm)
{
	int16_t metrics[32];

	_gen_branch_metrics_n3(64, seq, out, metrics);
	_gen_path_metrics(64, sums, metrics, paths, norm);

}
#endif

static void gen_metrics_k9_n3(const int8_t *seq, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm)
{
	int16_t metrics[128];

	_gen_branch_metrics_n3(256, seq, out, metrics);
	_gen_path_metrics(256, sums, metrics, paths, norm);

}
