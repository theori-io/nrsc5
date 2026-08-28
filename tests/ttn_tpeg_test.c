#include "ttn_tpeg.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static unsigned int events;

static void tec_callback(const nrsc5_ttn_tec_t *event, void *opaque)
{
    (void)opaque;
    assert(event->message_id == 1);
    events++;
}

int main(void)
{
    /* SID 0.0.31, EncID 0x2b, followed by a TEC component frame with SCID 0x9a. */
    static const uint8_t payload[] = {
        0x00, 0x00, 0x1f, 0x2b,
        0x9a, 0x00, 0x11, 0x00, 0x00,
        0x01, 0x01,
        0x00, 0x0b, 0x00,
        0x01, 0x08, 0x07, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };

    assert(ttn_tpeg1_decode(payload, sizeof(payload), NULL, tec_callback, NULL) == 0);
    assert(events == 1);
    return 0;
}
