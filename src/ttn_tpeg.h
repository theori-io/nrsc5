#pragma once

#include <stddef.h>
#include <stdint.h>

#include <nrsc5.h>

const char *ttn_weather_condition_name(unsigned int condition);

/* Inflate one complete TTN TPEG-2 envelope. The caller owns *output. */
int ttn_tpeg2_inflate(const uint8_t *payload, size_t payload_size,
                      uint8_t **output, size_t *output_size);

typedef void (*ttn_tpeg2_city_database_cb)(const nrsc5_ttn_city_database_t *, void *);
typedef void (*ttn_tpeg2_weather_cb)(unsigned int, unsigned int,
                                     const nrsc5_ttn_weather_city_t *, void *);
typedef void (*ttn_tpeg2_service_network_cb)(const nrsc5_ttn_service_network_t *, void *);

/* Decode one inflated TTN envelope and invoke callbacks synchronously. */
int ttn_tpeg2_decode(const uint8_t *payload, size_t payload_size,
                     ttn_tpeg2_city_database_cb city_database,
                     ttn_tpeg2_weather_cb weather,
                     ttn_tpeg2_service_network_cb service_network, void *opaque);

typedef void (*ttn_tpeg1_tec_cb)(const nrsc5_ttn_tec_t *, void *);

/* Decode a TTN TPEG-1 TEC envelope. Event storage is owned by the decoder. */
int ttn_tpeg1_decode(const uint8_t *payload, size_t payload_size,
                     nrsc5_location_table_t *locations,
                     ttn_tpeg1_tec_cb tec, void *opaque);
