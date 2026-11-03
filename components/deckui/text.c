/* Pure: the colours and the text of the maintenance UI (dui_int.h), in the deck font. */
#include <math.h>
#include <string.h>

#include "dui_int.h"
#include "statusui/deck_font.h"

uint32_t dui__role_rgb(int role)
{
    switch (role) {
    case DUI_ROLE_LABEL: return SUI_RGB_LABEL;
    case DUI_ROLE_ACCENT: return SUI_RGB_ACCENT;
    case DUI_ROLE_BAD: return SUI_RGB_BAD;
    case DUI_ROLE_WAIT: return SUI_RGB_WAIT;
    case DUI_ROLE_OK: return SUI_RGB_OK;
    default: return SUI_RGB_VALUE;
    }
}

uint32_t dui__tint_rgb(int tint)
{
    switch (tint) {
    case DUI_TINT_MAIN: return SUI_RGB_TINT_MAIN;
    case DUI_TINT_SVC: return SUI_RGB_TINT_SERVICE;
    case DUI_TINT_BAD: return SUI_RGB_TINT_BAD;
    case DUI_TINT_WAIT: return SUI_RGB_TINT_WAIT;
    case DUI_TINT_OK: return SUI_RGB_TINT_OK;
    case DUI_TINT_NEUTRAL: return DUI_RGB_TINT_NEUTRAL;
    default: return 0x000000;
    }
}

uint32_t dui__state_rgb(int state)
{
    switch (state) {
    case DUI_STATE_BAD: return SUI_RGB_BAD;
    case DUI_STATE_WAIT: return SUI_RGB_WAIT;
    case DUI_STATE_OK: return SUI_RGB_OK;
    default: return SUI_RGB_PLAIN;
    }
}

int dui__font_near(float px)
{
    int best = sui_font[0].px;
    for (int i = 0; i < sui_font_sizes; i++)
        if (fabsf(sui_font[i].px - px) < fabsf(best - px)) best = sui_font[i].px;
    return best;
}

void (*dui__text_probe)(int px, int x0, int y0, int x1, int y1);

int dui__descent(int px)
{
    const sui_font_size_t *f = sui_font_find(px);
    if (!f) return 0;
    int d = 0;
    for (const char *g = "gjpqy,"; *g; g++) {
        const sui_glyph_t *gl = sui_font_glyph(f, (unsigned char)*g);
        if (gl && gl->by + gl->bh > d) d = gl->by + gl->bh;
    }
    return d;
}

int dui__text_descent(int px, const char *s, int n)
{
    const sui_font_size_t *f = sui_font_find(px);
    if (!f) return 0;
    int d = 0;
    for (int i = 0; i < n; i++) {
        const sui_glyph_t *g = sui_font_glyph(f, (unsigned char)s[i]);
        if (g && g->bh && g->by + g->bh > d) d = g->by + g->bh;
    }
    return d;
}

int dui__cap(int px)
{
    const sui_font_size_t *f = sui_font_find(px);
    return f ? f->cap : 0;
}

/* The glyph at s[*i] (a middle dot "\xC2\xB7": the '.' raised to mid-cap, *dy px up), *i past it. */
static const sui_glyph_t *next_glyph(const sui_font_size_t *f, const char *s, int n, int *i, int *dy)
{
    *dy = 0;
    if ((unsigned char)s[*i] == 0xC2 && *i + 1 < n && (unsigned char)s[*i + 1] == 0xB7) {
        *i += 2;
        const sui_glyph_t *g = sui_font_glyph(f, '.');
        *dy = (f->cap - g->bh) / 2;
        return g;
    }
    return sui_font_glyph(f, (unsigned char)s[(*i)++]);
}

/* Spaced text (the captions' tracking): a space gets a quarter em more, so two words don't read as one with every
 * letter spread ("COMPANION IP", not "COMPANIONIP"). In 1/16 px. */
static uint32_t word_gap(int px, int spacing16) { return spacing16 ? (uint32_t)px * 4 : 0; }

int dui__text_w(int px, const char *s, int n, int spacing16, int *left)
{
    if (!spacing16 && n > 0 && !memchr(s, 0xC2, (size_t)n)) return sui_text_width(px, s, n, left); /* plain text */
    if (left) *left = 0;
    const sui_font_size_t *f = sui_font_find(px);
    if (!f || n <= 0) return 0;
    uint32_t pen = 0;
    const sui_glyph_t *first = NULL, *last = NULL;
    uint32_t last_pen = 0;
    for (int i = 0; i < n;) {
        int dy;
        const bool gap = s[i] == ' ';
        const sui_glyph_t *g = next_glyph(f, s, n, &i, &dy);
        if (!first) first = g;
        last = g;
        last_pen = pen;
        pen += g->adv + (uint32_t)spacing16 + (gap ? word_gap(px, spacing16) : 0);
    }
    if (left) *left = first->bx;
    return (int)((last_pen + 8) >> 4) + last->bx + last->bw - first->bx;
}

int dui__font_fit(const char *s, int n, int max_w, int max_px, int min_px, int spacing16)
{
    for (int i = sui_font_sizes - 1; i >= 0; i--) {
        const int px = sui_font[i].px;
        if (px > max_px || px < min_px) continue;
        if (dui__text_w(px, s, n, spacing16, NULL) <= max_w) return px;
    }
    return 0;
}

void dui__text(const dui_canvas_t *c, int px, float x, float y, const char *s, int n, uint32_t rgb, int spacing16,
               int align)
{
    const sui_font_size_t *f = sui_font_find(px);
    if (!f || n <= 0) return;
    int left;
    const int w = dui__text_w(px, s, n, spacing16, &left);
    const int x0 = (int)lroundf(align == DUI__CENTRE ? x - w / 2.0f : x) - left, base = (int)lroundf(y);
    const int col[3] = {(int)(rgb >> 16 & 255), (int)(rgb >> 8 & 255), (int)(rgb & 255)};
    uint32_t pen = 0;
    int bx0 = 1 << 20, by0 = 1 << 20, bx1 = -(1 << 20), by1 = -(1 << 20); /* the ink's box, for the probe */
    for (int i = 0; i < n;) {
        int dy;
        const bool gap = s[i] == ' ';
        const sui_glyph_t *g = next_glyph(f, s, n, &i, &dy);
        const int gx = x0 + (int)((pen + 8) >> 4) + g->bx, gy = base + g->by - dy;
        if (dui__text_probe && g->bw && g->bh) {
            if (gx < bx0) bx0 = gx;
            if (gy < by0) by0 = gy;
            if (gx + g->bw > bx1) bx1 = gx + g->bw;
            if (gy + g->bh > by1) by1 = gy + g->bh;
        }
        for (int yy = 0; yy < g->bh; yy++) {
            if (gy + yy < 0 || gy + yy >= c->h) continue;
            for (int xx = 0; xx < g->bw; xx++) {
                if (gx + xx < 0 || gx + xx >= c->w) continue;
                const int a = sui_font_alpha(f, g, xx, yy);
                if (!a) continue;
                uint8_t *o = c->rgb + 3 * ((size_t)(gy + yy) * c->w + gx + xx);
                for (int k = 0; k < 3; k++) o[k] = (uint8_t)((o[k] * (15 - a) + col[k] * a + 7) / 15);
            }
        }
        pen += g->adv + (uint32_t)spacing16 + (gap ? word_gap(px, spacing16) : 0);
    }
    if (dui__text_probe && bx1 > bx0) dui__text_probe(px, bx0, by0, bx1, by1);
}

void dui__split2(const char *s, int n, int *a, int *b)
{
    for (int i = 1; i < n; i++) /* after its own '-' */
        if (s[i - 1] == '-') {
            *a = *b = i;
            return;
        }
    for (int i = n - 1; i > 0; i--) /* before the last capital or digit after a small letter */
        if (s[i - 1] >= 'a' && s[i - 1] <= 'z' && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) {
            *a = *b = i;
            return;
        }
    for (int i = 1; i < n - 1; i++) /* at the first space */
        if (s[i] == ' ') {
            *a = i;
            *b = i + 1;
            return;
        }
    *a = *b = (n + 1) / 2;
}

int dui__join_words(const char *const *w, char *out, int n)
{
    int u = 0;
    for (int i = 0; i < DUI_STATUS_WORDS && w[i]; i++) {
        const int k = (int)strlen(w[i]);
        if (u + k + 2 > n) break;
        if (u) out[u++] = ' ';
        memcpy(out + u, w[i], (size_t)k);
        u += k;
    }
    out[u] = '\0';
    return u;
}

int dui__upper(const char *s, char *out, int max)
{
    int n = 0;
    for (; s && s[n] && n < max; n++) out[n] = (char)(s[n] >= 'a' && s[n] <= 'z' ? s[n] - 32 : s[n]);
    out[n] = '\0';
    return n;
}
