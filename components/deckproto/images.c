#include <string.h>
#include "deckproto/deckproto.h"
#include "report_hdr.h"

static void w_reset(deck_img_writer_t *w, const deck_model_t *m, const uint8_t *img, size_t len) {
    memset(w, 0, sizeof *w);
    w->m = m;
    w->mode = DECK_W_NONE;
    w->img = img;
    w->len = img ? len : 0;
}

void deckproto_img_key(deck_img_writer_t *w, const deck_model_t *m, uint8_t hw_key, const uint8_t *img, size_t len) {
    w_reset(w, m, img, len);
    w->key = hw_key;
    if (m->gen == DECK_GEN2) {
        w->mode = DECK_W_GEN2_KEY;
    } else {
        w->mode = DECK_W_GEN1;
        if (m->gen == DECK_GEN1_ORIG) {
            w->half = (w->len + 1) / 2;
            w->page = 1;
        }
    }
}

void deckproto_img_lcd(deck_img_writer_t *w, const deck_model_t *m, uint16_t x, uint16_t y, uint16_t width,
                       uint16_t height, const uint8_t *jpeg, size_t len) {
    w_reset(w, m, jpeg, len);
    if (m->gen != DECK_GEN2 || m->lcd_w == 0)
        return;
    w->mode = DECK_W_GEN2_LCD;
    w->x = x;
    w->y = y;
    w->w = width;
    w->h = height;
}

void deckproto_img_info(deck_img_writer_t *w, const deck_model_t *m, const uint8_t *jpeg, size_t len) {
    w_reset(w, m, jpeg, len);
    if (m->gen != DECK_GEN2 || m->info_w == 0)
        return;
    w->mode = DECK_W_NEO_INFO;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

bool deckproto_img_next(deck_img_writer_t *w, uint8_t *r) {
    if (w->mode == DECK_W_NONE || w->off >= w->len)
        return false;
    const size_t rl = w->m->report_len;
    memset(r, 0, rl);
    size_t hdr, remain = w->len - w->off, n;
    bool last;

    switch (w->mode) {
    case DECK_W_GEN2_KEY:
    case DECK_W_NEO_INFO:
        hdr = GEN2_KEY_HDR;
        n = rl - hdr;
        if (n > remain) n = remain;
        last = (n == remain);
        r[0] = 0x02;
        r[1] = (w->mode == DECK_W_GEN2_KEY) ? 0x07 : 0x0B;
        r[2] = (w->mode == DECK_W_GEN2_KEY) ? w->key : 0x00;
        r[3] = last;
        put16(r + 4, (uint16_t)n);
        put16(r + 6, w->page);
        break;
    case DECK_W_GEN2_LCD:
        hdr = GEN2_LCD_HDR;
        n = rl - hdr;
        if (n > remain) n = remain;
        last = (n == remain);
        r[0] = 0x02;
        r[1] = 0x0C;
        put16(r + 2, w->x);
        put16(r + 4, w->y);
        put16(r + 6, w->w);
        put16(r + 8, w->h);
        r[10] = last;
        put16(r + 11, w->page);
        put16(r + 13, (uint16_t)n);
        break;
    case DECK_W_GEN1:
        hdr = GEN1_HDR;
        n = rl - hdr;
        if (w->half) { /* Original v1: two pages, split at the half */
            size_t page_left = (w->page == 1) ? w->half - w->off : remain;
            if (n > page_left) n = page_left;
        } else if (n > remain) {
            n = remain;
        }
        last = (n == remain);
        r[0] = 0x02;
        r[1] = 0x01;
        r[2] = (uint8_t)w->page;
        r[3] = 0x00;
        r[4] = last;
        r[5] = (uint8_t)(w->key + 1);
        break;
    default:
        return false;
    }

    memcpy(r + hdr, w->img + w->off, n);
    w->off += n;
    if (!w->half)
        w->page++;
    else if (w->page == 1 && w->off >= w->half)
        w->page = 2; /* Original v1: page 1 ends exactly at the half */
    return true;
}
