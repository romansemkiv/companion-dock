#include "deckproto/deckproto.h"
#include "report_hdr.h"

static int emit(deck_event_t *out, int n, int max, deck_event_t ev) {
    if (n < max) out[n] = ev;
    return n < max ? n + 1 : n;
}

static int key_edges(deck_input_state_t *st, const uint8_t *keys, int nkeys, deck_event_t *out, int n, int max) {
    for (int i = 0; i < nkeys; i++) {
        uint8_t v = keys[i] ? 1 : 0;
        if (v != st->keys[i]) {
            if (n >= max) break; /* no room: leave state so the edge is re-detected next call */
            st->keys[i] = v;
            deck_event_t e = {.kind = DECK_EV_KEY, .index = (uint8_t)i, .pressed = v};
            n = emit(out, n, max, e);
        }
    }
    return n;
}

static int num_keys(const deck_model_t *m) {
    int n = m->pedal ? 3 : m->cols * m->rows + (m->touch_keys ? 2 : 0);
    return n > 40 ? 40 : n;
}

int deckproto_parse_input(const deck_model_t *m, deck_input_state_t *st, const uint8_t *rep, size_t len,
                          deck_event_t *out, int max) {
    int n = 0;
    if (!m || !st || !rep || len < 1 || rep[0] != 0x01 || max <= 0) return 0;
    int nk = num_keys(m);

    if (m->gen != DECK_GEN2) {
        if (len < (size_t)(1 + nk)) return 0;
        return key_edges(st, rep + 1, nk, out, n, max);
    }
    if (len < 5) return 0;
    uint8_t cmd = rep[1], type = rep[4];
    switch (cmd) {
    case 0x00:
        if (len < (size_t)(4 + nk)) return 0;
        return key_edges(st, rep + 4, nk, out, n, max);
    case 0x03: {
        int ne = m->encoders > 8 ? 8 : m->encoders;
        if (ne == 0 || len < (size_t)(5 + ne)) return 0;
        for (int i = 0; i < ne; i++) {
            uint8_t b = rep[5 + i];
            if (type == 0) {
                uint8_t v = b ? 1 : 0;
                if (v != st->enc[i]) {
                    if (n >= max) break;
                    st->enc[i] = v;
                    deck_event_t e = {.kind = DECK_EV_ENC_PRESS, .index = (uint8_t)i, .pressed = v};
                    n = emit(out, n, max, e);
                }
            } else if (type == 1 && b != 0) {
                deck_event_t e = {.kind = DECK_EV_ENC_ROTATE, .index = (uint8_t)i, .delta = (int8_t)b};
                n = emit(out, n, max, e);
            }
        }
        return n;
    }
    case 0x02: {
        if (m->lcd_w == 0 || type < 1 || type > 3 || len < 10) return 0;
        deck_event_t e = {.kind = type == 1 ? DECK_EV_TOUCH_TAP : type == 2 ? DECK_EV_TOUCH_LONG : DECK_EV_TOUCH_SWIPE,
                          .x = rd16(rep + 6), .y = rd16(rep + 8)};
        if (type == 3) {
            if (len < 14) return 0;
            e.x2 = rd16(rep + 10);
            e.y2 = rd16(rep + 12);
        }
        return emit(out, n, max, e);
    }
    default:
        return 0;
    }
}
