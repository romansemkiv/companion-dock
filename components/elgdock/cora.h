#pragma once
/* Elgato TCP ("Cora") framing, pure C. Ported from @elgato-stream-deck/tcp 7.8.0
 * (packages/tcp/src/socketWrapper.ts) under its MIT licence (cora.h and cora.c):
 *
 *   Copyright (c) 2024 Julian Waller
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions
 *   of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *   IN THE SOFTWARE.
 *
 * Every message is a 16-byte header, then the payload.
 *   0..3   magic 43 93 8a 41
 *   4..5   flags, u16 LE (CORA_F_*)
 *   6      hidOp (CORA_OP_*)
 *   7      unused (0)
 *   8..11  message id, u32 LE (the host's "STAN"; a reply carries the request's)
 *   12..15 payload length, u32 LE */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A little-endian 16-bit store (cora.c and dockproto.c: one copy). */
static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

#ifdef __cplusplus
extern "C" {
#endif

#define CORA_HDR_LEN 16

extern const uint8_t cora_magic[4];

/* CoraMessageFlags */
#define CORA_F_NONE 0x0000
#define CORA_F_VERBATIM 0x8000 /* payload is for / from the HID device itself */
#define CORA_F_REQ_ACK 0x4000  /* host asks for an ACK */
#define CORA_F_ACK_NAK 0x0200  /* answer to REQ_ACK; the host's keepalive ACK carries it */
#define CORA_F_RESULT 0x0100   /* answer to a GET_REPORT */

/* CoraHidOp */
#define CORA_OP_WRITE 0x00       /* hid_write: an output report */
#define CORA_OP_SEND_REPORT 0x01 /* hid_send_feature_report */
#define CORA_OP_GET_REPORT 0x02  /* hid_get_feature_report */

typedef struct {
    uint16_t flags;
    uint8_t hid_op;
    uint32_t msg_id;
    const uint8_t *payload;
    uint32_t len;
} cora_msg_t;

/* Header plus payload into out. Returns the bytes written, 0 if they don't fit in cap. */
size_t cora_encode(const cora_msg_t *m, uint8_t *out, size_t cap);

/* Streaming parser. buf (cap bytes) holds one payload; a message announcing more than cap bytes
 * is skipped whole (counted in `oversize`). Bytes that don't start a header are dropped until the
 * magic comes (counted in `resync`). */
typedef struct {
    uint8_t *buf;
    size_t cap;
    uint8_t hdr[CORA_HDR_LEN];
    size_t hdr_have;
    uint32_t len;  /* payload length of the message being read */
    uint32_t have; /* payload bytes buffered */
    uint32_t skip; /* bytes of an oversize payload still to drop */
    uint32_t oversize, resync;
} cora_parser_t;

void cora_parser_init(cora_parser_t *p, uint8_t *buf, size_t cap);

/* Consumes data[0..n) up to the end of the first message that completes. Returns 1 with that
 * message in *out (payload points into buf: valid until the next cora_feed), 0 when all n bytes
 * were taken without completing one. *used = bytes taken either way. */
int cora_feed(cora_parser_t *p, const uint8_t *data, size_t n, size_t *used, cora_msg_t *out);

#ifdef __cplusplus
}
#endif
