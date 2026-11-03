#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "imgproc/xform.h"

#define DECK_VID_ELGATO 0x0FD9

typedef enum { DECK_GEN1_ORIG, DECK_GEN1_MINI, DECK_GEN2 } deck_gen_t;
typedef enum { DECK_IMG_NONE, DECK_IMG_BMP, DECK_IMG_JPEG } deck_imgfmt_t;

typedef struct {
    uint16_t pid;
    const char *name;
    deck_gen_t gen;
    uint8_t cols, rows;
    uint16_t key_px;
    deck_imgfmt_t fmt;
    img_xform_t key_xf;
    uint16_t report_len, feature_len;
    uint8_t encoders;
    const uint8_t *enc_cols;
    uint16_t lcd_w, lcd_h;
    img_xform_t lcd_xf; /* transform for the non-key display: Plus/Plus XL strip, Neo info bar */
    uint8_t lcd_row;
    uint16_t info_w, info_h;
    bool touch_keys;
    bool pedal;
    bool rtl;
} deck_model_t;

typedef enum {
    CTL_KEY_LCD,
    CTL_KEY_RGB,
    CTL_KEY_NONE,
    CTL_ENCODER,
    CTL_LCD_CELL,
    CTL_INFO,
} deck_ctl_kind_t;

typedef struct {
    deck_ctl_kind_t kind;
    uint8_t row, col, hw;
    uint16_t x, w, h;
} deck_ctl_t;

/* vid must be 0x0FD9. Returns NULL for unknown devices. */
const deck_model_t *deck_model_find(uint16_t vid, uint16_t pid);

/* m has a display the box draws on (its keys, strip or info bar): not the Pedal, and an image format. */
static inline bool deck_model_has_display(const deck_model_t *m) { return !m->pedal && m->fmt != DECK_IMG_NONE; }

/* Fills out[] with the model's controls; returns the count, or -1 if max is too small. */
int deck_model_controls(const deck_model_t *m, deck_ctl_t *out, int max);

/* Writes "row/col" (NUL-terminated) to out. */
void deck_ctl_id(const deck_ctl_t *c, char out[8]);

const deck_ctl_t *deck_ctl_find(const deck_ctl_t *list, int n, const char *id);

/* The region an LCD strip cell (CTL_LCD_CELL) is written to, as an LCD image report's header
 * carries it: the strip's own coordinates, before any turn. The host library writes it so
 * (@elgato-stream-deck/core 7.8.0 lcdSegmentDisplay/generic.js) and turns only the image (the Plus
 * XL: lcd_xf, as its keys). */
void deck_lcd_cell_region(const deck_model_t *m, const deck_ctl_t *c, uint16_t *x, uint16_t *y, uint16_t *w,
                          uint16_t *h);
