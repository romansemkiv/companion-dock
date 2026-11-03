#include "deckmodel/model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t ENC_PLUS[] = {0, 1, 2, 3};
static const uint8_t ENC_PLUS_XL[] = {0, 2, 3, 5, 6, 8};

#define BASE(p, nm, g, c, r, px, f, xf, rep, feat) \
    .pid = p, .name = nm, .gen = g, .cols = c, .rows = r, .key_px = px, .fmt = f, .key_xf = xf, \
    .report_len = rep, .feature_len = feat
#define GEN2_15(pid, nm) \
    {BASE(pid, nm, DECK_GEN2, 5, 3, 72, DECK_IMG_JPEG, IMG_XF_ROT180, 1024, 32)}
#define GEN2_32(pid, nm) \
    {BASE(pid, nm, DECK_GEN2, 8, 4, 96, DECK_IMG_JPEG, IMG_XF_ROT180, 1024, 32)}
#define MINI(pid, nm, feat) \
    {BASE(pid, nm, DECK_GEN1_MINI, 3, 2, 80, DECK_IMG_BMP, IMG_XF_MINI, 1024, feat)}

static const deck_model_t MODELS[] = {
    {BASE(0x0060, "Stream Deck", DECK_GEN1_ORIG, 5, 3, 72, DECK_IMG_BMP, IMG_XF_MIRROR_H, 8191, 17),
     .rtl = true},
    MINI(0x0063, "Stream Deck Mini", 17),
    MINI(0x0090, "Stream Deck Mini", 32),
    MINI(0x00B3, "Stream Deck Mini Discord Edition", 32),
    MINI(0x00B8, "Stream Deck Module 6", 32),
    GEN2_15(0x006D, "Stream Deck"),
    GEN2_15(0x0080, "Stream Deck MK.2"),
    GEN2_15(0x00A5, "Stream Deck MK.2"),
    GEN2_15(0x00B9, "Stream Deck Module 15"),
    GEN2_15(0x00E4, "Stream Deck Module 15"),
    GEN2_32(0x006C, "Stream Deck XL"),
    GEN2_32(0x008F, "Stream Deck XL"),
    GEN2_32(0x00BA, "Stream Deck Module 32"),
    {BASE(0x009A, "Stream Deck Neo", DECK_GEN2, 4, 2, 96, DECK_IMG_JPEG, IMG_XF_ROT180, 1024, 32),
     .info_w = 248, .info_h = 58, .lcd_xf = IMG_XF_ROT180, .touch_keys = true},
    {BASE(0x0084, "Stream Deck +", DECK_GEN2, 4, 2, 120, DECK_IMG_JPEG, IMG_XF_NONE, 1024, 32),
     .encoders = 4, .enc_cols = ENC_PLUS, .lcd_w = 800, .lcd_h = 100, .lcd_xf = IMG_XF_NONE,
     .lcd_row = 2},
    {BASE(0x00C6, "Stream Deck + XL", DECK_GEN2, 9, 4, 112, DECK_IMG_JPEG, IMG_XF_ROT90_CCW, 1024,
          32),
     .encoders = 6, .enc_cols = ENC_PLUS_XL, .lcd_w = 1200, .lcd_h = 100,
     .lcd_xf = IMG_XF_ROT90_CCW, .lcd_row = 4},
    {BASE(0x0086, "Stream Deck Pedal", DECK_GEN2, 3, 1, 0, DECK_IMG_NONE, IMG_XF_NONE, 1024, 32),
     .pedal = true},
};

const deck_model_t *deck_model_find(uint16_t vid, uint16_t pid)
{
    if (vid != DECK_VID_ELGATO) return NULL;
    for (size_t i = 0; i < sizeof(MODELS) / sizeof(MODELS[0]); i++)
        if (MODELS[i].pid == pid) return &MODELS[i];
    return NULL;
}

static int push(deck_ctl_t *out, int max, int *n, deck_ctl_kind_t kind, uint8_t row, uint8_t col,
                uint8_t hw, uint16_t x, uint16_t w, uint16_t h)
{
    if (*n >= max) return -1;
    out[*n] = (deck_ctl_t){kind, row, col, hw, x, w, h};
    (*n)++;
    return 0;
}

int deck_model_controls(const deck_model_t *m, deck_ctl_t *out, int max)
{
    int n = 0;
    deck_ctl_kind_t kk = m->pedal ? CTL_KEY_NONE : CTL_KEY_LCD;
    for (int r = 0; r < m->rows; r++) {
        for (int c = 0; c < m->cols; c++) {
            uint8_t hw = (uint8_t)(r * m->cols + (m->rtl ? m->cols - 1 - c : c));
            if (push(out, max, &n, kk, (uint8_t)r, (uint8_t)c, hw, 0, m->key_px, m->key_px)) return -1;
        }
    }
    if (m->touch_keys) {
        if (push(out, max, &n, CTL_KEY_RGB, m->rows, 0, 8, 0, 0, 0)) return -1;
        if (push(out, max, &n, CTL_KEY_RGB, m->rows, 3, 9, 0, 0, 0)) return -1;
        if (push(out, max, &n, CTL_INFO, m->rows, 1, 0, 0, m->info_w, m->info_h)) return -1;
    }
    if (m->lcd_w) {
        uint16_t cell = (uint16_t)(m->lcd_w / m->encoders);
        for (int i = 0; i < m->encoders; i++)
            if (push(out, max, &n, CTL_LCD_CELL, m->lcd_row, m->enc_cols[i], (uint8_t)i,
                     (uint16_t)(cell * i), cell, m->lcd_h))
                return -1;
        for (int i = 0; i < m->encoders; i++)
            if (push(out, max, &n, CTL_ENCODER, (uint8_t)(m->lcd_row + 1), m->enc_cols[i],
                     (uint8_t)i, 0, 0, 0))
                return -1;
    }
    return n;
}

void deck_ctl_id(const deck_ctl_t *c, char out[8])
{
    snprintf(out, 8, "%u/%u", (unsigned)c->row, (unsigned)c->col);
}

const deck_ctl_t *deck_ctl_find(const deck_ctl_t *list, int n, const char *id)
{
    char buf[8];
    for (int i = 0; i < n; i++) {
        deck_ctl_id(&list[i], buf);
        if (strcmp(buf, id) == 0) return &list[i];
    }
    return NULL;
}

void deck_lcd_cell_region(const deck_model_t *m, const deck_ctl_t *c, uint16_t *x, uint16_t *y, uint16_t *w,
                          uint16_t *h)
{
    (void)m; /* every strip so far: Plus and Plus XL alike */
    *x = c->x;
    *y = 0;
    *w = c->w;
    *h = c->h;
}
