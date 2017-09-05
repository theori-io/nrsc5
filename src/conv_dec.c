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

#include "config.h"

#include <stdlib.h>
#ifndef __APPLE__
#include <malloc.h>
#endif
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "defines.h"
#include "conv.h"

#if defined(HAVE_SSE3)
#include "conv_sse.h"
#elif defined(HAVE_NEON)
#include "conv_neon.h"
#else
#include "conv_gen.h"
#endif

#define PARITY(X) __builtin_parity(X)

/*
 * Trellis State
 *
 * state - Internal shift register value
 * prev  - Register values of previous 0 and 1 states
 */
struct vstate {
	unsigned state;
	unsigned prev[2];
};

/*
 * Trellis Object
 *
 * num_states - Number of states in the trellis
 * sums       - Accumulated path metrics
 * outputs    - Trellis ouput values
 * vals       - Input value that led to each state
 */
struct vtrellis {
	int num_states;
	int16_t *sums;
	int16_t *outputs;
	uint8_t *vals;
};

/*
 * Viterbi Decoder
 *
 * n         - Code order
 * k         - Constraint length
 * len       - Horizontal length of trellis
 * recursive - Set to '1' if the code is recursive
 * intrvl    - Normalization interval
 * trellis   - Trellis object
 * punc      - Puncturing sequence
 * paths     - Trellis paths
 */
struct vdecoder {
	int n;
	int k;
	int len;
	int recursive;
	int intrvl;
	struct vtrellis *trellis;
	int *punc;
	int16_t **paths;

	void (*metric_func)(const int8_t *, const int16_t *,
			    int16_t *, int16_t *, int);
};

/*
 * Aligned Memory Allocator
 *
 * SSE requires 16-byte memory alignment. We store relevant trellis values
 * (accumulated sums, outputs, and path decisions) as 16 bit signed integers
 * so the allocated memory is casted as such.
 */
#define SSE_ALIGN	16

static int16_t *vdec_malloc(size_t n)
{
#if defined(HAVE_SSE3) && !defined(__APPLE__)
	return (int16_t *) memalign(SSE_ALIGN, sizeof(int16_t) * n);
#else
	return (int16_t *) malloc(sizeof(int16_t) * n);
#endif
}

/* Left shift and mask for finding the previous state */
static unsigned vstate_lshift(unsigned reg, int k, int val)
{
	unsigned mask;

	if (k == 5)
		mask = 0x0e;
	else if (k == 7)
		mask = 0x3e;
	else
		mask = 0;

	return ((reg << 1) & mask) | val;
}

/*
 * Populate non-recursive trellis state
 *
 * For a given state defined by the k-1 length shift register, find the
 * value of the input bit that drove the trellis to that state. Then
 * generate the N outputs of the generator polynomial at that state.
 */
static void gen_state_info(const struct lte_conv_code *code,
			   uint8_t *val, unsigned reg, int16_t *out)
{
	int i;
	unsigned prev;

	/* Previous '0' state */
	prev = vstate_lshift(reg, code->k, 0);

	/* Compute output and unpack to NRZ */
	*val = (reg >> (code->k - 2)) & 0x01;
	prev = prev | (unsigned) *val << (code->k - 1);

	for (i = 0; i < code->n; i++)
		out[i] = PARITY(prev & code->gen[i]) * 2 - 1;
}

/*
 * Populate recursive trellis state
 */
static void gen_rec_state_info(const struct lte_conv_code *code,
			       uint8_t *val, unsigned reg, int16_t *out)
{
	int i;
	unsigned prev, rec, mask;

	/* Previous '0' and '1' states */
	prev = vstate_lshift(reg, code->k, 0);

	/* Compute recursive input value (not the value shifted into register) */
	rec = (reg >> (code->k - 2)) & 0x01;

	if (PARITY(prev & code->rgen) == rec)
		*val = 0;
	else
		*val = 1;

	/* Compute outputs and unpack to NRZ */
	prev = prev | rec << (code->k - 1);

	if (code->k == 5)
		mask = 0x0f;
	else
		mask = 0x3f;

	/* Check for recursive outputs */
	for (i = 0; i < code->n; i++) {
		if (code->gen[i] & mask)
			out[i] = PARITY(prev & code->gen[i]) * 2 - 1;
		else
			out[i] = *val * 2 - 1;
	}
}

/* Release the trellis */
static void free_trellis(struct vtrellis *trellis)
{
	if (!trellis)
		return;

	free(trellis->vals);
	free(trellis->outputs);
	free(trellis->sums);
	free(trellis);
}

#define NUM_STATES(K)	(K == 7 ? 64 : 16)

/*
 * Allocate and initialize the trellis object
 *
 * Initialization consists of generating the outputs and output value of a
 * given state. Due to trellis symmetry, only one of the transition paths
 * is used by the butterfly operation in the forward recursion, so only one
 * set of N outputs is required per state variable.
 */
static struct vtrellis *generate_trellis(const struct lte_conv_code *code)
{
	int i;
	struct vtrellis *trellis;
	int16_t *out;

	int ns = NUM_STATES(code->k);
	int olen = (code->n == 2) ? 2 : 4;

	trellis = (struct vtrellis *) calloc(1, sizeof(struct vtrellis));
	trellis->num_states = ns;
	trellis->sums =	vdec_malloc(ns);
	trellis->outputs = vdec_malloc(ns * olen);
	trellis->vals = (uint8_t *) malloc(ns * sizeof(uint8_t));

	if (!trellis->sums || !trellis->outputs)
		goto fail;

	/* Populate the trellis state objects */
	for (i = 0; i < ns; i++) {
		out = &trellis->outputs[olen * i];

		if (code->rgen)
			gen_rec_state_info(code, &trellis->vals[i], i, out);
		else
			gen_state_info(code, &trellis->vals[i], i, out);
	}

	return trellis;
fail:
	free_trellis(trellis);
	return NULL;
}

/*
 * Reset decoder
 *
 * Set accumulated path metrics to zero. For termination other than
 * tail-biting, initialize the zero state as the encoder starting state.
 * Intialize with the maximum accumulated sum at length equal to the
 * constraint length.
 */
static void reset_decoder(struct vdecoder *dec, int term)
{
	int ns = dec->trellis->num_states;

	memset(dec->trellis->sums, 0, sizeof(int16_t) * ns);

	if (term != CONV_TERM_TAIL_BITING)
		dec->trellis->sums[0] = INT8_MAX * dec->n * dec->k;
}

static int _traceback(struct vdecoder *dec,
		       unsigned state, uint8_t *out, int len)
{
	int i;
	unsigned path;

	for (i = len - 1; i >= 0; i--) {
		path = dec->paths[i][state] + 1;
		out[i] = dec->trellis->vals[state];
		state = vstate_lshift(state, dec->k, path);
	}

	return state;
}

static void _traceback_rec(struct vdecoder *dec,
			   unsigned state, uint8_t *out, int len)
{
	int i;
	unsigned path;

	for (i = len - 1; i >= 0; i--) {
		path = dec->paths[i][state] + 1;
		out[i] = path ^ dec->trellis->vals[state];
		state = vstate_lshift(state, dec->k, path);
	}
}

/*
 * Traceback and generate decoded output
 *
 * For tail biting, find the largest accumulated path metric at the final state
 * followed by two trace back passes. For zero flushing the final state is
 * always zero with a single traceback path.
 */
static int traceback(struct vdecoder *dec, uint8_t *out, int term, int len)
{
	int i, sum, max_p = -1, max = -1;
	unsigned path, state = 0;

	if (term == CONV_TERM_TAIL_BITING) {
		for (i = 0; i < dec->trellis->num_states; i++) {
			sum = dec->trellis->sums[i];
			if (sum > max) {
				max_p = max;
				max = sum;
				state = i;
			}
		}
		if (max < 0)
			return -EPROTO;
	} else {
		for (i = dec->len - 1; i >= len; i--) {
			path = dec->paths[i][state] + 1;
			state = vstate_lshift(state, dec->k, path);
		}
	}

	if (dec->recursive)
		_traceback_rec(dec, state, out, len);
	else
		state =_traceback(dec, state, out, len);

	/* Don't handle the odd case of recursize tail-biting codes */
	if (term == CONV_TERM_TAIL_BITING)
		_traceback(dec, state, out, len);

	return max - max_p;
}

/* Release decoder object */
static void free_vdec(struct vdecoder *dec)
{
	if (!dec)
		return;

	free(dec->paths[0]);
	free(dec->paths);
	free_trellis(dec->trellis);
	free(dec);
}

/*
 * Allocate decoder object
 *
 * Subtract the constraint length K on the normalization interval to
 * accommodate the initialization path metric at state zero.
 */
static struct vdecoder *alloc_vdec(const struct lte_conv_code *code)
{
	int i, ns;
	struct vdecoder *dec;

	ns = NUM_STATES(code->k);

	dec = (struct vdecoder *) calloc(1, sizeof(struct vdecoder));
	dec->n = code->n;
	dec->k = code->k;
	dec->recursive = code->rgen ? 1 : 0;
	dec->intrvl = INT16_MAX / (dec->n * INT8_MAX) - dec->k;

    assert(dec->n == 3);
    assert(dec->k == 7);

	if (code->term == CONV_TERM_FLUSH)
		dec->len = code->len + code->k - 1;
	else
		dec->len = code->len;

	dec->trellis = generate_trellis(code);
	if (!dec->trellis)
		goto fail;

	dec->paths = (int16_t **) malloc(sizeof(int16_t *) * dec->len);
	dec->paths[0] = vdec_malloc(ns * dec->len);
	for (i = 1; i < dec->len; i++)
		dec->paths[i] = &dec->paths[0][i * ns];

	return dec;
fail:
	free_vdec(dec);
	return NULL;
}

/*
 * Forward trellis recursion
 *
 * Generate branch metrics and path metrics with a combined function. Only
 * accumulated path metric sums and path selections are stored. Normalize on
 * the interval specified by the decoder.
 */
static void _conv_decode(struct vdecoder *dec, const int8_t *seq, int len)
{
	int i;
	struct vtrellis *trellis = dec->trellis;

	for (i = 0; i < dec->len; i++) {
		gen_metrics_k7_n3(&seq[dec->n * i],
				 trellis->outputs,
				 trellis->sums,
				 dec->paths[i],
				 !(i % dec->intrvl));
	}
}

int nrsc5_conv_decode_p1(const int8_t *in, uint8_t *out)
{
	const struct lte_conv_code code = {
		.n = 3,
		.k = 7,
		.len = P1_FRAME_LEN,
		.gen = { 0133, 0171, 0165 },
		.term = CONV_TERM_TAIL_BITING,
	};
	int rc;

	struct vdecoder *vdec = alloc_vdec(&code);
	if (!vdec)
		return -EFAULT;

	reset_decoder(vdec, code.term);

	/* Propagate through the trellis with interval normalization */
	_conv_decode(vdec, in, code.len);

	if (code.term == CONV_TERM_TAIL_BITING)
		_conv_decode(vdec, in, code.len);

	rc = traceback(vdec, out, code.term, code.len);

	free_vdec(vdec);
	return rc;
}

int nrsc5_conv_decode_pids(const int8_t *in, uint8_t *out)
{
	const struct lte_conv_code code = {
		.n = 3,
		.k = 7,
		.len = PIDS_FRAME_LEN,
		.gen = { 0133, 0171, 0165 },
		.term = CONV_TERM_TAIL_BITING,
	};
	int rc;

	struct vdecoder *vdec = alloc_vdec(&code);
	if (!vdec)
		return -EFAULT;

	reset_decoder(vdec, code.term);

	/* Propagate through the trellis with interval normalization */
	_conv_decode(vdec, in, code.len);

	if (code.term == CONV_TERM_TAIL_BITING)
		_conv_decode(vdec, in, code.len);

	rc = traceback(vdec, out, code.term, code.len);

	free_vdec(vdec);
	return rc;
}

int nrsc5_conv_decode_p3(const int8_t *in, uint8_t *out)
{
	const struct lte_conv_code code = {
		.n = 3,
		.k = 7,
		.len = P3_FRAME_LEN,
		.gen = { 0133, 0171, 0165 },
		.term = CONV_TERM_TAIL_BITING,
	};
	int rc;

	struct vdecoder *vdec = alloc_vdec(&code);
	if (!vdec)
		return -EFAULT;

	reset_decoder(vdec, code.term);

	/* Propagate through the trellis with interval normalization */
	_conv_decode(vdec, in, code.len);

	if (code.term == CONV_TERM_TAIL_BITING)
		_conv_decode(vdec, in, code.len);

	rc = traceback(vdec, out, code.term, code.len);

	free_vdec(vdec);
	return rc;
}
