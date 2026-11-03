/* Pure: the word-per-key layout of the box's deck texts and its paging (statusui.h). */
#include "statusui/statusui.h"

#include <assert.h>
#include <string.h>

#include "statusui/deck_font.h"

#define MAX_LINES SUI_LINES
#define LINE_MAX 255  /* chars of a line looked at (sui_cell_t.start is 8 bits) */

/* An IPv4 address: four numbers of 1-3 digits, each at most 255, joined by dots. Fills the octets'
 * offsets in w and lengths. */
static bool ipv4(const char *w, int len, int start[4], int n[4])
{
    int i = 0;
    for (int k = 0; k < 4; k++) {
        int j = i, v = 0;
        while (j < len && j - i < 3 && w[j] >= '0' && w[j] <= '9') v = v * 10 + (w[j++] - '0');
        if (j == i || v > 255) return false;
        start[k] = i;
        n[k] = j - i;
        if (k == 3) return j == len;
        if (j == len || w[j] != '.') return false;
        i = j + 1;
    }
    return false;
}

/* ---- the fit: a word in its key ---- */

/* A word at one size: the pens of its chars, in 1/16 px from its first (pen[k]: char k). Widths come
 * from these as sui_render_key places the glyphs: pen rounded to the px, plus the glyph's bearing. */
typedef struct {
    const sui_font_size_t *f;
    const char *s;
    int len;
    int inset; /* px of the key's left edge the text may not use (a divider bar) */
    uint32_t pen[SUI_FIT_MAX + 1];
} word_t;

static void word_at(word_t *w, const sui_font_size_t *f)
{
    w->f = f;
    w->pen[0] = 0;
    for (int k = 0; k < w->len; k++) w->pen[k + 1] = w->pen[k] + sui_font_glyph(f, (unsigned char)w->s[k])->adv;
}

/* Ink width of chars [i, j) on one line (j > i): from the first's ink to the last's. */
static int width(const word_t *w, int i, int j)
{
    const sui_glyph_t *a = sui_font_glyph(w->f, (unsigned char)w->s[i]), *z = sui_font_glyph(w->f, (unsigned char)w->s[j - 1]);
    return (int)((w->pen[j - 1] - w->pen[i] + 8) >> 4) + z->bx + z->bw - a->bx;
}

int sui_text_width(int px, const char *s, int n, int *left)
{
    if (left) *left = 0;
    const sui_font_size_t *f = sui_font_find(px);
    if (!f || n <= 0) return 0;
    uint32_t pen = 0;
    for (int k = 0; k < n - 1; k++) pen += sui_font_glyph(f, (unsigned char)s[k])->adv;
    const sui_glyph_t *a = sui_font_glyph(f, (unsigned char)s[0]), *z = sui_font_glyph(f, (unsigned char)s[n - 1]);
    if (left) *left = a->bx;
    return (int)((pen + 8) >> 4) + z->bx + z->bw - a->bx;
}

/* nl text lines at size f fit a key of key_px top to bottom: the block from the first line's cap top to
 * the last line's baseline centred (as sui_render_key draws it), the font's tallest and deepest glyphs
 * SUI_MARGIN inside the edges. */
static bool fits_tall(const sui_font_size_t *f, int nl, int key_px)
{
    const int h = (nl - 1) * f->line + f->cap, top = (key_px - h) / 2;
    return top + f->cap - f->asc >= SUI_MARGIN && top + h + f->desc <= key_px - SUI_MARGIN;
}

/* Ink width of chars [0, a) and an added '-' on one line (a > 0). */
static int width_hy(const word_t *w, int a)
{
    const sui_glyph_t *f0 = sui_font_glyph(w->f, (unsigned char)w->s[0]), *h = sui_font_glyph(w->f, '-');
    return (int)((w->pen[a] + 8) >> 4) + h->bx + h->bw - f0->bx;
}

/* The largest size, capped and at most max_px, at which the whole word fits on one line; 0: none. */
static int one_line_px(word_t *w, int key_px, int max_px)
{
    for (int i = sui_font_sizes - 1; i >= 0; i--) {
        const sui_font_size_t *f = &sui_font[i];
        if (f->px * 100 > key_px * SUI_CAP_PCT || f->px > max_px || !fits_tall(f, 1, key_px)) continue;
        word_at(w, f);
        if (width(w, 0, w->len) <= key_px - 2 * SUI_MARGIN - w->inset) return f->px;
    }
    return 0;
}

/* The word's own break after chars [0, cut) of s: after a '-' or '.'. */
static bool own_break(const char *s, int cut) { return s[cut - 1] == '-' || s[cut - 1] == '.'; }

/* Between a small letter and a capital ("Companion|Box"): the next best place to break a word. */
static bool case_break(const char *s, int cut)
{
    return s[cut - 1] >= 'a' && s[cut - 1] <= 'z' && s[cut] >= 'A' && s[cut] <= 'Z';
}

static bool letter(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

/* An added '-' may go between two letters, leaving at least `least` chars a side. */
static bool hyph_break(const char *s, int len, int cut, int least)
{
    return cut >= least && len - cut >= least && letter(s[cut - 1]) && letter(s[cut]);
}

enum { ANY_BREAK, OWN_BREAK, CASE_BREAK, HYPH3_BREAK, HYPH1_BREAK };

/* The word in 2 lines at the largest size, capped, at which a split fits (all kinds but ANY_BREAK: only
 * splits there, and only down to SUI_MIN_PX; the HYPH kinds with an added '-' on the first line): of
 * the splits at that size, the narrowest widest line, then the widest narrowest line, then the longer
 * first line. False: none. */
static bool two_lines(word_t *w, int key_px, int at, sui_fit_t *r)
{
    const int inner = key_px - 2 * SUI_MARGIN - w->inset, len = w->len;
    for (int i = sui_font_sizes - 1; i >= 0; i--) {
        const sui_font_size_t *f = &sui_font[i];
        if (f->px * 100 > key_px * SUI_CAP_PCT || !fits_tall(f, 2, key_px)) continue;
        if (at != ANY_BREAK && f->px < SUI_MIN_PX) return false;
        word_at(w, f);
        int best = 0, best_hi = 0, best_lo = 0;
        const bool hy = at == HYPH3_BREAK || at == HYPH1_BREAK;
        for (int a = 1; a < len; a++) {
            if ((at == OWN_BREAK && !own_break(w->s, a)) || (at == CASE_BREAK && !case_break(w->s, a)) ||
                (hy && !hyph_break(w->s, len, a, at == HYPH3_BREAK ? 3 : 1)))
                continue;
            const int wa = hy ? width_hy(w, a) : width(w, 0, a), wb = width(w, a, len);
            if (wa > inner) break; /* longer first lines are wider still */
            if (wb > inner) continue;
            const int hi = wa > wb ? wa : wb, lo = wa < wb ? wa : wb;
            if (!best || hi < best_hi || (hi == best_hi && lo >= best_lo)) {
                best = a;
                best_hi = hi;
                best_lo = lo;
            }
        }
        if (best) {
            r->px = f->px;
            r->nl = 2;
            r->cut[0] = (uint8_t)best;
            r->cut[1] = (uint8_t)(len - best);
            r->hyph = hy;
            return true;
        }
    }
    return false;
}

sui_fit_t sui_fit(const char *s, int len, int key_px, bool hyphen) { return sui_fit_inset(s, len, key_px, hyphen, 0); }

sui_fit_t sui_fit_inset(const char *s, int len, int key_px, bool hyphen, int inset)
{
    sui_fit_t one = {0, 1, {(uint8_t)len, 0}, 0}, two = {0, 2, {0}, 0};
    if (len <= 0 || len > SUI_FIT_MAX) return (sui_fit_t){0, 1, {0}, 0};
    static_assert(SUI_KEY_LINES == 2, "a word takes one text line or two");
    word_t w = {.s = s, .len = len, .inset = inset};
    one.px = (uint8_t)one_line_px(&w, key_px, 255);
    if (one.px >= SUI_MIN_PX) return one; /* readable on one line */
    /* after its own '-' or '.' when that reads; else a word hyphenated, a value broken before
     * a capital; else even */
    const bool found = len >= 2 && (two_lines(&w, key_px, OWN_BREAK, &two) ||
                                    (hyphen ? two_lines(&w, key_px, HYPH3_BREAK, &two) || two_lines(&w, key_px, HYPH1_BREAK, &two)
                                            : two_lines(&w, key_px, CASE_BREAK, &two)) ||
                                    two_lines(&w, key_px, ANY_BREAK, &two));
    if (found && two.px >= SUI_MIN_PX) return two;
    /* nothing readable (a key too small, or a word too long for one key): the larger */
    return two.px > one.px ? two : one;
}

/* How far a piece of the word starting at char i reaches (chars [i, reach)) while it still reads in one
 * key: SUI_MIN_PX in at most SUI_KEY_LINES lines, each as long as fits. At least one char. */
static int reach(const char *s, int len, int i, int key_px)
{
    const sui_font_size_t *f = sui_font_find(SUI_MIN_PX);
    const int inner = key_px - 2 * SUI_MARGIN;
    if (!f || f->px * 100 > key_px * SUI_CAP_PCT) return i + 1;
    int end = i;
    for (int nl = 1; nl <= SUI_KEY_LINES && end < len && fits_tall(f, nl, key_px); nl++) {
        const sui_glyph_t *a = sui_font_glyph(f, (unsigned char)s[end]);
        uint32_t pen = 0; /* the pen of char j, 1/16 px from the line's first */
        int j = end;
        while (j < len) {
            const sui_glyph_t *z = sui_font_glyph(f, (unsigned char)s[j]);
            if ((int)((pen + 8) >> 4) + z->bx + z->bw - a->bx > inner) break;
            pen += z->adv;
            j++;
        }
        if (j == end) break; /* not even one char */
        end = j;
    }
    return end > i ? end : i + 1;
}

/* Cells being added: n of max in cell[] (a layout's, or the caller's array of sui_word_cells). */
typedef struct {
    sui_cell_t *cell;
    int n, max;
} cells_t;

/* A cell for chars [start, start + len) of line `line`, its fit not set; NULL: L->max reached. */
static sui_cell_t *new_cell(cells_t *L, int line, int start, int len, int role)
{
    if (L->n >= L->max) return NULL;
    sui_cell_t *e = &L->cell[L->n++];
    memset(e, 0, sizeof *e);
    e->line = (uint8_t)line;
    e->start = (uint8_t)start;
    e->len = (uint8_t)len;
    e->role = (uint8_t)role;
    return e;
}

static sui_cell_t *add_cell(cells_t *L, int line, const char *s, int start, int len, int key_px, int role)
{
    sui_cell_t *e = new_cell(L, line, start, len, role);
    if (e) e->fit = sui_fit(s + start, len, key_px, role == SUI_ROLE_STATUS || role == SUI_ROLE_LABEL);
    return e;
}

/* One size per role in a line: of cells [a, b) of line s, those of one role readable on one
 * line all take the smallest of their sizes (each the largest size up to that at which it fits, which
 * is that size: a word that fits at a size fits at a smaller one). Words of another role, and words on
 * two lines, keep theirs: "WiFi" isn't pulled down by the SSID, nor "port" by the octets. */
static void even_line(sui_layout_t *L, const char *s, int a, int b, int key_px)
{
    for (int role = 0; role < SUI_ROLES; role++) {
        int least = 0;
        for (int i = a; i < b; i++) {
            const sui_fit_t *f = &L->cell[i].fit;
            if (L->cell[i].role == role && f->nl == 1 && f->px >= SUI_MIN_PX && (!least || f->px < least)) least = f->px;
        }
        for (int i = a; i < b; i++) {
            sui_cell_t *e = &L->cell[i];
            if (e->role != role || e->fit.nl != 1 || e->fit.px < SUI_MIN_PX || e->fit.px == least) continue;
            word_t w = {.s = s + e->start, .len = e->len};
            const int px = one_line_px(&w, key_px, least);
            if (px) e->fit.px = (uint8_t)px;
        }
    }
}

/* A word as cells: one when it reads in one key (sui_fit), else the fewest pieces that each read, on
 * the keys after it, in order (never cut short, nothing added); of those, the most ending after
 * the word's own '-' or '.', then the most even (the shortest longest piece; then the earlier
 * pieces the longer). A word longer than SUI_FIT_MAX (no screen has one) goes in pieces each as long as
 * reads. */
static void add_word(cells_t *L, int line, const char *s, int start, int len, int key_px, int role)
{
    const char *w = s + start;
    if (len > SUI_FIT_MAX) {
        for (int i = 0, k = 0; i < len; k++) {
            const int to = reach(w, len, i, key_px);
            sui_cell_t *e = add_cell(L, line, s, start + i, to - i, key_px, role);
            if (!e) return;
            e->piece = (uint8_t)k;
            i = to;
        }
        return;
    }
    /* best[j]: the best way to cut chars [0, j) into pieces: their count, own breaks, longest, the last
     * piece's start */
    static_assert(SUI_FIT_MAX < 256, "best[] and cut[] keep char counts and offsets in 8 bits");
    struct { uint8_t n, own, most, from; } best[SUI_FIT_MAX + 1];
    best[0].n = 0;
    best[0].own = best[0].most = 0;
    for (int j = 1; j <= len; j++) best[j].n = 0xFF;
    for (int i = 0; i < len; i++) {
        if (best[i].n == 0xFF) continue;
        const int to = reach(w, len, i, key_px);
        for (int j = i + 1; j <= to; j++) {
            const int n = best[i].n + 1, own = best[i].own + (j < len && own_break(w, j));
            const int most = best[i].most > j - i ? best[i].most : j - i;
            if (n < best[j].n || (n == best[j].n && (own > best[j].own || (own == best[j].own && most <= best[j].most)))) {
                best[j].n = (uint8_t)n;
                best[j].own = (uint8_t)own;
                best[j].most = (uint8_t)most;
                best[j].from = (uint8_t)i;
            }
        }
    }
    uint8_t cut[SUI_FIT_MAX + 1];
    int n = 0; /* the pieces' starts, last first */
    for (int j = len; j > 0; j = best[j].from) cut[n++] = best[j].from;
    for (int k = n - 1; k >= 0; k--) {
        const int end = k ? cut[k - 1] : len;
        sui_cell_t *e = add_cell(L, line, s, start + cut[k], end - cut[k], key_px, role);
        if (!e) return;
        e->piece = (uint8_t)(n - 1 - k);
    }
}

/* ---- the divider ---- */

void sui_divider_box(int key_px, int *x, int *y, int *w, int *h)
{
    const int bw = (key_px * 4 + 48) / 96, bh = (key_px * 60 + 50) / 100;
    if (x) *x = (key_px * 3 + 48) / 96;
    if (y) *y = (key_px - bh) / 2;
    if (w) *w = bw < 2 ? 2 : bw;
    if (h) *h = bh;
}

int sui_divider_inset(int key_px)
{
    int x, w;
    sui_divider_box(key_px, &x, NULL, &w, NULL);
    return x + w;
}

/* ---- values over keys ---- */

#define VALUE_MAX 64 /* chars of a value that goes over keys (a 64-char password); longer: a word */

int sui_comfort_px(int key_px)
{
    const int c = (key_px * 25 + 50) / 100;
    return c > 18 ? c : 18;
}

static bool digit(char ch) { return ch >= '0' && ch <= '9'; }

/* The kind of the break before char a of s: 3 before a capital that follows a small letter, 2 after a
 * '-' or '.', 1 between a letter and a digit, 0 none (inside a part). */
static int cut_kind(const char *s, int a)
{
    const char p = s[a - 1], q = s[a];
    if (p >= 'a' && p <= 'z' && q >= 'A' && q <= 'Z') return 3;
    if (p == '-' || p == '.') return 2;
    if ((letter(p) && digit(q)) || (digit(p) && letter(q))) return 1;
    return 0;
}

/* A split of a value into pieces at one size, as good as it gets: the fewest cuts inside parts that read
 * on their own, the fewest keys, the most breaks of each kind (best first), the narrowest widest piece. */
typedef struct {
    uint8_t prot, keys, k3, k2, k1, hi, from;
} vsplit_t;

static bool vbetter(const vsplit_t *a, const vsplit_t *b)
{
    if (a->prot != b->prot) return a->prot < b->prot;
    if (a->keys != b->keys) return a->keys < b->keys;
    if (a->k3 != b->k3) return a->k3 > b->k3;
    if (a->k2 != b->k2) return a->k2 > b->k2;
    if (a->k1 != b->k1) return a->k1 > b->k1;
    return a->hi < b->hi;
}

/* The best split of the value (w, at its size) into 2 or more pieces of one line each, no wider than
 * inner (d[j]: the best for chars [0, j)); false: none. prot[a]: a cut before char a is inside a part
 * that reads on its own. */
static bool vsplit(const word_t *w, int inner, const uint8_t *kind, const uint8_t *prot, vsplit_t *d)
{
    const int len = w->len;
    d[0] = (vsplit_t){0, 0, 0, 0, 0, 0, 0};
    for (int j = 1; j <= len; j++) d[j].keys = 0xFF;
    for (int i = 0; i < len; i++) {
        if (d[i].keys == 0xFF) continue;
        for (int j = i + 1; j <= len; j++) {
            if (i == 0 && j == len) break; /* at least two pieces */
            const int wd = width(w, i, j);
            if (wd > inner) break; /* longer pieces are wider still */
            vsplit_t t = d[i];
            t.keys++;
            if (i > 0) {
                t.prot += prot[i];
                t.k3 += kind[i] == 3;
                t.k2 += kind[i] == 2;
                t.k1 += kind[i] == 1;
            }
            if (wd > t.hi) t.hi = (uint8_t)wd;
            t.from = (uint8_t)i;
            if (d[j].keys == 0xFF || vbetter(&t, &d[j])) d[j] = t;
        }
    }
    return d[len].keys != 0xFF;
}

/* A value as cells: over consecutive keys, one line each, all at the smallest of the pieces' own
 * sizes, when it doesn't reach the comfortable size on one line; false (nothing added) when it does, or
 * is too long or too short to split, or no split reads: then it is a word (add_word). Of the sizes at which a split has the fewest cuts inside parts that read on their own: the
 * comfortable ones by the fewest keys, then the better breaks, then the larger size; with none
 * comfortable, the largest. */
static bool add_value(cells_t *L, int line, const char *s, int start, int len, int key_px, int role)
{
    const char *v = s + start;
    const int inner = key_px - 2 * SUI_MARGIN, comfort = sui_comfort_px(key_px);
    word_t w = {.s = v, .len = len};
    if (len < 2 || len > VALUE_MAX || one_line_px(&w, key_px, 255) >= comfort) return false; /* a word */
    uint8_t kind[VALUE_MAX + 1] = {0}, prot[VALUE_MAX + 1] = {0};
    const sui_font_size_t *mf = sui_font_find(SUI_MIN_PX);
    if (mf) word_at(&w, mf);
    for (int a = 1, part = 0; a <= len; a++) { /* the parts between natural breaks: a cut inside one that reads */
        if (a < len && !(kind[a] = (uint8_t)cut_kind(v, a))) continue;
        const bool reads = mf && width(&w, part, a) <= inner;
        for (int k = part + 1; k < a; k++) prot[k] = reads;
        part = a;
    }
    vsplit_t d[VALUE_MAX + 1], best = {0xFF, 0xFF, 0, 0, 0, 0xFF, 0};
    int best_px = 0, least_prot = 0xFF;
    for (int pass = 0; pass < 2; pass++) { /* 0: the fewest protected cuts any size reaches; 1: choose */
        for (int i = sui_font_sizes - 1; i >= 0; i--) {
            const sui_font_size_t *f = &sui_font[i];
            if (f->px < SUI_MIN_PX || f->px * 100 > key_px * SUI_CAP_PCT || !fits_tall(f, 1, key_px)) continue;
            word_at(&w, f);
            if (!vsplit(&w, inner, kind, prot, d)) continue;
            const vsplit_t *t = &d[len];
            if (pass == 0) {
                if (t->prot < least_prot) least_prot = t->prot;
                continue;
            }
            if (t->prot != least_prot) continue;
            const bool ok = f->px >= comfort, had = best_px >= comfort;
            bool take;
            if (!best_px) take = true;
            else if (ok != had) take = ok;
            else if (!ok) take = false; /* neither comfortable: the larger size, met first */
            else if (t->keys != best.keys) take = t->keys < best.keys;
            else if (t->k3 != best.k3) take = t->k3 > best.k3;
            else if (t->k2 != best.k2) take = t->k2 > best.k2;
            else if (t->k1 != best.k1) take = t->k1 > best.k1;
            else take = false; /* the larger size, met first */
            if (take) {
                best = *t;
                best_px = f->px;
            }
        }
    }
    if (!best_px) return false; /* no split reads (a key too small): a word */
    word_at(&w, sui_font_find(best_px));
    vsplit(&w, inner, kind, prot, d);
    uint8_t cut[VALUE_MAX + 1];
    int n = 0, px = 0;
    for (int j = len; j > 0; j = d[j].from) cut[n++] = d[j].from; /* the pieces' starts, last first */
    for (int k = n - 1; k >= 0; k--) { /* one size: the smallest of the pieces' own */
        w.s = v + cut[k]; /* w's pens are done with: it measures the pieces now */
        w.len = (k ? cut[k - 1] : len) - cut[k];
        const int own = one_line_px(&w, key_px, 255);
        if (!px || own < px) px = own;
    }
    for (int k = n - 1; k >= 0; k--) {
        const int end = k ? cut[k - 1] : len;
        sui_cell_t *e = new_cell(L, line, start + cut[k], end - cut[k], role); /* its fit is set here */
        if (!e) return true;
        e->piece = (uint8_t)(n - 1 - k);
        e->fit = (sui_fit_t){(uint8_t)px, 1, {(uint8_t)(end - cut[k]), 0}, 0};
    }
    return true;
}

int sui_word_cells(const char *s, int len, int key_px, int role, sui_cell_t *out, int max)
{
    cells_t L = {out, 0, max}; /* straight into the caller's array: no scratch layout on the stack */
    if (!s || len <= 0 || max <= 0) return 0;
    if (len > LINE_MAX) len = LINE_MAX; /* sui_cell_t.start and .len are 8 bits */
    if (!(role == SUI_ROLE_VALUE && add_value(&L, 0, s, 0, len, key_px, role))) add_word(&L, 0, s, 0, len, key_px, role);
    return L.n;
}

/* The role of word `word` of line `line` (the words past the last role take the last). */
static int role_of(const sui_content_t *c, int line, int word)
{
    return c->role[line][word < SUI_LINE_ROLES ? word : SUI_LINE_ROLES - 1];
}

/* The words of line `line` as cells, not placed yet: split on spaces, an IPv4 address in octets (in
 * accent where it stands for a value: a host). */
static void add_words(sui_layout_t *Lay, const sui_content_t *c, int line, int key_px)
{
    cells_t cells = {Lay->cell, Lay->n, SUI_CELLS_MAX}, *L = &cells;
    const char *s = c->lines[line];
    int len = 0;
    while (s && len < LINE_MAX && s[len]) len++;
    int prev = -1; /* the previous word's role */
    for (int i = 0, word = 0; i < len; word++) {
        while (i < len && s[i] == ' ') i++;
        int j = i;
        while (j < len && s[j] != ' ') j++;
        int start[4], n[4];
        const int role = role_of(c, line, word), first = L->n;
        if (j > i && ipv4(s + i, j - i, start, n)) { /* its octets, a group */
            for (int k = 0; k < 4; k++) {
                sui_cell_t *e = add_cell(L, line, s, i + start[k], n[k], key_px, role == SUI_ROLE_VALUE ? SUI_ROLE_ACCENT : role);
                if (e) {
                    e->piece = (uint8_t)k;
                    e->even = 1;
                }
            }
        } else if (j > i && !(role == SUI_ROLE_VALUE && add_value(L, line, s, i, j - i, key_px, role))) {
            add_word(L, line, s, i, j - i, key_px, role);
            if (prev == SUI_ROLE_LABEL && role == SUI_ROLE_ACCENT && L->n == first + 1)
                L->cell[first].piece = 1; /* a label's number goes with it: "port 5343" */
        }
        if (j > i) prev = role;
        i = j;
    }
    Lay->n = cells.n;
}

/* The keys of cells [a, b) (one line, or lines that run on) from key `pos` on, in reading order (at[k]:
 * cell k's, may be NULL): a word's pieces that don't fit in what is left of a row start the next one
 *. Returns the key after the last. */
static int flow(const sui_layout_t *L, int a, int b, int cols, int pos, uint16_t *at)
{
    for (int k = a; k < b; k++) {
        if (L->cell[k].piece != 0) { /* in a group placed below: sequential */
            if (at) at[k] = (uint16_t)pos;
            pos++;
            continue;
        }
        int n = 1;
        while (k + n < b && L->cell[k + n].piece) n++;
        if (n > 1 && n <= cols && pos % cols && pos % cols + n > cols) pos += cols - pos % cols; /* the next row */
        if (n > cols && L->cell[k].even) { /* from a new row, in balanced rows */
            if (pos % cols) pos += cols - pos % cols;
            const int nr = (n + cols - 1) / cols;
            for (int r = 0, j = 0; r < nr; r++) {
                const int m = n / nr + (r < n % nr);
                for (int q = 0; q < m; q++, j++)
                    if (at) at[k + j] = (uint16_t)(pos + r * cols + q);
                if (r == nr - 1) pos += r * cols + m;
            }
            k += n - 1;
            continue;
        }
        if (at) at[k] = (uint16_t)pos;
        pos++;
    }
    return pos;
}

/* Rows the cells [a, b) of one line take (1 for an empty line). */
static int line_rows(const sui_layout_t *L, int a, int b, int cols)
{
    return a == b ? 1 : (flow(L, a, b, cols, 0, NULL) + cols - 1) / cols;
}

/* ---- zones ---- */

/* Puts cell k on key (page, row, col of at). */
static void put_at(sui_layout_t *L, int k, int page, int row, int col)
{
    L->cell[k].page = (uint8_t)page;
    L->cell[k].row = (uint8_t)row;
    L->cell[k].col = (uint8_t)col;
}

/* Lays a status screen out in zones (statusui.h): the status lines on every page's top rows, the main
 * lines under them, the service lines packed at the bottom, spacer rows between. False (nothing done)
 * when the status leaves no row (a deck of one row: the overlay's), or there is none. */
static bool layout_zones(sui_layout_t *L, const sui_content_t *c, int n, const int *first, int cols, int rows, int key_px)
{
    uint16_t at[SUI_CELLS_MAX];
    int srows = 0; /* the status: each line from a new row */
    for (int i = 0; i < n; i++) {
        if (c->group[i] != SUI_GROUP_STATUS || first[i] == first[i + 1]) continue;
        const int end = flow(L, first[i], first[i + 1], cols, srows * cols, at);
        for (int k = first[i]; k < first[i + 1]; k++) put_at(L, k, SUI_PAGE_ALL, at[k] / cols, at[k] % cols);
        srows = (end + cols - 1) / cols;
    }
    const int per = rows - srows; /* rows under the status on a page */
    if (!srows || per < 1) return false;

    int mrows = 0, vend = 0; /* main rows; the service's keys, packed: a line goes on in the row it fits in */
    for (int i = 0; i < n; i++) {
        if (first[i] == first[i + 1]) continue;
        if (c->group[i] == SUI_GROUP_MAIN) {
            mrows += line_rows(L, first[i], first[i + 1], cols);
        } else if (c->group[i] == SUI_GROUP_SERVICE) {
            const int w = flow(L, first[i], first[i + 1], cols, 0, NULL);
            if (w <= cols && vend % cols && vend % cols + w > cols) vend += cols - vend % cols;
            vend = flow(L, first[i], first[i + 1], cols, vend, at);
        }
    }
    const int vrows = (vend + cols - 1) / cols;

    int page = 0, used = 0; /* main: under the status, a line on the next page when it doesn't fit */
    for (int i = 0; i < n; i++) {
        if (c->group[i] != SUI_GROUP_MAIN || first[i] == first[i + 1]) continue;
        const int need = (flow(L, first[i], first[i + 1], cols, 0, at) + cols - 1) / cols;
        int group = need; /* with the lines kept with it (a label and its value line) */
        for (int k = i; k + 1 < n && ((c->keep_next >> k) & 1) && c->group[k + 1] == SUI_GROUP_MAIN; k++)
            group += line_rows(L, first[k + 1], first[k + 2], cols);
        if (used > 0 && used + (group <= per ? group : need) > per) {
            page++;
            used = 0;
        }
        for (int r = 0; r < need; r++, used++) {
            if (used == per) { /* a line longer than a page goes on on the next one */
                page++;
                used = 0;
            }
            for (int k = first[i]; k < first[i + 1]; k++)
                if (at[k] / cols == r) put_at(L, k, page, srows + used, at[k] % cols);
        }
    }
    if (vrows) { /* the service: at the bottom of main's page when all fits there, else on pages of its own */
        int vpage = mrows + vrows <= per ? 0 : mrows ? page + 1 : 0;
        for (int i = 0; i < n; i++) {
            if (c->group[i] != SUI_GROUP_SERVICE || first[i] == first[i + 1]) continue;
            for (int k = first[i]; k < first[i + 1]; k++) {
                const int r = at[k] / cols, p = r / per, last = (vrows - 1) / per;
                const int on = p < last ? per : vrows - last * per; /* rows of service on that page */
                put_at(L, k, vpage + p, rows - on + r % per, at[k] % cols);
            }
            /* a line that starts mid-row, after another line: a divider bar at its first key's left */
            sui_cell_t *e = &L->cell[first[i]];
            if (e->col > 0 && e->piece == 0) {
                e->divider = 1;
                const sui_fit_t was = e->fit;
                e->fit = sui_fit_inset(c->lines[i] + e->start, e->len, key_px,
                                       e->role == SUI_ROLE_STATUS || e->role == SUI_ROLE_LABEL, sui_divider_inset(key_px));
                if (e->fit.nl == 1 && was.nl == 1 && e->fit.px > was.px) e->fit.px = was.px; /* the line's one size */
                even_line(L, c->lines[i], first[i], first[i + 1], key_px); /* the words beside it follow it down */
            }
        }
        page = vpage + (vrows - 1) / per;
    }
    L->pages = page + 1;
    return true;
}

void sui_layout(int cols, int rows, int key_px, const sui_content_t *c, sui_layout_t *out)
{
    memset(out, 0, sizeof *out);
    out->pages = 1;
    if (cols < 1 || rows < 1) return;
    const int n = c->n < 0 ? 0 : (c->n > MAX_LINES ? MAX_LINES : c->n);
    int first[MAX_LINES + 1]; /* line i's cells: first[i] .. first[i + 1] - 1 */
    for (int i = 0; i < n; i++) {
        first[i] = out->n;
        add_words(out, c, i, key_px);
        even_line(out, c->lines[i], first[i], out->n, key_px);
    }
    first[n] = out->n;
    for (int i = 0; i < n; i++)
        if (c->group[i] != SUI_GROUP_NONE) {
            if (layout_zones(out, c, n, first, cols, rows, key_px)) return;
            break;
        }

    /* empty lines before the first line with words and after the last take no rows */
    int a = 0, b = n - 1;
    while (a < n && first[a + 1] == first[a]) a++;
    while (b > a && first[b + 1] == first[b]) b--;

    uint16_t at[SUI_CELLS_MAX];
    int page = 0, used = 0; /* rows of the page filled */
    for (int i = a; i <= b; i++) {
        const int words = first[i + 1] - first[i];
        if (words == 0) { /* an empty row; none at the top of a page or past its end */
            if (used > 0 && used < rows) used++;
            continue;
        }
        const int need = (flow(out, first[i], first[i + 1], cols, 0, at) + cols - 1) / cols;
        /* with the lines kept with it when they fit on one page together, else by itself */
        int group = need;
        for (int k = i; k < b && ((c->keep_next >> k) & 1); k++) group += line_rows(out, first[k + 1], first[k + 2], cols);
        if (used > 0 && used + (group <= rows ? group : need) > rows) { /* the line starts the next page */
            page++;
            used = 0;
        }
        for (int r = 0; r < need; r++) {
            if (used == rows) { /* a line longer than a page goes on on the next one */
                page++;
                used = 0;
            }
            for (int k = first[i]; k < first[i + 1]; k++) {
                if (at[k] / cols != r) continue;
                sui_cell_t *e = &out->cell[k];
                e->page = (uint8_t)page;
                e->row = (uint8_t)used;
                e->col = (uint8_t)(at[k] % cols);
            }
            used++;
        }
    }
    out->pages = page + 1;
}

const sui_cell_t *sui_layout_cell(const sui_layout_t *L, int page, int key_col, int key_row)
{
    for (int i = 0; i < L->n; i++) {
        const sui_cell_t *e = &L->cell[i];
        if ((e->page == page || (e->page == SUI_PAGE_ALL && page >= 0 && page < L->pages)) && e->row == key_row &&
            e->col == key_col)
            return e;
    }
    return NULL;
}

int sui_page_count(int cols, int rows, int key_px, const sui_content_t *c)
{
    sui_layout_t L;
    sui_layout(cols, rows, key_px, c, &L);
    return L.pages;
}

/* ---- paging ---- */

int64_t sui_page_us(int pages) { return pages > SUI_PAGE_MANY ? SUI_PAGE_FAST_US : SUI_PAGE_US; }

int sui_pager_next(const sui_pager_t *p, bool fresh, int64_t now_us, int64_t *due_us)
{
    if (due_us) *due_us = 0;
    if (fresh) return 0;
    if (p->pages < 2) return -1;
    const int64_t due = p->at_us + sui_page_us(p->pages);
    if (now_us < due) {
        if (due_us) *due_us = due;
        return -1;
    }
    return (p->page + 1) % p->pages;
}

int64_t sui_pager_shown(sui_pager_t *p, int page, int pages, int64_t now_us)
{
    p->page = page;
    p->pages = pages;
    p->at_us = now_us;
    return pages > 1 ? now_us + sui_page_us(pages) : 0;
}
