#pragma once

#include <stddef.h>
#include <stdint.h>

#include <nrsc5.h>

/* Callback invoked for each decoded HERE TPEG2-TFP flow polygon. */
typedef void (*here_tfp_cb)(const nrsc5_here_tfp_t *flow, void *opaque);

/* Decode one inflated service-data frame body (post-zlib): a sequence of
 * SFW component frames. The fast-tuning registry is learned in-band from
 * the SNI and used to label application messages (AID 5 TEC, AID 7 TFP). */
int here_decode_frame(const uint8_t *payload, size_t size,
                       here_tfp_cb flow, void *opaque);
