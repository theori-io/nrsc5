#include "here_tpeg.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static unsigned int events;

static uint16_t crc_tpeg(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFF;
    for (size_t i = 0; i < size; i++)
    {
        uint32_t tmp = ((crc << 8) & 0xFFFF) | (crc >> 8);
        crc = (tmp ^ data[i]) & 0xFFFF;
        crc ^= (crc & 0x00FF) >> 4;
        tmp = ((crc & 0x00FF) << 8) | ((crc & 0x00FF) >> 8);
        crc = (crc ^ (tmp << 4) ^ ((crc & 0x00FF) << 5)) & 0xFFFF;
    }
    return (uint16_t) (crc ^ 0xFFFF);
}

static size_t add_component(uint8_t *out, uint8_t scid,
                            const uint8_t *payload, uint16_t size)
{
    uint8_t header[16];
    uint16_t crc;
    size_t verify = size < 13 ? size : 13;

    out[0] = scid;
    out[1] = size >> 8;
    out[2] = size;
    header[0] = out[0];
    header[1] = out[1];
    header[2] = out[2];
    memcpy(header + 3, payload, verify);
    crc = crc_tpeg(header, verify + 3);
    out[3] = crc >> 8;
    out[4] = crc;
    memcpy(out + 5, payload, size);
    return size + 5;
}

static void flow_callback(const nrsc5_here_tfp_t *flow, void *opaque)
{
    (void) opaque;
    assert(flow->message_id == 1093422784);
    assert(flow->version == 12);
    assert(flow->expiry_time == 1787763604);
    assert(flow->cancel == 0);
    assert(flow->start_time == 1787762704);
    assert(flow->duration == -1);
    assert(flow->spatial_resolution == 0);
    assert(flow->polygon_index == 1402);
    assert(flow->level_of_service == 1);
    assert(flow->average_speed == 78);
    assert(flow->free_flow_travel_time == -1);
    assert(flow->delay == -1);
    assert(flow->location == 20160);
    assert(flow->country_code == 1);
    assert(flow->location_table_number == 22);
    assert(flow->direction_positive == 1);
    assert(flow->both_directions == 0);
    assert(flow->extent == 5);
    events++;
}

int main(void)
{
    uint8_t frame[128];
    uint8_t sni[] = {
        0x01, 0x01, 0x00, 0x07,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07,
        0x00, 0x00
    };
    uint8_t tfp[] = {
        0x00, 0x01,
        0x00, 0x30, 0x00,
        0x01, 0x0c, 0x0b, 0x84, 0x89, 0xb1, 0x9d, 0x40, 0x0c, 0x6a, 0x8f,
        0x1b, 0x94, 0x00,
        0x06, 0x13, 0x06, 0x6a, 0x8f, 0x18, 0x10, 0x00, 0x01,
        0x07, 0x0a, 0x09, 0x00, 0x01, 0x8a, 0x7a, 0x60, 0x01, 0x4e, 0x00, 0x00,
        0x02, 0x0a, 0x00, 0x02, 0x07, 0x06, 0x4e, 0xc0, 0x01, 0x16, 0x50, 0x05,
        0x00, 0x00
    };
    size_t size = 0;
    uint16_t crc;

    crc = crc_tpeg(sni, sizeof(sni) - 2);
    sni[sizeof(sni) - 2] = crc >> 8;
    sni[sizeof(sni) - 1] = crc;
    crc = crc_tpeg(tfp, sizeof(tfp) - 2);
    tfp[sizeof(tfp) - 2] = crc >> 8;
    tfp[sizeof(tfp) - 1] = crc;

    size += add_component(frame + size, 0, sni, sizeof(sni));
    size += add_component(frame + size, 2, tfp, sizeof(tfp));
    assert(here_decode_frame(frame, size, flow_callback, NULL) == 0);
    assert(events == 1);
    return 0;
}
