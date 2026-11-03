#include <string.h>
#include "deckproto/deckproto.h"

#define FEAT_MAX 32

static size_t feat_start(const deck_model_t *m, uint8_t *buf) {
    size_t n = m->feature_len > FEAT_MAX ? FEAT_MAX : m->feature_len;
    memset(buf, 0, n);
    return n;
}

size_t deckproto_feat_brightness(const deck_model_t *m, uint8_t pct, uint8_t *buf) {
    size_t n = feat_start(m, buf);
    if (pct > 100) pct = 100;
    if (m->gen == DECK_GEN2) {
        buf[0] = 0x03; buf[1] = 0x08; buf[2] = pct;
    } else {
        buf[0] = 0x05; buf[1] = 0x55; buf[2] = 0xAA; buf[3] = 0xD1; buf[4] = 0x01; buf[5] = pct;
    }
    return n;
}

size_t deckproto_feat_reset(const deck_model_t *m, uint8_t *buf) {
    size_t n = feat_start(m, buf);
    if (m->gen == DECK_GEN2) {
        buf[0] = 0x03; buf[1] = 0x02;
    } else {
        buf[0] = 0x0B; buf[1] = 0x63;
    }
    return n;
}

size_t deckproto_feat_fill_key(const deck_model_t *m, uint8_t hw, uint8_t r, uint8_t g, uint8_t b, uint8_t *buf) {
    if (m->gen != DECK_GEN2) return 0; /* gen1 has no fill-key command */
    size_t n = feat_start(m, buf);
    buf[0] = 0x03; buf[1] = 0x06; buf[2] = hw; buf[3] = r; buf[4] = g; buf[5] = b;
    return n;
}

uint8_t deckproto_serial_report_id(const deck_model_t *m) { return m->gen == DECK_GEN2 ? 0x06 : 0x03; }

/* Official docs: Mini reports AP2 firmware on 0xA1; Original v1 is undocumented, 0x04 kept. */
uint8_t deckproto_fw_report_id(const deck_model_t *m) {
    if (m->gen == DECK_GEN2) return 0x05;
    return m->gen == DECK_GEN1_MINI ? 0xA1 : 0x04;
}

/* @elgato-stream-deck/core 7.8.0 reads, per properties service: gen2 and the Pedal firmware 0x05 and
 * serial 0x06 (services/properties/gen2.ts:40,53, pedal.ts:20,32), the same as the box's; gen1 (the
 * Original v1 and the 6-key Mini family) firmware 0x04 (gen1.ts:38-41) and serial 0x03 (gen1.ts:54-64),
 * where the box reads the Mini's firmware from 0xA1 instead. */
size_t deckproto_kept_report_ids(const deck_model_t *m, uint8_t out[DECK_KEPT_REPORTS_MAX]) {
    size_t n = 0;
    out[n++] = deckproto_serial_report_id(m);
    out[n++] = deckproto_fw_report_id(m);
    if (m->gen != DECK_GEN2 && out[1] != 0x04) out[n++] = 0x04;
    return n;
}

/* Copies up to max_chars ASCII bytes starting at rep[start] (stops at NUL / end of report). */
static bool copy_ascii(const uint8_t *rep, size_t len, size_t start, size_t max_chars, char *out, size_t cap) {
    if (!rep || !out || cap == 0) return false;
    size_t n = 0;
    while (n < max_chars && start + n < len && rep[start + n] != 0) n++;
    if (n == 0 || n >= cap) return false;
    memcpy(out, rep + start, n);
    out[n] = 0;
    return true;
}

bool deckproto_parse_serial(const deck_model_t *m, const uint8_t *rep, size_t len, char *out, size_t cap) {
    if (m->gen == DECK_GEN2) {
        if (len < 2) return false;
        return copy_ascii(rep, len, 2, rep[1], out, cap);
    }
    return copy_ascii(rep, len, 5, 32, out, cap);
}

bool deckproto_parse_fw(const deck_model_t *m, const uint8_t *rep, size_t len, char *out, size_t cap) {
    if (m->gen == DECK_GEN2) return copy_ascii(rep, len, 6, 8, out, cap);
    return copy_ascii(rep, len, 5, 12, out, cap);
}

int deckproto_feat_brightness_of(const deck_model_t *m, const uint8_t *rep, size_t len) {
    if (!m || !rep) return -1;
    int pct = -1;
    if (m->gen == DECK_GEN2) {
        if (len >= 3 && rep[0] == 0x03 && rep[1] == 0x08) pct = rep[2];
    } else if (len >= 6 && rep[0] == 0x05 && rep[1] == 0x55 && rep[2] == 0xAA && rep[3] == 0xD1 && rep[4] == 0x01) {
        pct = rep[5];
    }
    return pct > 100 ? 100 : pct;
}
