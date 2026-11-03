/* The deck texts drawn on the keys: the deck font anti-aliased, each word in its
 * role's colour; and the screen builders. */
#include "statusui/statusui.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "statusui/deck_font.h"

#define MSG_LINES 4     /* a custom message's lines at most */
#define MSG_LINE_MAX 40

// The palette (statusui.h) as RGB888.
#define RGB(hex) {(uint8_t)((hex) >> 16), (uint8_t)((hex) >> 8), (uint8_t)(hex)}
static const uint8_t ROLE_RGB[SUI_ROLES][3] = {
    [SUI_ROLE_LABEL] = RGB(SUI_RGB_LABEL),
    [SUI_ROLE_ACCENT] = RGB(SUI_RGB_ACCENT),
    [SUI_ROLE_VALUE] = RGB(SUI_RGB_VALUE),
};
static const uint8_t STATE_RGB[][3] = {
    [SUI_STATE_NONE] = RGB(SUI_RGB_PLAIN),
    [SUI_STATE_BAD] = RGB(SUI_RGB_BAD),
    [SUI_STATE_WAIT] = RGB(SUI_RGB_WAIT),
    [SUI_STATE_OK] = RGB(SUI_RGB_OK),
};
static const uint8_t STATE_TINT[][3] = {
    [SUI_STATE_BAD] = RGB(SUI_RGB_TINT_BAD),
    [SUI_STATE_WAIT] = RGB(SUI_RGB_TINT_WAIT),
    [SUI_STATE_OK] = RGB(SUI_RGB_TINT_OK),
};
static const uint8_t DIVIDER_SERVICE[3] = RGB(SUI_RGB_DIVIDER_SERVICE);
static const uint8_t TINT_MAIN[3] = RGB(SUI_RGB_TINT_MAIN), TINT_SERVICE[3] = RGB(SUI_RGB_TINT_SERVICE);
#define N_STATES ((int)(sizeof STATE_RGB / sizeof STATE_RGB[0]))

const uint8_t *sui_role_rgb(const sui_content_t *c, int role)
{
    if (role == SUI_ROLE_STATUS) return c->state < N_STATES ? STATE_RGB[c->state] : STATE_RGB[SUI_STATE_NONE];
    return role > SUI_ROLE_FG && role < SUI_ROLES ? ROLE_RGB[role] : c->fg;
}

const uint8_t *sui_line_bg(const sui_content_t *c, int line)
{
    switch (line >= 0 && line < SUI_LINES ? c->group[line] : SUI_GROUP_NONE) {
    case SUI_GROUP_STATUS: return c->state > SUI_STATE_NONE && c->state < N_STATES ? STATE_TINT[c->state] : c->bg;
    case SUI_GROUP_MAIN: return TINT_MAIN;
    case SUI_GROUP_SERVICE: return TINT_SERVICE;
    default: return c->bg;
    }
}

const uint8_t *sui_line_divider_rgb(const sui_content_t *c, int line)
{
    (void)c;
    (void)line; /* only the service zone shares rows with another line today */
    return DIVIDER_SERVICE;
}

int sui_halo_px(int px) { return 1 + px / 12; }

// Where the word of one key sits in it (sui_layout: size, text lines): each text line centred
// across, the block from the first line's cap top to the last line's baseline centred down.
typedef struct {
    const sui_font_size_t *f;
    const char *word;
    const uint8_t *col; // its role's colour
    const uint8_t *bg;  // its line's background
    const uint8_t *div; // its divider bar's colour
    int nl;
    int inset; // px at the key's left taken by a divider bar; 0: none
    const char *text[SUI_KEY_LINES]; // each text line: its chars (of the word, or hy)
    int len[SUI_KEY_LINES];
    char hy[SUI_FIT_MAX + 2];        // a hyphenated first line: its chars and the added '-'
    int x0[SUI_KEY_LINES];   // the pen of its first char, px from the key's left edge
    int base[SUI_KEY_LINES]; // its baseline, px from the key's top
} place_t;

// The word of key (key_col, key_row) on page `page` (modulo the page count); false for a blank key.
static bool place(int cols, int rows, int key_px, const sui_content_t *c, int page, int key_col,
                  int key_row, place_t *P)
{
    sui_layout_t L;
    sui_layout(cols, rows, key_px, c, &L);
    const sui_cell_t *e = sui_layout_cell(&L, page < 0 ? 0 : page % L.pages, key_col, key_row);
    if (!e || !(P->f = sui_font_find(e->fit.px))) return false;
    P->word = c->lines[e->line] + e->start;
    P->col = sui_role_rgb(c, e->role);
    P->bg = sui_line_bg(c, e->line);
    P->div = sui_line_divider_rgb(c, e->line);
    P->nl = e->fit.nl;
    P->inset = e->divider ? sui_divider_inset(key_px) : 0;
    const int top = (key_px - ((P->nl - 1) * P->f->line + P->f->cap)) / 2;
    for (int i = 0, off = 0; i < P->nl; off += e->fit.cut[i++]) { // the text lines follow each other in the word
        P->text[i] = P->word + off;
        P->len[i] = e->fit.cut[i];
        if (i == 0 && e->fit.hyph) { /* the first line and its added '-' */
            memcpy(P->hy, P->word, (size_t)P->len[0]);
            P->hy[P->len[0]++] = '-';
            P->text[0] = P->hy;
        }
        int left;
        const int w = sui_text_width(P->f->px, P->text[i], P->len[i], &left);
        P->x0[i] = P->inset + (key_px - P->inset - w) / 2 - left;
        P->base[i] = top + P->f->cap + i * P->f->line;
    }
    return true;
}

typedef void (*plot_fn)(void *ctx, int x, int y, int alpha);

// Calls plot for every glyph pixel of the word inside the key, with its alpha (1..15). The pens are
// those sui_text_width measures: rounded to the px from the line's first char.
static void each_px(const place_t *P, int key_px, plot_fn plot, void *ctx)
{
    for (int i = 0; i < P->nl; i++) {
        uint32_t pen = 0; // 1/16 px
        for (int k = 0; k < P->len[i]; k++) {
            const sui_glyph_t *g = sui_font_glyph(P->f, (unsigned char)P->text[i][k]);
            const int gx = P->x0[i] + (int)((pen + 8) >> 4) + g->bx, gy = P->base[i] + g->by;
            for (int y = 0; y < g->bh; y++) {
                if (gy + y < 0 || gy + y >= key_px) continue;
                for (int x = 0; x < g->bw; x++) {
                    if (gx + x < 0 || gx + x >= key_px) continue;
                    const int a = sui_font_alpha(P->f, g, x, y);
                    if (a) plot(ctx, gx + x, gy + y, a);
                }
            }
            pen += g->adv;
        }
    }
}

// rgb = rgb over col at alpha/15.
static void blend(uint8_t *rgb, const uint8_t *col, int alpha)
{
    for (int k = 0; k < 3; k++) rgb[k] = (uint8_t)((rgb[k] * (15 - alpha) + col[k] * alpha + 7) / 15);
}

typedef struct {
    uint8_t *rgb;
    int key_px;
    const uint8_t *col;
} paint_t;

static void paint(void *ctx, int x, int y, int alpha)
{
    const paint_t *p = ctx;
    blend(p->rgb + 3 * ((size_t)y * p->key_px + x), p->col, alpha);
}

void sui_render_key(int cols, int rows, int key_px, const sui_content_t *c, int page,
                    int key_col, int key_row, uint8_t *rgb_out)
{
    place_t P;
    const bool word = place(cols, rows, key_px, c, page, key_col, key_row, &P);
    for (int i = 0; i < key_px * key_px; i++) memcpy(rgb_out + 3 * i, word ? P.bg : c->bg, 3);
    if (!word) return;
    paint_t p = {rgb_out, key_px, P.col};
    each_px(&P, key_px, paint, &p);
    if (P.inset) { // the divider bar of a line that shares its row
        int bx, by, bw, bh;
        sui_divider_box(key_px, &bx, &by, &bw, &bh);
        for (int y = by; y < by + bh && y < key_px; y++)
            for (int x = bx; x < bx + bw && x < key_px; x++)
                memcpy(rgb_out + 3 * ((size_t)y * key_px + x), P.div, 3);
    }
}

// ---- the overlay: a dark outline under the glyphs ----

typedef struct {
    int x0, y0, x1, y1; // the box (x1, y1 past it)
    uint8_t *a;         // alpha in the box, row by row (NULL: the box is being measured)
} cover_t;

static void cover(void *ctx, int x, int y, int alpha)
{
    cover_t *v = ctx;
    if (!v->a) {
        if (x < v->x0) v->x0 = x;
        if (y < v->y0) v->y0 = y;
        if (x >= v->x1) v->x1 = x + 1;
        if (y >= v->y1) v->y1 = y + 1;
        return;
    }
    uint8_t *o = v->a + (size_t)(y - v->y0) * (v->x1 - v->x0) + (x - v->x0);
    if (alpha > *o) *o = (uint8_t)alpha; // where glyphs overlap, the more solid one
}

// dst = src spread r px along one axis: each value the largest within r of it (n values, step apart).
static void spread(const uint8_t *src, uint8_t *dst, int n, int step, int r)
{
    for (int i = 0; i < n; i++) {
        uint8_t m = 0;
        for (int k = i - r < 0 ? 0 : i - r; k <= i + r && k < n; k++)
            if (src[k * step] > m) m = src[k * step];
        dst[i * step] = m;
    }
}

void sui_render_overlay_key(int key_px, const sui_content_t *c, uint8_t *rgb_inout)
{
    place_t P;
    if (!place(1, 1, key_px, c, 0, 0, 0, &P)) return; /* c laid out in one key: its first word */
    const int r = sui_halo_px(P.f->px);
    cover_t v = {key_px, key_px, 0, 0, NULL};
    each_px(&P, key_px, cover, &v); /* the glyphs' box */
    if (v.x1 <= v.x0) return;
    v.x0 = v.x0 - r < 0 ? 0 : v.x0 - r; /* grown by the outline, inside the key */
    v.y0 = v.y0 - r < 0 ? 0 : v.y0 - r;
    v.x1 = v.x1 + r > key_px ? key_px : v.x1 + r;
    v.y1 = v.y1 + r > key_px ? key_px : v.y1 + r;
    const int w = v.x1 - v.x0, h = v.y1 - v.y0;
    uint8_t *buf = calloc(2, (size_t)w * h); /* no memory: the glyphs without their outline */
    if (buf) {
        v.a = buf;
        each_px(&P, key_px, cover, &v);
        uint8_t *tmp = buf + (size_t)w * h;
        for (int y = 0; y < h; y++) spread(buf + (size_t)y * w, tmp + (size_t)y * w, w, 1, r);
        for (int x = 0; x < w; x++) spread(tmp + x, buf + x, h, w, r);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const int a = buf[(size_t)y * w + x];
                if (a) blend(rgb_inout + 3 * ((size_t)(v.y0 + y) * key_px + v.x0 + x), c->bg, a);
            }
        free(buf);
    }
    paint_t p = {rgb_inout, key_px, P.col};
    each_px(&P, key_px, paint, &p);
}

// ---- screen builders -------------------------------------------------------

// White on black; the words' roles say their colours.
static void init(sui_content_t *c)
{
    memset(c, 0, sizeof *c);
    memset(c->fg, 255, 3);
}

static uint8_t role_letter(char ch)
{
    switch (ch) {
    case 'S': return SUI_ROLE_STATUS;
    case 'L': return SUI_ROLE_LABEL;
    case 'A': return SUI_ROLE_ACCENT;
    case 'V': return SUI_ROLE_VALUE;
    default: return SUI_ROLE_FG;
    }
}

// Adds a line of group `group` (SUI_GROUP_*); roles: a letter per word (S status, L label, A accent,
// V value, F the content's fg), the last one also for the words after it.
static void add_line(sui_content_t *c, size_t *used, int group, const char *roles, const char *fmt, ...)
{
    if (c->n >= SUI_LINES) return;
    c->group[c->n] = (uint8_t)group;
    for (int w = 0, k = 0; w < SUI_LINE_ROLES; w++) {
        c->role[c->n][w] = role_letter(roles[k]);
        if (roles[k + 1]) k++;
    }
    size_t room = sizeof c->storage - *used;
    if (room < 1) return;
    char *dst = c->storage + *used;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(dst, room, fmt, ap);
    va_end(ap);
    if (w < 0) { dst[0] = '\0'; w = 0; }
    c->lines[c->n++] = dst;
    size_t adv = (size_t)w < room - 1 ? (size_t)w : room - 1;
    *used += adv + 1;
}

static const char *s_or_empty(const char *s) { return s ? s : ""; }



/* The status line (the top row on every page), in the screen's state. */
static void add_status(sui_content_t *c, size_t *used, int state, const char *text)
{
    c->state = (uint8_t)state;
    add_line(c, used, SUI_GROUP_STATUS, "S", "%s", text);
}





void sui_screen_reconnecting(sui_content_t *c, const char *to)
{
    size_t u = 0;
    init(c);
    add_status(c, &u, SUI_STATE_WAIT, "Reconnecting");
    if (to && to[0]) add_line(c, &u, SUI_GROUP_MAIN, "V", "%s", to);
}

void sui_screen_message(sui_content_t *c, const char *text)
{
    size_t u = 0;
    init(c);
    const char *p = s_or_empty(text);
    for (;;) {
        size_t len = strcspn(p, "\n");
        add_line(c, &u, SUI_GROUP_NONE, "F", "%.*s", (int)(len < MSG_LINE_MAX ? len : MSG_LINE_MAX), p);
        if (p[len] != '\n' || c->n >= MSG_LINES) break;
        p += len + 1;
    }
}


