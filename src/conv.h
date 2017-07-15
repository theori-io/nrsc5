#ifndef _CONV_H_
#define _CONV_H_

#include <stdint.h>

enum {
	CONV_TERM_FLUSH,
	CONV_TERM_TAIL_BITING,
};

/*
 * Convolutional code descriptor
 *
 * n    - Rate 2, 3, 4 (1/2, 1/3, 1/4)
 * k    - Constraint length (5 or 7)
 * rgen - Recursive generator polynomial in octal
 * gen  - Generator polynomials in octal
 * punc - Puncturing matrix (-1 terminated)
 * term - Termination type (zero flush default)
 */
struct lte_conv_code {
	int n;
	int k;
	int len;
	unsigned rgen;
	unsigned gen[4];
	int *punc;
	int term;
};

int nrsc5_conv_decode_p1(const int8_t *in, uint8_t *out);
int nrsc5_conv_decode_pids(const int8_t *in, uint8_t *out);
int nrsc5_conv_decode_p3(const int8_t *in, uint8_t *out);

#endif /* _CONV_H_ */
