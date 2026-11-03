/* Cora framing (cora.h). Pure C. */
#include "cora.h"

#include <string.h>

const uint8_t cora_magic[4] = {0x43, 0x93, 0x8a, 0x41};

static void wr32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t cora_encode(const cora_msg_t *m, uint8_t *out, size_t cap)
{
    if (!m || !out || (m->len && !m->payload)) return 0;
    const size_t total = CORA_HDR_LEN + (size_t)m->len;
    if (total > cap) return 0;
    memcpy(out, cora_magic, sizeof cora_magic);
    wr16(out + 4, m->flags);
    out[6] = m->hid_op;
    out[7] = 0;
    wr32(out + 8, m->msg_id);
    wr32(out + 12, m->len);
    if (m->len) memcpy(out + CORA_HDR_LEN, m->payload, m->len);
    return total;
}

void cora_parser_init(cora_parser_t *p, uint8_t *buf, size_t cap)
{
    memset(p, 0, sizeof *p);
    p->buf = buf;
    p->cap = cap;
}

static void emit(cora_parser_t *p, cora_msg_t *out)
{
    out->flags = (uint16_t)(p->hdr[4] | p->hdr[5] << 8);
    out->hid_op = p->hdr[6];
    out->msg_id = rd32(p->hdr + 8);
    out->payload = p->buf;
    out->len = p->len;
    p->hdr_have = 0; /* the next byte starts a new header */
}

int cora_feed(cora_parser_t *p, const uint8_t *data, size_t n, size_t *used, cora_msg_t *out)
{
    size_t i = 0;
    while (i < n) {
        if (p->skip) { /* the rest of an oversize payload */
            const size_t k = p->skip < n - i ? p->skip : n - i;
            p->skip -= (uint32_t)k;
            i += k;
            continue;
        }
        if (p->hdr_have < CORA_HDR_LEN) {
            const uint8_t b = data[i++];
            if (p->hdr_have < sizeof cora_magic && b != cora_magic[p->hdr_have]) {
                /* No proper prefix of the magic is also a suffix of it, so the only restart is
                 * at this byte, when it is the magic's first one. */
                p->resync += (uint32_t)p->hdr_have;
                p->hdr_have = 0;
                if (b != cora_magic[0]) {
                    p->resync++;
                    continue;
                }
            }
            p->hdr[p->hdr_have++] = b;
            if (p->hdr_have < CORA_HDR_LEN) continue;
            p->len = rd32(p->hdr + 12);
            p->have = 0;
            if (p->len > p->cap) {
                p->skip = p->len;
                p->oversize++;
                p->hdr_have = 0;
                continue;
            }
            if (p->len == 0) {
                emit(p, out);
                *used = i;
                return 1;
            }
            continue;
        }
        const size_t k = p->len - p->have < n - i ? p->len - p->have : n - i;
        memcpy(p->buf + p->have, data + i, k);
        p->have += (uint32_t)k;
        i += k;
        if (p->have == p->len) {
            emit(p, out);
            *used = i;
            return 1;
        }
    }
    *used = i;
    return 0;
}
