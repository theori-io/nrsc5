#pragma once

#include <stdio.h>
#include <stdlib.h>

#define ERR(x,...) fprintf(stderr, x, ##__VA_ARGS__)
#define ERR_FAIL(x,...) do { fprintf(stderr, x, ##__VA_ARGS__); exit(1); } while (0)

#define INPUT_BUF_LEN (2160 * 512)
#define FFT 2048
#define CP 112
#define K (FFT + CP)
#define N 32
#define REF_PER_BAND 11
#define DATA_PER_BAND 180
#define BAND_LENGTH (REF_PER_BAND + DATA_PER_BAND)
#define TOTAL_DATA (DATA_PER_BAND * 2)
#define TOTAL_REF (REF_PER_BAND * 2)
#define LB_START (1024 - 546)
#define UB_START (1024 + 356)
#define UB_OFFSET (UB_START - LB_START)
#define SYNCLEN (UB_OFFSET + BAND_LENGTH)
#define FRAME_LEN 146176
