#include "here_tpeg.h"

#include <string.h>

/*
 * HERE TPEG transport (ISO/TS 21219-5, TPEG2-SFW). All framing facts are
 * verified against a live 100.3 MHz WOMC capture and the TISA evaluation-kit
 * reference implementation (BSD-3 licensed).
 *
 *  component frame    := SCID:u8 fieldlength:u16BE hdrCRC:u16BE payload
 *                        fieldlength counts the payload after hdrCRC;
 *                        hdrCRC = TPEG_CRC(SCID,len_hi,len_lo + first
 *                        min(13, fieldlength) bytes of that region)
 *  frame continuation := [priority:u8] msgCount:u8 content dataCRC:u16BE
 *  SNI components     := CompID:u8 compLen:u16BE content
 *  fast tuning table  := tblVer:u8 charTbl:u8 entries{ scid:u8 sel:u8 [...]
 *                        COID:u8 AID:u16BE } while data remains
 *  message header     := msgID:IntUnLoMB version:u8 expiryTime:u32BE selector:u8
 *
 * AID 5 = TEC (ISO/TS 21219-15), AID 7 = TFP (ISO/TS 21219-18).
 */

#define HERE_AID_SNI 0
#define HERE_AID_TEC 5
#define HERE_AID_TFP 7

typedef struct {
    uint8_t *data;
    size_t size;
    size_t off;
} here_cursor_t;

struct here_tfp_sink {
    here_tfp_cb flow;
    void *opaque;
    unsigned int aid_by_scid[256];
};

typedef struct {
    uint8_t id;
    const uint8_t *attrs;
    size_t attrs_size;
    const uint8_t *children;
    size_t children_size;
} here_component_t;

static int cur_read(here_cursor_t *cur, size_t len, const uint8_t **out)
{
    if (cur->size - cur->off < len)
        return -1;
    *out = cur->data + cur->off;
    cur->off += len;
    return 0;
}

static int cur_u8(here_cursor_t *cur, uint8_t *out)
{
    if (cur->off >= cur->size)
        return -1;
    *out = cur->data[cur->off++];
    return 0;
}

static int cur_u16(here_cursor_t *cur, uint16_t *out)
{
    const uint8_t *p;
    if (cur_read(cur, 2, &p) != 0)
        return -1;
    *out = ((uint16_t) p[0] << 8) | p[1];
    return 0;
}

static int cur_lomb(here_cursor_t *cur, uint32_t *out)
{
    uint32_t value = 0;
    while (cur->off < cur->size)
    {
        value = (value << 7) | (cur->data[cur->off] & 0x7F);
        if (!(cur->data[cur->off] & 0x80))
        {
            *out = value;
            cur->off++;
            return 0;
        }
        cur->off++;
    }
    return -1;
}

static int cur_bitarray(here_cursor_t *cur, uint32_t *out)
{
    uint32_t options = 0;
    for (unsigned int byte = 0; byte < 5; byte++)
    {
        uint8_t value;
        if (cur_u8(cur, &value) != 0)
            return -1;
        for (unsigned int bit = 0; bit < 7; bit++)
            if (value & (0x40 >> bit))
                options |= 1U << (byte * 7 + bit);
        if (!(value & 0x80))
        {
            *out = options;
            return 0;
        }
    }
    return -1;
}

static int cur_component(here_cursor_t *cur, here_component_t *component)
{
    uint32_t comp_len, attr_len;
    const uint8_t *body;
    here_cursor_t body_cur;

    if (cur_u8(cur, &component->id) != 0 || cur_lomb(cur, &comp_len) != 0
        || cur_read(cur, comp_len, &body) != 0)
        return -1;
    body_cur = (here_cursor_t) { (uint8_t *) body, comp_len, 0 };
    if (cur_lomb(&body_cur, &attr_len) != 0
        || attr_len > body_cur.size - body_cur.off)
        return -1;
    component->attrs = body + body_cur.off;
    component->attrs_size = attr_len;
    component->children = component->attrs + attr_len;
    component->children_size = comp_len - body_cur.off - attr_len;
    return 0;
}

static uint16_t crc_tpeg(const uint8_t *data, size_t size)
{
    /* TISA TPEG_CRC (evaluation-kit Base/TPEG_CRC.py), BSD-3. */
    uint32_t crc = 0xFFFF;
    for (size_t i = 0; i < size; i++)
    {
        uint32_t tmp = ((crc << 8) & 0xFFFF) | (crc >> 8);
        crc = (tmp ^ data[i]) & 0xFFFF;
        crc ^= ((crc & 0x00FF) >> 4);
        tmp = ((crc & 0x00FF) << 8) | ((crc & 0x00FF) >> 8);
        crc = ((crc ^ (tmp << 4)) ^ ((crc & 0x00FF) << 5)) & 0xFFFF;
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

static int decode_mmc(nrsc5_here_tfp_t *flow, const uint8_t *data, size_t size)
{
    here_cursor_t cur = { (uint8_t *) data, size, 0 };
    uint8_t version;
    uint32_t selector;

    if (cur_lomb(&cur, &flow->message_id) != 0)
        return -1;
    if (cur_u8(&cur, &version) != 0)
        return -1;
    {
        const uint8_t *p;
        if (cur_read(&cur, 4, &p) != 0)
            return -1;
        flow->expiry_time = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
                          | ((uint32_t) p[2] << 8) | p[3];
    }
    if (cur_bitarray(&cur, &selector) != 0)
        return -1;
    flow->version = version;
    flow->cancel = selector & 0x01;
    return 0;
}

static void decode_sni_component(const uint8_t *attrs, size_t size,
                                  struct here_tfp_sink *sink)
{
    /* SNI_01 "Guide to the service table 1 (Fast Tuning)" per ISO/TS 21219-9:
     *   tableVersion:u8 characterTable:u8 entries...
     *   entry := scid:u8 selector:u8 [SIDa.b.c if selector&1]
     *            coid:u8 aid:u16BE [optime x2 if selector&4]
     *            [EncID:u8 if selector&8] */
    here_cursor_t cur = { (uint8_t *) attrs, size, 2 };

    while (cur.off < cur.size)
    {
        uint8_t scid, selector, coid;
        uint16_t aid;
        if (cur_u8(&cur, &scid) || cur_u8(&cur, &selector))
            break;
        if (selector & 0x01)
        {
            const uint8_t *skip;
            if (cur_read(&cur, 3, &skip) != 0)
                break;
        }
        if (cur_u8(&cur, &coid) || cur_u16(&cur, &aid))
            break;
        if (selector & 0x04)
        {
            const uint8_t *skip;
            if (cur_read(&cur, 10, &skip) != 0)
                break;
        }
        if (selector & 0x08)
        {
            uint8_t encid;
            if (cur_u8(&cur, &encid) != 0)
                break;
        }
        sink->aid_by_scid[scid] = aid;
    }
}

static void decode_app_component(struct here_tfp_sink *sink, uint32_t aid,
                                 const uint8_t *content, size_t size);

static void decode_sni_continuation(here_cursor_t *cur,
                                     struct here_tfp_sink *sink)
{
    uint8_t count;

    if (cur_u8(cur, &count) != 0)
        return;
    for (unsigned int i = 0; i < count; i++)
    {
        uint8_t comp_id;
        uint16_t comp_len;
        const uint8_t *body;

        if (cur_u8(cur, &comp_id) != 0 || cur_u16(cur, &comp_len) != 0
            || cur_read(cur, comp_len, &body) != 0)
            return;
        if (comp_id == 1)
            decode_sni_component(body, comp_len, sink);
    }
}

/* One SCID-n component frame continuation:
 *   msgCount:u8 messages{ components... } dataCRC:u16BE */
static void decode_component_continuation(uint32_t scid, const uint8_t *payload,
                                           size_t size, struct here_tfp_sink *sink)
{
    uint16_t stored_crc;
    uint8_t count, priority;
    here_cursor_t cur = { (uint8_t *) payload, size > 2 ? size - 2 : 0, 0 };

    if (size < 2)
        return;
    stored_crc = ((uint16_t) payload[size-2] << 8) | payload[size-1];
    if (crc_tpeg(payload, size - 2) != stored_crc)
        return;
    if (scid == HERE_AID_SNI)
    {
        decode_sni_continuation(&cur, sink);
        return;
    }
    if (cur_u8(&cur, &priority) != 0)
        return;
    if (cur_u8(&cur, &count) != 0)
        return;
    (void) priority;

    for (unsigned int i = 0; i < count && cur.off < cur.size; i++)
    {
        uint8_t comp_id;
        uint32_t comp_len, attr_len;

        if (cur_u8(&cur, &comp_id) != 0)
            break;
        if (cur_lomb(&cur, &comp_len) != 0)
            break;
        if (cur.size - cur.off < comp_len)
            break;
        {
            const uint8_t *body = cur.data + cur.off;
            here_cursor_t body_cur = { (uint8_t *) body, comp_len, 0 };
            if (cur_lomb(&body_cur, &attr_len) != 0)
                break;
            if (attr_len > comp_len - body_cur.off)
                break;
            decode_app_component(sink, sink->aid_by_scid[scid],
                                 body + body_cur.off + attr_len,
                                 comp_len - body_cur.off - attr_len);
        }
        cur.off += comp_len;
    }
}

static void init_flow(nrsc5_here_tfp_t *flow)
{
    memset(flow, 0, sizeof(*flow));
    flow->duration = -1;
    flow->level_of_service = -1;
    flow->average_speed = -1;
    flow->free_flow_travel_time = -1;
    flow->delay = -1;
}

static int decode_status(here_cursor_t *cur, nrsc5_here_tfp_t *flow)
{
    uint32_t selector, value;
    uint8_t tiny;

    if (cur_bitarray(cur, &selector) != 0)
        return -1;
    if (selector & 0x01)
    {
        if (cur_u8(cur, &tiny) != 0)
            return -1;
        flow->level_of_service = tiny;
    }
    if (selector & 0x02)
    {
        if (cur_u8(cur, &tiny) != 0)
            return -1;
        flow->average_speed = tiny;
    }
    if (selector & 0x04)
    {
        if (cur_lomb(cur, &value) != 0)
            return -1;
        flow->free_flow_travel_time = value;
    }
    if (selector & 0x08)
    {
        if (cur_lomb(cur, &value) != 0)
            return -1;
        flow->delay = value;
    }
    return 0;
}

static unsigned int decode_flow_polygon_object(const uint8_t *attrs, size_t size,
                                                nrsc5_here_tfp_t *flows,
                                                unsigned int capacity)
{
    here_cursor_t cur = { (uint8_t *) attrs, size, 0 };
    uint32_t spatial_resolution, count;
    unsigned int decoded = 0;

    if (cur_lomb(&cur, &spatial_resolution) != 0 || cur_lomb(&cur, &count) != 0)
        return 0;
    for (uint32_t i = 0; i < count && decoded < capacity; i++)
    {
        nrsc5_here_tfp_t *flow = &flows[decoded];
        uint32_t selector;

        init_flow(flow);
        flow->spatial_resolution = spatial_resolution;
        if (cur_lomb(&cur, &flow->polygon_index) != 0
            || decode_status(&cur, flow) != 0
            || cur_bitarray(&cur, &selector) != 0)
            break;
        decoded++;
        if (selector != 0)
            break;
    }
    return decoded;
}

static unsigned int decode_method(const here_component_t *method,
                                  nrsc5_here_tfp_t *flows,
                                  unsigned int capacity)
{
    here_cursor_t attrs = { (uint8_t *) method->attrs, method->attrs_size, 0 };
    here_cursor_t children = { (uint8_t *) method->children, method->children_size, 0 };
    const uint8_t *time;
    uint32_t selector, duration = 0;
    uint8_t choice;
    unsigned int count = 0;

    if (cur_read(&attrs, 4, &time) != 0 || cur_bitarray(&attrs, &selector) != 0)
        return 0;
    if ((selector & 0x01) && cur_lomb(&attrs, &duration) != 0)
        return 0;
    if (cur_u8(&attrs, &choice) != 0 || choice != 1)
        return 0;

    while (children.off < children.size)
    {
        here_component_t component;
        if (cur_component(&children, &component) != 0)
            break;
        if (component.id == 7)
            count += decode_flow_polygon_object(component.attrs, component.attrs_size,
                                                flows + count, capacity - count);
    }
    for (unsigned int i = 0; i < count; i++)
    {
        flows[i].start_time = ((uint32_t) time[0] << 24) | ((uint32_t) time[1] << 16)
                            | ((uint32_t) time[2] << 8) | time[3];
        if (selector & 0x01)
            flows[i].duration = duration;
    }
    return count;
}

static int decode_tmc_location(const uint8_t *attrs, size_t size,
                               nrsc5_here_tfp_t *flow)
{
    here_cursor_t cur = { (uint8_t *) attrs, size, 0 };
    uint32_t selector;
    uint8_t extent;

    if (cur_u16(&cur, &flow->location) != 0
        || cur_u8(&cur, &flow->country_code) != 0
        || cur_u8(&cur, &flow->location_table_number) != 0
        || cur_bitarray(&cur, &selector) != 0)
        return -1;
    flow->direction_positive = !!(selector & 0x01);
    flow->both_directions = !!(selector & 0x02);
    if (selector & 0x04)
    {
        if (cur_u8(&cur, &extent) != 0)
            return -1;
        flow->extent = extent;
    }
    return 0;
}

static void decode_location(const here_component_t *location,
                            nrsc5_here_tfp_t *flow)
{
    here_cursor_t children = { (uint8_t *) location->children,
                               location->children_size, 0 };

    while (children.off < children.size)
    {
        here_component_t method;
        if (cur_component(&children, &method) != 0)
            return;
        if (method.id == 2)
        {
            decode_tmc_location(method.attrs, method.attrs_size, flow);
            return;
        }
    }
}

static void decode_app_component(struct here_tfp_sink *sink, uint32_t aid,
                                 const uint8_t *content, size_t size)
{
    here_cursor_t cur = { (uint8_t *) content, size, 0 };
    nrsc5_here_tfp_t common;
    nrsc5_here_tfp_t flows[100];
    unsigned int flow_count = 0;

    if (aid != HERE_AID_TFP)
        return;
    init_flow(&common);

    while (cur.off < cur.size)
    {
        here_component_t component;
        if (cur_component(&cur, &component) != 0)
            return;
        if (component.id == 1)
            decode_mmc(&common, component.attrs, component.attrs_size);
        else if (component.id == 6 && flow_count < 100)
            flow_count += decode_method(&component, flows + flow_count, 100 - flow_count);
        else if (component.id == 2)
            decode_location(&component, &common);
    }
    for (unsigned int i = 0; i < flow_count; i++)
    {
        flows[i].message_id = common.message_id;
        flows[i].version = common.version;
        flows[i].expiry_time = common.expiry_time;
        flows[i].cancel = common.cancel;
        flows[i].location = common.location;
        flows[i].country_code = common.country_code;
        flows[i].location_table_number = common.location_table_number;
        flows[i].direction_positive = common.direction_positive;
        flows[i].both_directions = common.both_directions;
        flows[i].extent = common.extent;
        sink->flow(&flows[i], sink->opaque);
    }
}

int here_decode_frame(const uint8_t *payload, size_t size,
                       here_tfp_cb flow, void *opaque)
{
    struct here_tfp_sink sink;
    here_cursor_t cur;
    unsigned int i;

    if (!payload || !flow)
        return -1;
    memset(&sink, 0, sizeof(sink));
    sink.flow = flow;
    sink.opaque = opaque;

    cur.data = (uint8_t *) payload;
    cur.size = size;
    cur.off = 0;
    for (i = 0; i < 256; i++)
        sink.aid_by_scid[i] = 0;

    while (cur.off + 5 <= cur.size)
    {
        uint8_t scid;
        uint16_t fieldlength, hdr_crc;
        const uint8_t *frame_payload;

        if (cur_u8(&cur, &scid) != 0)
            break;
        if (cur_u16(&cur, &fieldlength) != 0 || fieldlength < 2)
            break;
        if (cur_u16(&cur, &hdr_crc) != 0)
            break;
        if (cur.size - cur.off < fieldlength)
            break;
        {
            uint8_t head[16];
            size_t verify_len = fieldlength < 13 ? fieldlength : 13;
            head[0] = scid;
            head[1] = (uint8_t)(fieldlength >> 8);
            head[2] = (uint8_t) fieldlength;
            memcpy(head + 3, cur.data + cur.off, verify_len);
            if (crc_tpeg(head, 3 + verify_len) != hdr_crc)
            {
                cur.off += fieldlength;
                continue;
            }
        }
        frame_payload = cur.data + cur.off;
        decode_component_continuation(scid, frame_payload, fieldlength, &sink);
        cur.off += fieldlength;
    }
    return 0;
}
