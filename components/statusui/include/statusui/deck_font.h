#pragma once
/* The deck font: Barlow Condensed Bold (SIL Open Font License 1.1, tools/fonts/OFL.txt), printable
 * ASCII rasterised at a set of sizes with 4-bit alpha. The tables (deck_font.c) come from
 * tools/gen_deck_font.py and are committed. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUI_FONT_FIRST 0x20 /* ' ' */
#define SUI_FONT_GLYPHS 95  /* ' ' .. '~' */

typedef struct {
    uint16_t off;   /* its alpha: bw * bh 4-bit values from bits[off], row by row, the first in the high nibble */
    uint16_t adv;   /* advance, in 1/16 px */
    int8_t bx, by;  /* the ink's top left corner from the pen on the baseline (by < 0: above it) */
    uint8_t bw, bh; /* the ink's size (0 x 0: no ink, ' ') */
} sui_glyph_t;

typedef struct {
    uint8_t px;   /* the size: the font's em, in px */
    uint8_t asc;  /* the most ink above the baseline, of any glyph */
    uint8_t desc; /* the most ink below the baseline, of any glyph */
    uint8_t cap;  /* the capitals' height ('H') */
    uint8_t line; /* baseline to baseline of text lines */
    const sui_glyph_t *glyph; /* SUI_FONT_GLYPHS, from ' ' */
    const uint8_t *bits;
} sui_font_size_t;

extern const sui_font_size_t sui_font[]; /* smallest first */
extern const int sui_font_sizes;

/* Size px, or NULL when the font doesn't have it. A binary search: the sizes ascend (sui_font, smallest first;
 * test_statusui checks it). */
static inline const sui_font_size_t *sui_font_find(int px)
{
    int lo = 0, hi = sui_font_sizes - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (sui_font[mid].px == px) return &sui_font[mid];
        if (sui_font[mid].px < px) lo = mid + 1;
        else hi = mid - 1;
    }
    return (const sui_font_size_t *)0;
}

/* The glyph of ch ('?' for a char outside printable ASCII). */
static inline const sui_glyph_t *sui_font_glyph(const sui_font_size_t *f, unsigned char ch)
{
    return &f->glyph[(ch >= SUI_FONT_FIRST && ch < SUI_FONT_FIRST + SUI_FONT_GLYPHS ? ch : '?') - SUI_FONT_FIRST];
}

/* The alpha (0..15) of pixel (x, y) of glyph g's ink box. */
static inline int sui_font_alpha(const sui_font_size_t *f, const sui_glyph_t *g, int x, int y)
{
    const int k = y * g->bw + x;
    const uint8_t b = f->bits[g->off + k / 2];
    return k & 1 ? b & 15 : b >> 4;
}

#ifdef __cplusplus
}
#endif
