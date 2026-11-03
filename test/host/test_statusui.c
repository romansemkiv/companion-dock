#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "deckmodel/model.h"
#include "statusui/deck_font.h"
#include "statusui/statusui.h"
#include "statusui_fixtures.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers ---- */

/* Page `page` of L as text: a row's words joined by '|' (blank keys at the row's end left out),
 * rows joined by '\n'. */
static const char *page_text(const sui_layout_t *L, const sui_content_t *c, int cols, int rows, int page)
{
    static char out[2048];
    size_t u = 0;
    for (int r = 0; r < rows; r++) {
        if (r) out[u++] = '\n';
        int last = -1;
        for (int k = 0; k < cols; k++)
            if (sui_layout_cell(L, page, k, r)) last = k;
        for (int k = 0; k <= last; k++) {
            if (k) out[u++] = '|';
            const sui_cell_t *e = sui_layout_cell(L, page, k, r);
            if (e) {
                memcpy(out + u, c->lines[e->line] + e->start, e->len);
                u += e->len;
            }
        }
    }
    out[u] = '\0';
    return out;
}

/* One-line content holding just `word`, white on black. */
static void one_word(sui_content_t *c, const char *word)
{
    memset(c, 0, sizeof *c);
    c->lines[0] = word;
    c->n = 1;
    memset(c->fg, 255, 3);
}

/* The fit of a value (nothing may be added) and of a status or label word (may be hyphenated). */
static sui_fit_t fit_of(const char *word, int key_px) { return sui_fit(word, (int)strlen(word), key_px, false); }
static sui_fit_t hy_of(const char *word, int key_px) { return sui_fit(word, (int)strlen(word), key_px, true); }

/* The deck font's size px, or NULL. */
static const sui_font_size_t *font_px(int px)
{
    for (int i = 0; i < sui_font_sizes; i++)
        if (sui_font[i].px == px) return &sui_font[i];
    return NULL;
}

/* The placement rule: the block from the first line's cap top to the last line's baseline
 * is centred in the key; every glyph's ink (the font's tallest and deepest) stays SUI_MARGIN px inside. */
static bool fits_tall(int px, int nl, int key_px)
{
    const sui_font_size_t *f = font_px(px);
    if (!f) return false;
    const int h = (nl - 1) * f->line + f->cap, top = (key_px - h) / 2;
    return top + f->cap - f->asc >= SUI_MARGIN && top + h + f->desc <= key_px - SUI_MARGIN;
}

/* The ink width of s[0..n) at px, with a '-' after it when hyph. */
static int line_width(int px, const char *s, int n, bool hyph)
{
    char buf[SUI_FIT_MAX + 2];
    memcpy(buf, s, (size_t)n);
    buf[n] = '-';
    return sui_text_width(px, buf, n + hyph, NULL);
}

/* s[0..len) in nl lines of `cut` chars (the first ending in an added '-' when hyph) fits a key of
 * key_px at px. */
static bool fits_cut_hy(const char *s, int nl, const uint8_t *cut, bool hyph, int px, int key_px)
{
    if (!fits_tall(px, nl, key_px)) return false;
    for (int i = 0, at = 0; i < nl; at += cut[i++])
        if (cut[i] < 1 || line_width(px, s + at, cut[i], hyph && i == 0) > key_px - 2 * SUI_MARGIN) return false;
    return true;
}
static bool fits_cut(const char *s, int nl, const uint8_t *cut, int px, int key_px)
{
    return fits_cut_hy(s, nl, cut, false, px, key_px);
}

/* The largest size, capped, at which s[0..len) fits on one line (0: none). */
static int one_line_px(const char *s, int len, int key_px)
{
    const uint8_t cut[1] = {(uint8_t)len};
    for (int i = sui_font_sizes - 1; i >= 0; i--)
        if (sui_font[i].px * 100 <= key_px * SUI_CAP_PCT && fits_cut(s, 1, cut, sui_font[i].px, key_px))
            return sui_font[i].px;
    return 0;
}

static bool own_break(const char *s, int cut) { return s[cut - 1] == '-' || s[cut - 1] == '.'; }
static bool case_break(const char *s, int cut) { return islower((unsigned char)s[cut - 1]) && isupper((unsigned char)s[cut]); }

/* The kind of break after chars [0, cut): 2 after the word's own '-' or '.', 1 before a capital that
 * follows a small letter, 0 any other. */
static int break_kind(const char *s, int cut) { return own_break(s, cut) ? 2 : case_break(s, cut) ? 1 : 0; }

/* The best size, capped, of any split of s[0..len) into 2 lines (kind > 0: only those of that kind);
 * 0: none. */
static int lines_px(const char *s, int len, int kind, int key_px)
{
    for (int i = sui_font_sizes - 1; i >= 0; i--) {
        const int px = sui_font[i].px;
        if (px * 100 > key_px * SUI_CAP_PCT) continue;
        for (int a = 1; a < len; a++) {
            const uint8_t cut[2] = {(uint8_t)a, (uint8_t)(len - a)};
            if ((!kind || break_kind(s, a) == kind) && fits_cut(s, 2, cut, px, key_px)) return px;
        }
    }
    return 0;
}

/* A fit is whole and inside its key: its lines add up to the word, none empty, each within the margin. */
static void assert_fit_ok(const char *s, int len, const sui_fit_t *f, int key_px)
{
    TEST_ASSERT_TRUE_MESSAGE(f->nl >= 1 && f->nl <= 2, s); /* never a third line */
    TEST_ASSERT_NOT_NULL_MESSAGE(font_px(f->px), s);
    TEST_ASSERT_TRUE_MESSAGE(f->px * 100 <= key_px * SUI_CAP_PCT, s);
    int t = 0;
    for (int i = 0; i < f->nl; i++) t += f->cut[i];
    TEST_ASSERT_EQUAL_INT_MESSAGE(len, t, s); /* the chunks are the word (an added '-' is not a chunk) */
    TEST_ASSERT_TRUE_MESSAGE(!f->hyph || f->nl == 2, s);
    TEST_ASSERT_TRUE_MESSAGE(fits_cut_hy(s, f->nl, f->cut, f->hyph, f->px, key_px), s);
}

/* The text lines a fit draws, joined by '|' (the first with its added '-'). */
static const char *drawn(const char *s, const sui_fit_t *f)
{
    static char out[2 * SUI_FIT_MAX];
    size_t u = 0;
    for (int i = 0, at = 0; i < f->nl; at += f->cut[i++]) {
        if (i) out[u++] = '|';
        memcpy(out + u, s + at, f->cut[i]);
        u += f->cut[i];
        if (i == 0 && f->hyph) out[u++] = '-';
    }
    out[u] = '\0';
    return out;
}

static int count_px(const uint8_t *buf, int px, const uint8_t *col)
{
    int n = 0;
    for (int i = 0; i < px; i++)
        if (!memcmp(buf + 3 * i, col, 3)) n++;
    return n;
}

static void fill_key(uint8_t *buf, int px, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < px; i++) {
        buf[3 * i] = r;
        buf[3 * i + 1] = g;
        buf[3 * i + 2] = b;
    }
}

/* The box of the pixels of a key_px key that differ from bg; false: none. */
static bool ink_box(const uint8_t *buf, int key_px, const uint8_t *bg, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = *y0 = key_px;
    *x1 = *y1 = -1;
    for (int y = 0; y < key_px; y++)
        for (int x = 0; x < key_px; x++)
            if (memcmp(buf + 3 * (y * key_px + x), bg, 3)) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
    return *x1 >= 0;
}

/* Every pixel of the key is bg, col, or a blend of the two (each channel between them). */
static void assert_blends(const uint8_t *buf, int key_px, const uint8_t *bg, const uint8_t *col)
{
    for (int i = 0; i < key_px * key_px; i++)
        for (int k = 0; k < 3; k++) {
            const int lo = bg[k] < col[k] ? bg[k] : col[k], hi = bg[k] < col[k] ? col[k] : bg[k];
            TEST_ASSERT_TRUE(buf[3 * i + k] >= lo && buf[3 * i + k] <= hi);
        }
}

/* The background of key (col, row) of page `page`: its word's line's, c->bg for a blank key. */
static const uint8_t *key_bg(int cols, int rows, int key_px, const sui_content_t *c, int page, int col, int row)
{
    static sui_layout_t L;
    sui_layout(cols, rows, key_px, c, &L);
    const sui_cell_t *e = sui_layout_cell(&L, page % L.pages, col, row);
    return e ? sui_line_bg(c, e->line) : c->bg;
}

/* Key (col, row) of page `page` drawn: nothing within SUI_MARGIN px of its edges. */
static void assert_key_inside(int cols, int rows, int key_px, const sui_content_t *c, int page, int col, int row)
{
    static uint8_t buf[120 * 120 * 3];
    sui_render_key(cols, rows, key_px, c, page, col, row, buf);
    static sui_layout_t L;
    sui_layout(cols, rows, key_px, c, &L);
    const sui_cell_t *e = sui_layout_cell(&L, page % L.pages, col, row);
    if (e && e->divider) { /* the divider bar is inside the margin by design: only the text is checked */
        int bx, by, bw, bh;
        sui_divider_box(key_px, &bx, &by, &bw, &bh);
        for (int y = by; y < by + bh; y++)
            for (int x = bx; x < bx + bw; x++) memcpy(buf + 3 * (y * key_px + x), sui_line_bg(c, e->line), 3);
    }
    int x0, y0, x1, y1;
    if (!ink_box(buf, key_px, key_bg(cols, rows, key_px, c, page, col, row), &x0, &y0, &x1, &y1)) return;
    TEST_ASSERT_TRUE(x0 >= SUI_MARGIN && y0 >= SUI_MARGIN);
    TEST_ASSERT_TRUE(x1 < key_px - SUI_MARGIN && y1 < key_px - SUI_MARGIN);
}

/* ---- layout: placement ---- */

/* The reference layout: Module 32 / XL, everything at once. */
static void test_layout_dock_8x4(void)
{
    sui_content_t c;
    fx_screen_dock(&c, "192.168.8.225", 5343, NULL, NULL);
    static sui_layout_t L;
    sui_layout(8, 4, 96, &c, &L);
    TEST_ASSERT_EQUAL_INT(1, L.pages);
    TEST_ASSERT_EQUAL_INT(11, L.n);
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nRemote|surface\n192|168|8|225|port|5343\n", page_text(&L, &c, 8, 4, 0));
    TEST_ASSERT_EQUAL_INT(1, sui_page_count(8, 4, 96, &c));
    TEST_ASSERT_NULL(sui_layout_cell(&L, 1, 0, 0)); /* no second page */
}

/* The reference layout: a Mini pages through it. */
static void test_layout_dock_mini_pages(void)
{
    sui_content_t c;
    fx_screen_dock(&c, "192.168.8.225", 5343, NULL, NULL);
    static sui_layout_t L;
    sui_layout(3, 2, 80, &c, &L); /* the status row on every page; the address 2 + 2, the pair */
    TEST_ASSERT_EQUAL_INT(4, L.pages);
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nRemote|surface", page_text(&L, &c, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\n192|168", page_text(&L, &c, 3, 2, 1));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\n8|225", page_text(&L, &c, 3, 2, 2));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nport|5343", page_text(&L, &c, 3, 2, 3));
    TEST_ASSERT_EQUAL_INT(4, sui_page_count(3, 2, 80, &c));
}

static void test_layout_ipv4_octets(void)
{
    sui_content_t c = {.lines = {"10.0.0.7 4.2.0 1.2.3.256 1.2.3.4.5 a.b.c.d 0255.1.1.1"}, .n = 1};
    static sui_layout_t L;
    sui_layout(8, 2, 96, &c, &L);
    TEST_ASSERT_EQUAL_STRING("10|0|0|7|4.2.0|1.2.3.256|1.2.3.4.5|a.b.c.d\n0255.1.1.1", page_text(&L, &c, 8, 2, 0));

    /* an address at the end of a line, and words around it, as the screens have them */
    sui_content_t d = {.lines = {"Connecting 192.168.100.200", "255.255.255.255:16622"}, .n = 2};
    sui_layout(5, 3, 72, &d, &L);
    TEST_ASSERT_EQUAL_STRING("Connecting|192|168|100|200\n255.255.255.255:16622\n", page_text(&L, &d, 5, 3, 0));
}

static void test_layout_line_wraps_rows(void)
{
    sui_content_t c = {.lines = {"one two three four five", "six"}, .n = 2};
    static sui_layout_t L;
    sui_layout(3, 3, 72, &c, &L);
    TEST_ASSERT_EQUAL_INT(1, L.pages);
    TEST_ASSERT_EQUAL_STRING("one|two|three\nfour|five\nsix", page_text(&L, &c, 3, 3, 0));

    /* runs of spaces separate words like one space; leading and trailing ones add nothing */
    sui_content_t d = {.lines = {"  a   b  "}, .n = 1};
    sui_layout(3, 1, 72, &d, &L);
    TEST_ASSERT_EQUAL_STRING("a|b", page_text(&L, &d, 3, 1, 0));
}

/* A line that doesn't fit on the rest of a page starts the next one. */
static void test_layout_page_break_at_line_end(void)
{
    sui_content_t c = {.lines = {"a b c d", "e f"}, .n = 2};
    static sui_layout_t L;
    sui_layout(3, 2, 80, &c, &L);
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("a|b|c\nd", page_text(&L, &c, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("e|f\n", page_text(&L, &c, 3, 2, 1));

    sui_content_t d = {.lines = {"a", "b c d e"}, .n = 2};
    sui_layout(3, 2, 80, &d, &L);
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("a\n", page_text(&L, &d, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("b|c|d\ne", page_text(&L, &d, 3, 2, 1));

    /* the finding screen on a Mini: one row under the status, a line split over pages only when it alone
     * needs more */
    sui_content_t e;
    fx_screen_looking(&e, "192.168.100.200", NULL, "CompanionBox-ABCD", "abcdefghjk");
    sui_layout(3, 2, 80, &e, &L);
    TEST_ASSERT_EQUAL_INT(5, L.pages);
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\nBox|IP", page_text(&L, &e, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\n192|168", page_text(&L, &e, 3, 2, 1)); /* 2 + 2 */
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\n100|200", page_text(&L, &e, 3, 2, 2));
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\nWiFi|Companion|Box-ABCD", page_text(&L, &e, 3, 2, 3));
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\nPass|abcde|fghjk", page_text(&L, &e, 3, 2, 4));
}

/* Only a line longer than a whole page is split over pages, and it starts on a page of its own. */
static void test_layout_long_line_spans_pages(void)
{
    sui_content_t c = {.lines = {"x", "1 2 3 4 5 6 7 8"}, .n = 2};
    static sui_layout_t L;
    sui_layout(3, 2, 80, &c, &L);
    TEST_ASSERT_EQUAL_INT(3, L.pages);
    TEST_ASSERT_EQUAL_STRING("x\n", page_text(&L, &c, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("1|2|3\n4|5|6", page_text(&L, &c, 3, 2, 1));
    TEST_ASSERT_EQUAL_STRING("7|8\n", page_text(&L, &c, 3, 2, 2));

    /* what follows it shares its last page when it fits there */
    sui_content_t d = {.lines = {"1 2 3 4 5 6 7", "y"}, .n = 2};
    sui_layout(3, 2, 80, &d, &L);
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("1|2|3\n4|5|6", page_text(&L, &d, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("7\ny", page_text(&L, &d, 3, 2, 1));
}

static void test_layout_empty_lines(void)
{
    sui_content_t c = {.lines = {"", "Hello", "", "World"}, .n = 4};
    static sui_layout_t L;
    sui_layout(3, 4, 72, &c, &L);
    TEST_ASSERT_EQUAL_INT(1, L.pages);
    TEST_ASSERT_EQUAL_STRING("Hello\n\nWorld\n", page_text(&L, &c, 3, 4, 0));

    sui_content_t m;
    sui_screen_message(&m, "\nHello\n\nWorld");
    sui_layout(3, 4, 72, &m, &L);
    TEST_ASSERT_EQUAL_STRING("Hello\n\nWorld\n", page_text(&L, &m, 3, 4, 0));
    sui_screen_message(&m, "Hello\n\nWorld\n ");
    sui_layout(3, 4, 72, &m, &L);
    TEST_ASSERT_EQUAL_STRING("Hello\n\nWorld\n", page_text(&L, &m, 3, 4, 0));

    /* one row: the empty line is not a page of its own, and no page starts with an empty row */
    sui_layout(3, 1, 72, &c, &L);
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("Hello", page_text(&L, &c, 3, 1, 0));
    TEST_ASSERT_EQUAL_STRING("World", page_text(&L, &c, 3, 1, 1));
    sui_content_t d = {.lines = {"a b c d", "", "e"}, .n = 3};
    sui_layout(3, 2, 72, &d, &L);
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("a|b|c\nd", page_text(&L, &d, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("e\n", page_text(&L, &d, 3, 2, 1));

    /* nothing but spaces: every key blank, one page */
    sui_screen_message(&m, " ");
    sui_layout(5, 3, 72, &m, &L);
    TEST_ASSERT_EQUAL_INT(0, L.n);
    TEST_ASSERT_EQUAL_INT(1, L.pages);
}


/* ---- the fit: a word in its key ---- */

static const int KEY_PX[] = {72, 80, 96, 112, 120}; /* every model's key size (deckmodel) */

/* Short words stop at the cap, 42 % of the key: octets match, a lone "8" doesn't dwarf them. */
static void test_fit_cap(void)
{
    static const struct { int key, px; } K[] = {{72, 30}, {80, 33}, {96, 40}, {112, 47}, {120, 50}};
    static const char *const W[] = {"8", "192", "No", "5343", "port", "WiFi", "Pass"};
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        for (size_t w = 0; w < sizeof W / sizeof W[0]; w++) {
            const sui_fit_t f = fit_of(W[w], K[i].key);
            TEST_ASSERT_EQUAL_INT_MESSAGE(K[i].px, f.px, W[w]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, f.nl, W[w]);
            assert_fit_ok(W[w], (int)strlen(W[w]), &f, K[i].key);
        }
    /* every key size's cap is one of the font's sizes, and so is the smallest readable size */
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) TEST_ASSERT_NOT_NULL(font_px(K[i].px));
    TEST_ASSERT_NOT_NULL(font_px(SUI_MIN_PX));
    TEST_ASSERT_EQUAL_INT(10, sui_font[0].px);
}

/* The rule, against a search of every split: one line at the largest size that fits, unless that is
 * below SUI_MIN_PX; then 2 lines: after the word's own '-' or '.' at the largest size such a split
 * reaches when that is readable, else the same before a capital that follows a small letter,
 * else at the largest size any split reaches; when nothing is readable, the larger of one line and two
 * (one on a tie). Never a third line. */
static void test_fit_sizes_and_min_split(void)
{
    static const char *const W[] = {
        "No", "in", "Add", "for", "port", "Pass", "WiFi", "Remote", "surface", "network", "Finding",
        "Several", "Choose", "password", "Companion", "Companions", "Connecting", "Reconnecting",
        "abcdefghjk", "CompanionBox-ABCD", "4.0.0-beta.2+7156-main-1234abcd", "4.2.0", "available",
        "companion-main.studio-b.example.internal", "WWWWWWWWWWWW", "iiiiiiiiiiiiiiiiiiii", "Tech-Desk",
        "255.255.255.255:16622", "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmm", "1234567890.1234567890", "CompanionBox-8D1C",
        "studioBTechDeskCompanion", "aBcDeFgHjKmNpQrStUvW",
    };
    for (size_t k = 0; k < sizeof KEY_PX / sizeof KEY_PX[0]; k++)
        for (size_t w = 0; w < sizeof W / sizeof W[0]; w++) {
            const char *s = W[w];
            const int len = (int)strlen(s), key = KEY_PX[k];
            const sui_fit_t f = sui_fit(s, len, key, false);
            char msg[96];
            snprintf(msg, sizeof msg, "%s on %d px", s, key);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, f.hyph, msg); /* a value: nothing added */
            const int one = one_line_px(s, len, key), own = len >= 2 ? lines_px(s, len, 2, key) : 0,
                      cap = len >= 2 ? lines_px(s, len, 1, key) : 0, any = len >= 2 ? lines_px(s, len, 0, key) : 0;
            int nl, px, kind = 0;
            if (one >= SUI_MIN_PX) nl = 1, px = one;
            else if (own >= SUI_MIN_PX) nl = 2, px = own, kind = 2;
            else if (cap >= SUI_MIN_PX) nl = 2, px = cap, kind = 1;
            else if (any >= SUI_MIN_PX) nl = 2, px = any;
            else if (any > one) nl = 2, px = any; /* nothing readable: the largest */
            else nl = 1, px = one;
            TEST_ASSERT_EQUAL_INT_MESSAGE(nl, f.nl, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(px, f.px, msg);
            if (f.px) assert_fit_ok(s, len, &f, key);
            if (kind) TEST_ASSERT_EQUAL_INT_MESSAGE(kind, break_kind(s, f.cut[0]), msg);
        }
}

/* Pinned: what the keys show. */
static void test_fit_pinned(void)
{
    sui_fit_t f = fit_of("Companion", 72); /* readable on one line: not split */
    TEST_ASSERT_EQUAL_INT(1, f.nl);
    TEST_ASSERT_TRUE(f.px >= SUI_MIN_PX);
    f = fit_of("Reconnecting", 72); /* too small on one line: two, even */
    TEST_ASSERT_EQUAL_INT(2, f.nl);
    TEST_ASSERT_EQUAL_INT(6, f.cut[0]);
    TEST_ASSERT_TRUE(f.px > SUI_MIN_PX);
    TEST_ASSERT_EQUAL_INT(1, fit_of("Reconnecting", 120).nl);
    f = fit_of("CompanionBox-8D1C", 96); /* the hotspot's name on a Module 32: at its own '-' */
    TEST_ASSERT_EQUAL_INT(2, f.nl);
    TEST_ASSERT_EQUAL_INT(13, f.cut[0]);
    TEST_ASSERT_EQUAL_INT(4, f.cut[1]);
    TEST_ASSERT_TRUE(f.px >= SUI_MIN_PX);
    f = fit_of("CompanionBox-8D1C", 72); /* "CompanionBox-" doesn't read on 72 px: Companion / Box-8D1C */
    TEST_ASSERT_EQUAL_INT(2, f.nl);
    TEST_ASSERT_EQUAL_INT(9, f.cut[0]);
    TEST_ASSERT_EQUAL_INT(8, f.cut[1]);
    f = fit_of("abcdefghjk", 72); /* the hotspot password: one line */
    TEST_ASSERT_EQUAL_INT(1, f.nl);
    TEST_ASSERT_TRUE(f.px >= SUI_MIN_PX);
}

/* Split text lines: no char added or dropped (the chunks are the word, in order); of the splits at the
 * chosen size of the chosen one's kind (test_fit_sizes_and_min_split checks the kind), the narrowest
 * widest line, then the widest narrowest line, then the longer first line. */
static void test_fit_breaks(void)
{
    static const char *const W[] = {
        "Reconnecting", "CompanionBox-ABCD", "4.0.0-beta.2+7156-main-1234abcd", "abcdefghjkmnpqrstuvwxyz2",
        "companion-main.studio-b.example.internal", "a.b-c.d-e.f-g.h-i.j-k.l-m.n-o.p", "Tech-Desk-Studio-B",
        "1234567890.1234567890", "WWWWWWWWWWWW", "mmmmmmmmmmmmmmmmmmmm-", "-mmmmmmmmmmmmmmmmmmmm", "Companions",
        "CompanionBox-8D1C", "studioBTechDeskCompanion", "aBcDeFgHjKmNpQrStUvW",
    };
    for (size_t k = 0; k < sizeof KEY_PX / sizeof KEY_PX[0]; k++)
        for (size_t w = 0; w < sizeof W / sizeof W[0]; w++) {
            const char *s = W[w];
            const int len = (int)strlen(s), key = KEY_PX[k];
            const sui_fit_t f = sui_fit(s, len, key, false);
            if (f.nl < 2 || !f.px) continue;
            assert_fit_ok(s, len, &f, key);
            char msg[96];
            snprintf(msg, sizeof msg, "%s on %d px", s, key);
            const int kind = break_kind(s, f.cut[0]);
            int best = 0, bhi = 0, blo = 0;
            for (int a = 1; a < len; a++) {
                const uint8_t cut[2] = {(uint8_t)a, (uint8_t)(len - a)};
                if ((kind && break_kind(s, a) != kind) || !fits_cut(s, 2, cut, f.px, key)) continue;
                const int wa = sui_text_width(f.px, s, a, NULL), wb = sui_text_width(f.px, s + a, len - a, NULL);
                const int hi = wa > wb ? wa : wb, lo = wa < wb ? wa : wb;
                if (!best || hi < bhi || (hi == bhi && lo >= blo)) best = a, bhi = hi, blo = lo;
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE(best, f.cut[0], msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(len, f.cut[0] + f.cut[1], msg);
        }
    /* its own '.' */
    const sui_fit_t f = fit_of("1234567890.1234567890", 72);
    TEST_ASSERT_EQUAL_INT(2, f.nl);
    TEST_ASSERT_EQUAL_INT(11, f.cut[0]);
}

/* a status or label word that needs two lines may break with an added '-': at the largest size
 * where a break between two letters, leaving 3 or more letters each side, fits (its own '-' or '.'
 * first, as for any word); of those, the narrowest widest line, then the widest narrowest, then the
 * longer first line. Without its '-' it is the word again. A value never gets one. */
static void test_fit_hyphenation(void)
{
    sui_fit_t f = hy_of("Reconnecting", 72);
    TEST_ASSERT_EQUAL_INT(2, f.nl);
    TEST_ASSERT_EQUAL_INT(1, f.hyph);
    TEST_ASSERT_EQUAL_STRING("Recon-|necting", drawn("Reconnecting", &f));
    const sui_fit_t v = fit_of("Reconnecting", 72); /* as a value: no '-' */
    TEST_ASSERT_EQUAL_STRING("Reconn|ecting", drawn("Reconnecting", &v));
    TEST_ASSERT_EQUAL_INT(0, hy_of("Reconnecting", 120).hyph); /* one line: nothing to break */
    f = hy_of("Tech-Desk-Studio-B", 72); /* its own '-' first: nothing added */
    TEST_ASSERT_EQUAL_INT(2, f.nl);
    TEST_ASSERT_EQUAL_INT(0, f.hyph);
    TEST_ASSERT_EQUAL_CHAR('-', "Tech-Desk-Studio-B"[f.cut[0] - 1]);

    static const char *const W[] = {"Reconnecting", "Disconnected", "Configuration", "Unavailability", "Companions",
                                    "Connecting", "Internationalisation", "Synchronising"};
    for (size_t k = 0; k < sizeof KEY_PX / sizeof KEY_PX[0]; k++)
        for (size_t w = 0; w < sizeof W / sizeof W[0]; w++) {
            const char *s = W[w];
            const int len = (int)strlen(s), key = KEY_PX[k];
            char msg[96];
            snprintf(msg, sizeof msg, "%s on %d px", s, key);
            f = hy_of(s, key);
            assert_fit_ok(s, len, &f, key);
            /* without its '-' it is the word again */
            char back[2 * SUI_FIT_MAX];
            size_t u = 0;
            for (const char *p = drawn(s, &f); *p; p++)
                if (*p != '|' && !(f.hyph && *p == '-' && p[1] == '|')) back[u++] = *p;
            back[u] = '\0';
            TEST_ASSERT_EQUAL_STRING_MESSAGE(s, back, msg);
            if (one_line_px(s, len, key) >= SUI_MIN_PX) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(1, f.nl, msg);
                continue;
            }
            /* the best hyphenated split by search: 3+ letters a side, between letters */
            int px = 0, best = 0, bhi = 0, blo = 0;
            for (int i = sui_font_sizes - 1; i >= 0 && !px; i--) {
                const int p = sui_font[i].px;
                if (p * 100 > key * SUI_CAP_PCT || p < SUI_MIN_PX) continue;
                for (int a = 3; a <= len - 3; a++) {
                    const uint8_t cut[2] = {(uint8_t)a, (uint8_t)(len - a)};
                    if (!isalpha((unsigned char)s[a - 1]) || !isalpha((unsigned char)s[a]) || !fits_cut_hy(s, 2, cut, true, p, key))
                        continue;
                    const int wa = line_width(p, s, a, true), wb = line_width(p, s + a, len - a, false);
                    const int hi = wa > wb ? wa : wb, lo = wa < wb ? wa : wb;
                    if (!best || hi < bhi || (hi == bhi && lo >= blo)) best = a, bhi = hi, blo = lo;
                    px = p;
                }
            }
            TEST_ASSERT_TRUE_MESSAGE(px > 0, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, f.hyph, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(px, f.px, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(best, f.cut[0], msg);
        }

    /* a word too short for 3 letters a side: then 1 or more (here a 48 px key) */
    f = hy_of("WWWWW", 48);
    TEST_ASSERT_TRUE(one_line_px("WWWWW", 5, 48) < SUI_MIN_PX);
    TEST_ASSERT_EQUAL_INT(2, f.nl);
    TEST_ASSERT_EQUAL_INT(1, f.hyph);
    TEST_ASSERT_TRUE(f.cut[0] < 3 || f.cut[1] < 3);
    TEST_ASSERT_TRUE(f.px >= SUI_MIN_PX);
    assert_fit_ok("WWWWW", 5, &f, 48);
    TEST_ASSERT_EQUAL_INT(0, fit_of("WWWWW", 48).hyph); /* as a value: nothing added */

    /* on the keys: the Reconnecting screen on an MK.2; the same word in a custom message gets no '-' */
    sui_content_t c;
    sui_screen_reconnecting(&c, NULL);
    static sui_layout_t L;
    sui_layout(5, 3, 72, &c, &L);
    TEST_ASSERT_EQUAL_STRING("Recon-|necting", drawn(c.lines[0], &L.cell[0].fit));
    sui_screen_message(&c, "Reconnecting");
    sui_layout(5, 3, 72, &c, &L);
    TEST_ASSERT_EQUAL_INT(0, L.cell[0].fit.hyph);
    TEST_ASSERT_EQUAL_STRING("Reconn|ecting", drawn(c.lines[0], &L.cell[0].fit));
}

/* The first cell showing exactly `text`. */
static const sui_cell_t *cell_of(const sui_layout_t *L, const sui_content_t *c, const char *text)
{
    for (int k = 0; k < L->n; k++) {
        const sui_cell_t *e = &L->cell[k];
        if ((size_t)e->len == strlen(text) && !memcmp(c->lines[e->line] + e->start, text, e->len)) return e;
    }
    TEST_FAIL_MESSAGE(text);
    return NULL;
}

/* in a line, the words of one role that fit on one line take the smallest of their sizes, so
 * the line looks even; words of another role keep theirs, and a word on two lines keeps its own. */
static void test_layout_one_size_per_line(void)
{
    static const int keys[] = {72, 96, 120};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        const int key = keys[k];
        sui_content_t c[5];
        fx_screen_no_network(&c[0], "192.168.4.1", "CompanionBox-ABCD", NULL);         /* No network, No password */
        fx_screen_no_network(&c[1], "192.168.4.1", "CompanionBox-ABCD", "abcdefghjk"); /* Pass <password> */
        fx_screen_dock(&c[2], "192.168.8.225", 5343, NULL, NULL);      /* the IP and port line */
        fx_screen_dock(&c[3], "192.168.8.225", 5343, "CompanionBox-8D1C", "abcdefghjk");
        fx_screen_looking(&c[4], "10.0.0.7", NULL, NULL, NULL);
        static sui_layout_t L;
        for (int s = 0; s < 5; s++) {
            sui_layout(8, 4, key, &c[s], &L);
            for (int i = 0; i < L.n; i++) {
                const sui_cell_t *e = &L.cell[i];
                const bool word = e->role == SUI_ROLE_STATUS || e->role == SUI_ROLE_LABEL;
                const sui_fit_t own = sui_fit(c[s].lines[e->line] + e->start, e->len, key, word);
                int least = 0; /* the smallest best size of the line's one-line words of its role */
                for (int j = 0; j < L.n; j++) {
                    const sui_cell_t *o = &L.cell[j];
                    if (o->line != e->line || o->role != e->role) continue;
                    const sui_fit_t f = sui_fit(c[s].lines[o->line] + o->start, o->len, key, word);
                    if (f.nl == 1 && f.px >= SUI_MIN_PX && (!least || f.px < least)) least = f.px;
                }
                char msg[64];
                snprintf(msg, sizeof msg, "screen %d word %d on %d px", s, i, key);
                TEST_ASSERT_EQUAL_INT_MESSAGE(own.nl == 1 && own.px >= SUI_MIN_PX ? least : own.px, e->fit.px, msg);
            }
        }
        /* pinned: "No" as large as "network" (both status) */
        sui_layout(8, 4, key, &c[0], &L);
        TEST_ASSERT_EQUAL_INT(L.cell[1].fit.px, L.cell[0].fit.px);
        TEST_ASSERT_TRUE(L.cell[0].fit.px < fit_of("No", key).px);
        /* the IP line: the octets and the port number alike (accent), "port" its own (label) */
        sui_layout(8, 4, key, &c[2], &L);
        static const char *const accent[] = {"192", "168", "8", "225", "5343"};
        for (size_t i = 0; i < sizeof accent / sizeof accent[0]; i++)
            TEST_ASSERT_EQUAL_INT(cell_of(&L, &c[2], "192")->fit.px, cell_of(&L, &c[2], accent[i])->fit.px);
        TEST_ASSERT_EQUAL_INT(SUI_ROLE_LABEL, cell_of(&L, &c[2], "port")->role);
        TEST_ASSERT_EQUAL_INT(hy_of("port", key).px, cell_of(&L, &c[2], "port")->fit.px);
        /* "Pass" (label) is not pulled down by the password beside it (value) */
        sui_layout(8, 4, key, &c[1], &L);
        TEST_ASSERT_EQUAL_INT(hy_of("Pass", key).px, cell_of(&L, &c[1], "Pass")->fit.px);
    }
    /* the Plus: "WiFi" keeps its own size beside the hotspot's name, which goes over 2 keys */
    sui_content_t d;
    fx_screen_no_network(&d, "192.168.4.1", "CompanionBox-ABCD", NULL);
    static sui_layout_t M;
    sui_layout(4, 2, 120, &d, &M);
    TEST_ASSERT_EQUAL_INT(hy_of("WiFi", 120).px, cell_of(&M, &d, "WiFi")->fit.px);
    TEST_ASSERT_TRUE(cell_of(&M, &d, "WiFi")->fit.px > cell_of(&M, &d, "Companion")->fit.px);
}

/* the comfortable size, max(18 px, a quarter of the key). */
static void test_comfort_px(void)
{
    static const int K[][2] = {{72, 18}, {80, 20}, {96, 24}, {112, 28}, {120, 30}, {40, 18}};
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) TEST_ASSERT_EQUAL_INT(K[i][1], sui_comfort_px(K[i][0]));
    for (size_t i = 0; i < 5; i++) TEST_ASSERT_NOT_NULL(font_px(K[i][1]));
}

/* The cells of word `word` (0-based, split on spaces) of line `line`: their count, and their text joined by
 * '|' into out. */
static int word_cells(const sui_layout_t *L, const sui_content_t *c, int line, int word, char *out)
{
    int n = 0, w = -1, end = -1;
    size_t u = 0;
    for (int k = 0; k < L->n; k++) {
        const sui_cell_t *e = &L->cell[k];
        if (e->line != line) continue;
        if (end < 0 || e->start > end) w++; /* a new word */
        end = e->start + e->len;
        if (w != word) continue;
        if (n++) out[u++] = '|';
        memcpy(out + u, c->lines[line] + e->start, e->len);
        u += e->len;
    }
    out[u] = '\0';
    return n;
}

/* A value's pieces: one line each, one size, the smallest of their own sizes; they make the value. */
static int assert_value_pieces(const sui_layout_t *L, const sui_content_t *c, int line, int word, int key_px, const char *want)
{
    char got[160];
    const int n = word_cells(L, c, line, word, got);
    int px = -1, least = 0;
    char back[160];
    size_t u = 0;
    for (int k = 0, w = -1, end = -1; k < L->n; k++) {
        const sui_cell_t *e = &L->cell[k];
        if (e->line != line) continue;
        if (end < 0 || e->start > end) w++;
        end = e->start + e->len;
        if (w != word) continue;
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, e->fit.nl, want);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, e->fit.hyph, want);
        if (px < 0) px = e->fit.px;
        TEST_ASSERT_EQUAL_INT_MESSAGE(px, e->fit.px, want); /* one size */
        const int own = one_line_px(c->lines[line] + e->start, e->len, key_px);
        if (!least || own < least) least = own;
        memcpy(back + u, c->lines[line] + e->start, e->len);
        u += e->len;
    }
    back[u] = '\0';
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, back, got); /* nothing added or dropped */
    if (n > 1) TEST_ASSERT_EQUAL_INT_MESSAGE(least, px, want); /* the smallest piece's own size */
    return n;
}

static int cut_kind(const char *s, int a)
{
    const char p = s[a - 1], q = s[a];
    if (islower((unsigned char)p) && isupper((unsigned char)q)) return 3;
    if (p == '-' || p == '.') return 2;
    if ((isalpha((unsigned char)p) && isdigit((unsigned char)q)) || (isdigit((unsigned char)p) && isalpha((unsigned char)q))) return 1;
    return 0;
}

/* a value that doesn't reach the comfortable size on one line goes over consecutive keys, one line
 * each, at one size: the SSID as [Companion][Box-8D1C], the 10-character password on 2 keys. */
static void test_value_across_keys(void)
{
    static const struct { int cols, rows, px; } D[] = {{8, 4, 96}, {5, 3, 72}, {4, 2, 120}}; /* Module 32, MK.2, Plus */
    static sui_layout_t L;
    char got[160];
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) {
        const int key = D[i].px;
        char msg[32];
        snprintf(msg, sizeof msg, "%d px", key);
        sui_content_t c;
        fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", "abcdefghjk"); /* WiFi line 3, Pass line 4 */
        sui_layout(D[i].cols, D[i].rows, key, &c, &L);
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, word_cells(&L, &c, 3, 1, got), msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("Companion|Box-8D1C", got, msg);
        assert_value_pieces(&L, &c, 3, 1, key, "CompanionBox-8D1C");
        /* the password: 2 keys when one can't reach the comfortable size, each piece comfortable */
        TEST_ASSERT_TRUE(one_line_px("abcdefghjk", 10, key) < sui_comfort_px(key));
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, assert_value_pieces(&L, &c, 4, 1, key, "abcdefghjk"), msg);
        word_cells(&L, &c, 4, 1, got);
        int px = 0;
        for (int k = 0; k < L.n; k++)
            if (L.cell[k].line == 4 && L.cell[k].start > 0) px = L.cell[k].fit.px;
        TEST_ASSERT_TRUE_MESSAGE(px >= sui_comfort_px(key), msg);
    }

    /* long values break at their own breaks and reach the comfortable size; a short one stays whole */
    static const int keys[] = {72, 96, 120};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        const int key = keys[k];
        sui_content_t c;
        fx_screen_too_old(&c, "4.0.0-beta.2+7156-main-1234abcd", NULL, NULL); /* "Has <ver>": line 2 */
        sui_layout(8, 4, key, &c, &L);
        const int n = assert_value_pieces(&L, &c, 2, 1, key, "4.0.0-beta.2+7156-main-1234abcd");
        TEST_ASSERT_TRUE(n >= 2);
        for (int i = 0; i < L.n; i++) {
            const sui_cell_t *e = &L.cell[i];
            if (e->line != 2 || e->start <= 4) continue;
            TEST_ASSERT_TRUE(cut_kind(c.lines[2], e->start) > 0); /* a piece starts after a natural break */
            TEST_ASSERT_TRUE(e->fit.px >= sui_comfort_px(key));
        }
        fx_screen_too_old(&c, "4.2.0", NULL, NULL);
        sui_layout(8, 4, key, &c, &L);
        TEST_ASSERT_EQUAL_INT(1, word_cells(&L, &c, 2, 1, got));

        /* a long value with no break (no part reads alone): the fewest keys at the comfortable size */
        static char flat[41];
        for (int i = 0; i < 40; i++) flat[i] = (char)('a' + i % 26);
        flat[40] = '\0';
        fx_screen_no_network(&c, "192.168.4.1", "S", flat);
        sui_layout(8, 4, key, &c, &L);
        const int pieces = assert_value_pieces(&L, &c, 4, 1, key, flat);
        int greedy = 0; /* the fewest pieces that fit one line each at the comfortable size */
        for (int i = 0; i < 40; greedy++) {
            int j = i + 1;
            while (j < 40 && sui_text_width(sui_comfort_px(key), flat + i, j + 1 - i, NULL) <= key - 2 * SUI_MARGIN) j++;
            i = j;
        }
        TEST_ASSERT_EQUAL_INT(greedy, pieces);
    }

    /* status and label words keep their 2 hyphenated lines in one key */
    sui_content_t r;
    sui_screen_reconnecting(&r, NULL);
    sui_layout(5, 3, 72, &r, &L);
    TEST_ASSERT_EQUAL_INT(1, L.n);
    TEST_ASSERT_EQUAL_INT(2, L.cell[0].fit.nl);
}

/* a word's pieces that don't fit in what is left of the row start the next row, in order. */
static void test_value_pieces_wrap(void)
{
    sui_content_t c = {.lines = {"x y z abcdefghjk"}, .n = 1, .role = {{SUI_ROLE_LABEL, SUI_ROLE_LABEL, SUI_ROLE_LABEL, SUI_ROLE_VALUE}}};
    static sui_layout_t L;
    sui_layout(4, 2, 96, &c, &L);
    TEST_ASSERT_EQUAL_INT(1, L.pages);
    TEST_ASSERT_EQUAL_INT(5, L.n);
    const char *t = page_text(&L, &c, 4, 2, 0);
    TEST_ASSERT_EQUAL_STRING_LEN("x|y|z\n", t, 6);
    TEST_ASSERT_NOT_NULL(sui_layout_cell(&L, 0, 0, 1));
    TEST_ASSERT_NOT_NULL(sui_layout_cell(&L, 0, 1, 1));
    TEST_ASSERT_NULL(sui_layout_cell(&L, 0, 3, 0)); /* left blank: the pieces stay together */
}

/* sui_word_cells (for deckui) cuts one word exactly as sui_layout cuts it in a line of its own: a value
 * over keys at one size, another word in pieces that read, one cell when it reads. */
static void test_word_cells(void)
{
    static const struct { const char *w; int role, key; } T[] = {
        {"CompanionBox-8D1C", SUI_ROLE_VALUE, 72},
        {"k7m2p9xq", SUI_ROLE_VALUE, 96},
        {"k7m2p9xq", SUI_ROLE_VALUE, 112},
        {"FOH-PC", SUI_ROLE_VALUE, 80},
        {"abcdefghjkmnpqrstuvwxyz23456789abcdefghjkmnpqrstuvwxyz23456789a", SUI_ROLE_VALUE, 80},
        {"Reconnecting", SUI_ROLE_STATUS, 72},
        {"Supercalifragilisticexpialidocious", SUI_ROLE_LABEL, 72},
    };
    static sui_layout_t L;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        sui_content_t c;
        one_word(&c, T[i].w);
        memset(c.role[0], T[i].role, SUI_LINE_ROLES);
        sui_layout(16, 6, T[i].key, &c, &L);
        sui_cell_t out[SUI_CELLS_MAX];
        const int n = sui_word_cells(T[i].w, (int)strlen(T[i].w), T[i].key, T[i].role, out, SUI_CELLS_MAX);
        TEST_ASSERT_EQUAL_INT_MESSAGE(L.n, n, T[i].w);
        for (int k = 0; k < n; k++) {
            TEST_ASSERT_EQUAL_MESSAGE(L.cell[k].start, out[k].start, T[i].w);
            TEST_ASSERT_EQUAL_MESSAGE(L.cell[k].len, out[k].len, T[i].w);
            TEST_ASSERT_EQUAL_MESSAGE(L.cell[k].piece, out[k].piece, T[i].w);
            TEST_ASSERT_EQUAL_MESSAGE(T[i].role, out[k].role, T[i].w);
            TEST_ASSERT_EQUAL_MESSAGE(L.cell[k].fit.nl, out[k].fit.nl, T[i].w);
            TEST_ASSERT_EQUAL_MESSAGE(L.cell[k].fit.cut[0], out[k].fit.cut[0], T[i].w);
            if (T[i].role == SUI_ROLE_VALUE || n == 1) /* (sui_layout evens a label's pieces in its line) */
                TEST_ASSERT_EQUAL_MESSAGE(L.cell[k].fit.px, out[k].fit.px, T[i].w);
        }
    }
    sui_cell_t out[4];
    TEST_ASSERT_EQUAL_INT(2, sui_word_cells("CompanionBox-8D1C", 17, 72, SUI_ROLE_VALUE, out, 4));
    TEST_ASSERT_EQUAL_INT(9, out[0].len); /* "Companion" */
    TEST_ASSERT_EQUAL_INT(9, out[1].start);
    TEST_ASSERT_EQUAL_INT(out[0].fit.px, out[1].fit.px); /* one size */
    TEST_ASSERT_EQUAL_INT(1, sui_word_cells("CompanionBox-8D1C", 17, 72, SUI_ROLE_VALUE, out, 1)); /* max */
    TEST_ASSERT_EQUAL_INT(9, out[0].len);
    TEST_ASSERT_EQUAL_INT(1, sui_word_cells("FOH-PC", 6, 80, SUI_ROLE_VALUE, out, 4)); /* reads in one */
    TEST_ASSERT_EQUAL_INT(0, sui_word_cells("x", 0, 80, SUI_ROLE_VALUE, out, 4));
    TEST_ASSERT_EQUAL_INT(0, sui_word_cells(NULL, 3, 80, SUI_ROLE_VALUE, out, 4));
    TEST_ASSERT_EQUAL_INT(0, sui_word_cells("abc", 3, 80, SUI_ROLE_VALUE, out, 0));
}

/* A 4-digit number (a dock port) is never split, on any model's keys. */
static void test_fit_four_digits_whole(void)
{
    for (size_t k = 0; k < sizeof KEY_PX / sizeof KEY_PX[0]; k++)
        for (int v = 1000; v <= 9999; v++) {
            char w[5];
            snprintf(w, sizeof w, "%d", v);
            const sui_fit_t f = sui_fit(w, 4, KEY_PX[k], false);
            TEST_ASSERT_EQUAL_INT(1, f.nl);
            TEST_ASSERT_EQUAL_INT(4, f.cut[0]);
            TEST_ASSERT_TRUE(f.px >= SUI_MIN_PX);
        }
}

/* Nothing to draw: an empty word, a key too small for any size, a word past SUI_FIT_MAX. */
static void test_fit_nothing(void)
{
    TEST_ASSERT_EQUAL_INT(0, sui_fit("", 0, 72, true).px);
    TEST_ASSERT_EQUAL_INT(0, sui_fit("abc", 3, 20, true).px); /* the cap (8 px) is below the smallest size */
    static char w[SUI_FIT_MAX + 2];
    memset(w, 'a', sizeof w - 1);
    TEST_ASSERT_EQUAL_INT(0, sui_fit(w, SUI_FIT_MAX + 1, 120, true).px);
    TEST_ASSERT_EQUAL_INT(0, sui_text_width(13, "abc", 3, NULL)); /* no such size */
    TEST_ASSERT_EQUAL_INT(0, sui_text_width(14, "abc", 0, NULL));
    /* a key with an empty word draws nothing */
    sui_content_t c;
    one_word(&c, "x");
    static uint8_t buf[20 * 20 * 3];
    sui_render_key(1, 1, 20, &c, 0, 0, 0, buf);
    TEST_ASSERT_EQUAL_INT(20 * 20, count_px(buf, 20 * 20, c.bg));
}

/* True when s[0..len) reads in one key: SUI_MIN_PX or more in 1 or 2 lines. */
static bool fits_readable(const char *s, int len, int key_px)
{
    return one_line_px(s, len, key_px) >= SUI_MIN_PX || (len >= 2 && lines_px(s, len, 0, key_px) >= SUI_MIN_PX);
}

/* The words of line `line` of L, in order: on keys one after the other in reading order (across rows
 * and pages), each piece whole and within its key. Writes the line's text back together (pieces of
 * one word joined, words joined by ' ') to out; returns the keys it takes. */
static int line_back(const sui_layout_t *L, const sui_content_t *c, int cols, int rows, int px, int line,
                     char *out)
{
    size_t u = 0;
    int keys = 0, prev = -1, end = -1;
    for (int k = 0; k < L->n; k++) {
        const sui_cell_t *e = &L->cell[k];
        if (e->line != line) continue;
        const int pos = (e->page * rows + e->row) * cols + e->col;
        if (prev >= 0) TEST_ASSERT_TRUE(pos == prev + 1 || (pos > prev && e->col == 0)); /* in order (a word's pieces may start a row) */
        prev = pos;
        assert_fit_ok(c->lines[line] + e->start, e->len, &e->fit, px);
        TEST_ASSERT_TRUE(e->fit.px >= SUI_MIN_PX);
        if (end >= 0 && e->start > end) out[u++] = ' '; /* a new word, not the next piece of this one */
        memcpy(out + u, c->lines[line] + e->start, e->len);
        u += e->len;
        end = e->start + e->len;
        assert_key_inside(cols, rows, px, c, e->page, e->col, e->row);
        keys++;
    }
    out[u] = '\0';
    return keys;
}

/* a word one key can't hold readably (SUI_MIN_PX in 2 lines) goes on over the next keys of
 * the row, then the next row, in the fewest pieces that each read, even when it has no '-' or '.': in
 * order, nothing added, nothing dropped (a deck never shows a wrong password). */
static void test_layout_long_word_continues(void)
{
    static const char abc[] = "abcdefghjkmnpqrstuvwxyz23456789";
    char pass[64], want[80];
    static char got[400];
    for (int i = 0; i < 63; i++) pass[i] = abc[i % 31];
    pass[63] = '\0';
    static const struct { int cols, rows, px; } D[] = {{5, 3, 72}, {8, 4, 96}, {4, 2, 120}}; /* MK.2, XL, Plus */
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) {
        sui_content_t c;
        fx_screen_looking(&c, "192.168.100.200", NULL, "CompanionBox-ABCD", pass);
        static sui_layout_t L;
        sui_layout(D[i].cols, D[i].rows, D[i].px, &c, &L);
        const int keys = line_back(&L, &c, D[i].cols, D[i].rows, D[i].px, 4, got); /* the Pass line (4) */
        snprintf(want, sizeof want, "Pass %s", pass);
        TEST_ASSERT_EQUAL_STRING(want, got);
        /* a value: over keys, one line each, at one comfortable size */
        TEST_ASSERT_EQUAL_INT(keys - 1, assert_value_pieces(&L, &c, 4, 1, D[i].px, pass));
        TEST_ASSERT_TRUE(keys - 1 >= 2);
        for (int k = 0; k < L.n; k++)
            if (L.cell[k].line == 4 && L.cell[k].start > 0) TEST_ASSERT_TRUE(L.cell[k].fit.px >= sui_comfort_px(D[i].px));
    }

    /* on a deck of one key: one piece per page */
    char w[41];
    memset(w, 'm', 40);
    w[40] = '\0';
    sui_content_t c;
    one_word(&c, w);
    static sui_layout_t L;
    sui_layout(1, 1, 72, &c, &L);
    TEST_ASSERT_TRUE(L.pages >= 2);
    TEST_ASSERT_EQUAL_INT(L.pages, line_back(&L, &c, 1, 1, 72, 0, got));
    TEST_ASSERT_EQUAL_STRING(w, got);

    /* a word longer than SUI_FIT_MAX (no screen has one): still whole, in order, over the keys */
    static char huge[201];
    for (int i = 0; i < 200; i++) huge[i] = abc[i % 31];
    huge[200] = '\0';
    one_word(&c, huge);
    sui_layout(4, 4, 96, &c, &L);
    TEST_ASSERT_TRUE(L.pages >= 1);
    line_back(&L, &c, 4, 4, 96, 0, got);
    TEST_ASSERT_EQUAL_STRING(huge, got);

    /* what one key holds readably stays one piece */
    one_word(&c, "abcdefghjkmnpqrs");
    TEST_ASSERT_TRUE(fits_readable(c.lines[0], 16, 72));
    sui_layout(1, 1, 72, &c, &L);
    TEST_ASSERT_EQUAL_INT(1, L.n);

    /* pieces end after the word's own '-' or '.' when they can: a host name on an MK.2 */
    fx_screen_looking(&c, "10.0.0.7", "companion-main.studio-b.example.internal", NULL, NULL);
    sui_layout(5, 3, 72, &c, &L);
    TEST_ASSERT_TRUE(line_back(&L, &c, 5, 3, 72, 1, got) >= 2); /* the target (line 1) in pieces */
    TEST_ASSERT_EQUAL_STRING("companion-main.studio-b.example.internal", got);
    for (int k = 0; k < L.n; k++) {
        const sui_cell_t *e = &L.cell[k];
        if (e->line == 1 && e->start > 0) TEST_ASSERT_TRUE(c.lines[1][e->start - 1] == '-' || c.lines[1][e->start - 1] == '.');
    }
}

/* lines marked keep-with-next share a page unless the group alone is longer than the deck. */
static void test_layout_keep_with_next(void)
{
    sui_content_t c;
    static sui_layout_t L;
    fx_screen_looking(&c, "192.168.8.225", NULL, "CompanionBox-ABCD", "abcdefghjk");
    sui_layout(5, 3, 72, &c, &L); /* MK.2: the hotspot name and password on one page, at the bottom */
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\nBox|IP\n192|168|8|225", page_text(&L, &c, 5, 3, 0));
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\nWiFi|Companion|Box-ABCD\nPass|abcde|fghjk", page_text(&L, &c, 5, 3, 1));

    /* every screen that shows the hotspot marks its WiFi line */
    fx_screen_no_network(&c, "192.168.4.1", "S", "P"); /* and the label line with its value */
    TEST_ASSERT_EQUAL_HEX8((1u << 1) | (1u << 3), c.keep_next);
    fx_screen_looking(&c, "1.2.3.4", NULL, "S", "P");
    TEST_ASSERT_EQUAL_HEX8((1u << 1) | (1u << 3), c.keep_next);
    fx_screen_dock(&c, "1.2.3.4", 5343, "S", "P");
    TEST_ASSERT_EQUAL_HEX8((1u << 1) | (1u << 3), c.keep_next);
    fx_screen_dock(&c, "1.2.3.4", 5343, NULL, NULL);
    TEST_ASSERT_EQUAL_HEX8(1u << 1, c.keep_next);

    sui_content_t d = {.lines = {"a", "b", "c"}, .n = 3, .keep_next = 1u << 1};
    sui_layout(3, 2, 80, &d, &L);
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("a\n", page_text(&L, &d, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("b\nc", page_text(&L, &d, 3, 2, 1));
    d.keep_next = 0;
    sui_layout(3, 2, 80, &d, &L);
    TEST_ASSERT_EQUAL_STRING("a\nb", page_text(&L, &d, 3, 2, 0));
    d.keep_next = 1u << 1; /* a group longer than the deck: page breaks at line ends as usual */
    sui_layout(3, 1, 80, &d, &L);
    TEST_ASSERT_EQUAL_INT(3, L.pages);
    TEST_ASSERT_EQUAL_STRING("b", page_text(&L, &d, 3, 1, 1));
}


/* Every word the box's screens really show fits whole and readable on every model's keys (sizes and
 * grids from deckmodel), and every key draws inside its margin. */
static void test_layout_real_words_fit_every_model(void)
{
    static const struct { uint16_t pid; int cols, rows, px; } M[] = {
        {0x0063, 3, 2, 80},  /* Mini */
        {0x00B8, 3, 2, 80},  /* Module 6 */
        {0x0060, 5, 3, 72},  /* Original */
        {0x0080, 5, 3, 72},  /* MK.2 */
        {0x00B9, 5, 3, 72},  /* Module 15 */
        {0x006C, 8, 4, 96},  /* XL */
        {0x00BA, 8, 4, 96},  /* Module 32 */
        {0x009A, 4, 2, 96},  /* Neo */
        {0x0084, 4, 2, 120}, /* Plus */
        {0x00C6, 9, 4, 112}, /* Plus XL */
    };
    static const char *const ver[] = {"4.2.0", "4.0.0-beta.2+7156-main-1234abcd", "abcdefghijklmnopqrstuvwxyz01234"};
    TEST_ASSERT_EQUAL_INT(2, SUI_KEY_LINES); /* at most 2 text lines in a key (each cell: assert_fit_ok) */
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
        const deck_model_t *m = deck_model_find(DECK_VID_ELGATO, M[i].pid);
        TEST_ASSERT_NOT_NULL(m);
        TEST_ASSERT_EQUAL_INT(M[i].cols, m->cols);
        TEST_ASSERT_EQUAL_INT(M[i].rows, m->rows);
        TEST_ASSERT_EQUAL_INT(M[i].px, m->key_px);

        sui_content_t c[16];
        int n = 0;
        sui_screen_message(&c[n++], "Reconnecting Disconnected\nInternationalisation"); /* FG words: no '-' */
        fx_screen_no_network(&c[n++], "192.168.4.1", "CompanionBox-ABCD", "abcdefghjk");
        fx_screen_no_network(&c[n++], "192.168.4.1", "CompanionBox-ABCD", NULL); /* open hotspot */
        fx_screen_looking(&c[n++], "192.168.100.200", NULL, "CompanionBox-ABCD", NULL);
        fx_screen_dock(&c[n++], "192.168.100.200", 5353, "CompanionBox-ABCD", NULL);
        fx_screen_looking(&c[n++], "192.168.100.200", NULL, "CompanionBox-ABCD", "abcdefghjk");
        fx_screen_looking(&c[n++], "10.0.0.7", "192.168.100.200", NULL, NULL);
        fx_screen_choose(&c[n++], "255.255.255.255", NULL, NULL);
        for (size_t v = 0; v < sizeof ver / sizeof ver[0]; v++) fx_screen_too_old(&c[n++], ver[v], NULL, NULL);
        sui_screen_reconnecting(&c[n++], NULL);
        fx_screen_dock(&c[n++], "192.168.100.200", 5353, NULL, NULL);
        fx_screen_dock(&c[n++], "192.168.100.200", 5353, "CompanionBox-ABCD", "abcdefghjk");
        fx_screen_no_dock(&c[n++]);
        fx_screen_looking(&c[n++], "192.168.100.200", "companion-main.studio-b.example.internal",
                           "CompanionBox-ABCD", "abcdefghjkmnpqrstuvwxyz23456789abcdefghjkmnpqrstuvwxyz23456789a");
        static sui_layout_t L;
        for (int s = 0; s < n; s++) {
            sui_layout(m->cols, m->rows, m->key_px, &c[s], &L);
            TEST_ASSERT_TRUE(L.n > 0);
            for (int k = 0; k < L.n; k++) {
                const sui_cell_t *e = &L.cell[k];
                assert_fit_ok(c[s].lines[e->line] + e->start, e->len, &e->fit, m->key_px); /* whole */
                TEST_ASSERT_TRUE(e->fit.px >= SUI_MIN_PX);                                 /* readable */
                if (e->role != SUI_ROLE_STATUS && e->role != SUI_ROLE_LABEL) TEST_ASSERT_EQUAL_INT(0, e->fit.hyph);
                TEST_ASSERT_TRUE((e->page < L.pages || e->page == SUI_PAGE_ALL) && e->row < m->rows && e->col < m->cols);
            }
            for (int p = 0; p < L.pages; p++)
                for (int r = 0; r < m->rows; r++)
                    for (int k = 0; k < m->cols; k++) assert_key_inside(m->cols, m->rows, m->key_px, &c[s], p, k, r);
        }
    }
}

/* ---- roles ---- */

/* The words of c laid out on cols x rows keys of key_px, each with its role's letter: "Add:L in:L". */
static const char *roles_text(const sui_content_t *c, int cols, int rows, int key_px)
{
    static const char letter[SUI_ROLES] = {'F', 'S', 'L', 'A', 'V'};
    static sui_layout_t L;
    static char out[1024];
    sui_layout(cols, rows, key_px, c, &L);
    size_t u = 0;
    for (int k = 0; k < L.n; k++) {
        const sui_cell_t *e = &L.cell[k];
        u += (size_t)snprintf(out + u, sizeof out - u, "%s%.*s:%c", k ? " " : "", e->len, c->lines[e->line] + e->start,
                              e->role < SUI_ROLES ? letter[e->role] : '?');
    }
    out[u] = '\0';
    return out;
}

static void test_roles_per_screen(void)
{
    sui_content_t c;
    fx_screen_dock(&c, "192.168.8.225", 5343, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("Waiting:S for:S Companion:S Remote:L surface:L 192:A 168:A 8:A 225:A port:L 5343:A",
                             roles_text(&c, 8, 4, 96));
    fx_screen_dock(&c, "10.0.0.7", 5345, "CompanionBox-ABCD", "abcdefghjk");
    TEST_ASSERT_EQUAL_STRING("Waiting:S for:S Companion:S Remote:L surface:L 10:A 0:A 0:A 7:A port:L 5345:A WiFi:L "
                             "Companion:V Box-ABCD:V Pass:L abcde:V fghjk:V", roles_text(&c, 8, 4, 96));
    fx_screen_dock(&c, "10.0.0.7", 5345, "My Box", NULL); /* an SSID with a space; an open hotspot */
    TEST_ASSERT_EQUAL_STRING("Waiting:S for:S Companion:S Remote:L surface:L 10:A 0:A 0:A 7:A port:L 5345:A WiFi:L My:V "
                             "Box:V No:L password:L", roles_text(&c, 8, 4, 96));
    fx_screen_dock(&c, NULL, 5353, NULL, NULL); /* no IP yet: "port" is still the label */
    TEST_ASSERT_EQUAL_STRING("Waiting:S for:S Companion:S Remote:L surface:L port:L 5353:A", roles_text(&c, 8, 4, 96));

    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-ABCD", "pass word");
    TEST_ASSERT_EQUAL_STRING("No:S network:S Setup:L page:L 192:A 168:A 4:A 1:A WiFi:L Companion:V Box-ABCD:V Pass:L pass:V "
                             "word:V", roles_text(&c, 8, 4, 96));
    fx_screen_no_network(&c, "192.168.4.1", NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("No:S network:S Plug:L in:L network:L", roles_text(&c, 8, 4, 96));
    fx_screen_looking(&c, "192.168.8.225", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("Finding:S Companion:S Box:L IP:L 192:A 168:A 8:A 225:A", roles_text(&c, 8, 4, 96));
    fx_screen_looking(&c, "192.168.8.225", "companion.local", NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("Connecting:S companion.:V local:V Box:L IP:L 192:A 168:A 8:A 225:A", roles_text(&c, 8, 4, 96));
    fx_screen_looking(&c, "192.168.8.225", "192.168.8.10", NULL, NULL); /* an address: its octets in accent */
    TEST_ASSERT_EQUAL_STRING("Connecting:S 192:A 168:A 8:A 10:A Box:L IP:L 192:A 168:A 8:A 225:A", roles_text(&c, 8, 4, 96));
    fx_screen_choose(&c, "192.168.8.225", NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("Choose:S Companion:S Web:L page:L 192:A 168:A 8:A 225:A", roles_text(&c, 8, 4, 96));
    fx_screen_too_old(&c, "4.2.0", NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("Companion:S too:S old:S Needs:L 5.0+:V Has:L 4.2.0:V", roles_text(&c, 8, 4, 96));
    sui_screen_reconnecting(&c, NULL);
    TEST_ASSERT_EQUAL_STRING("Reconnecting:S", roles_text(&c, 8, 4, 96));
    sui_screen_reconnecting(&c, "192.168.8.10");
    TEST_ASSERT_EQUAL_STRING("Reconnecting:S 192:A 168:A 8:A 10:A", roles_text(&c, 8, 4, 96));
    fx_screen_no_dock(&c);
    TEST_ASSERT_EQUAL_STRING("No:S free:S dock:S Max:L 6:L decks:L", roles_text(&c, 8, 4, 96));
    sui_screen_message(&c, "Back at 10.0.0.12\nport 5343");
    TEST_ASSERT_EQUAL_STRING("Back:F at:F 10:F 0:F 0:F 12:F port:F 5343:F", roles_text(&c, 8, 4, 96));

    /* every piece of a word too long for one key keeps its role */
    char pass[64];
    memset(pass, 'p', 63);
    pass[63] = '\0';
    fx_screen_no_network(&c, "192.168.4.1", "S", pass);
    static sui_layout_t L;
    sui_layout(5, 3, 72, &c, &L);
    int pieces = 0;
    for (int k = 0; k < L.n; k++)
        if (L.cell[k].line == 4 && L.cell[k].start > 0) { /* the Pass line */
            TEST_ASSERT_EQUAL_INT(SUI_ROLE_VALUE, L.cell[k].role);
            pieces++;
        }
    TEST_ASSERT_TRUE(pieces >= 2);
}

static void rgb_of(uint32_t hex, uint8_t *rgb)
{
    rgb[0] = (uint8_t)(hex >> 16);
    rgb[1] = (uint8_t)(hex >> 8);
    rgb[2] = (uint8_t)hex;
}

/* WCAG 2 relative luminance and contrast ratio. */
static double lin(int v)
{
    const double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double luminance(const uint8_t *rgb) { return 0.2126 * lin(rgb[0]) + 0.7152 * lin(rgb[1]) + 0.0722 * lin(rgb[2]); }
static double contrast(const uint8_t *a, const uint8_t *b)
{
    const double la = luminance(a) + 0.05, lb = luminance(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

/* The colours come from the palette: the state's colour for status words, cyan for addresses,
 * grey for labels, white for values; the content's fg otherwise. */
static void test_role_colours(void)
{
    sui_content_t c = {.fg = {1, 2, 3}};
    uint8_t want[3];
    static const struct { int state; uint32_t rgb; } S[] = {
        {SUI_STATE_NONE, SUI_RGB_PLAIN}, {SUI_STATE_BAD, SUI_RGB_BAD}, {SUI_STATE_WAIT, SUI_RGB_WAIT}, {SUI_STATE_OK, SUI_RGB_OK}};
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
        c.state = (uint8_t)S[i].state;
        rgb_of(S[i].rgb, want);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sui_role_rgb(&c, SUI_ROLE_STATUS), 3);
    }
    rgb_of(SUI_RGB_LABEL, want);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sui_role_rgb(&c, SUI_ROLE_LABEL), 3);
    rgb_of(SUI_RGB_ACCENT, want);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sui_role_rgb(&c, SUI_ROLE_ACCENT), 3);
    rgb_of(SUI_RGB_VALUE, want);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, sui_role_rgb(&c, SUI_ROLE_VALUE), 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(c.fg, sui_role_rgb(&c, SUI_ROLE_FG), 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(c.fg, sui_role_rgb(&c, 99), 3);
    /* the design's figures: label #9AA0A6, values white */
    TEST_ASSERT_EQUAL_HEX32(0x9AA0A6, SUI_RGB_LABEL);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, SUI_RGB_VALUE);
}

/* the screens' states and their lines' tints: the status line the state's, the IP and port navy,
 * the hotspot purple, blank keys and custom messages c->bg. */
static void test_screen_states_and_groups(void)
{
    static const struct { const char *name; int state; } W[] = {
        {"no network", SUI_STATE_BAD}, {"too old", SUI_STATE_BAD}, {"no dock", SUI_STATE_BAD}, {"looking", SUI_STATE_WAIT},
        {"connecting", SUI_STATE_WAIT}, {"choose", SUI_STATE_WAIT}, {"reconnecting", SUI_STATE_WAIT}, {"dock", SUI_STATE_WAIT},
        {"message", SUI_STATE_NONE},
    };
    sui_content_t c[9];
    fx_screen_no_network(&c[0], "192.168.4.1", "S", NULL);
    fx_screen_too_old(&c[1], "4.2.0", NULL, NULL);
    fx_screen_no_dock(&c[2]);
    fx_screen_looking(&c[3], "1.2.3.4", NULL, "S", "P");
    fx_screen_looking(&c[4], "1.2.3.4", "host", NULL, NULL);
    fx_screen_choose(&c[5], "1.2.3.4", NULL, NULL);
    sui_screen_reconnecting(&c[6], NULL);
    fx_screen_dock(&c[7], "1.2.3.4", 5343, "S", "P");
    sui_screen_message(&c[8], "Hello");
    for (int i = 0; i < 9; i++) TEST_ASSERT_EQUAL_INT_MESSAGE(W[i].state, c[i].state, W[i].name);

    uint8_t wait[3], navy[3], purple[3], bad[3];
    rgb_of(SUI_RGB_TINT_WAIT, wait);
    rgb_of(SUI_RGB_TINT_MAIN, navy);
    rgb_of(SUI_RGB_TINT_SERVICE, purple);
    rgb_of(SUI_RGB_TINT_BAD, bad);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(wait, sui_line_bg(&c[7], 0), 3);   /* Waiting for Companion */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(navy, sui_line_bg(&c[7], 1), 3);   /* main: Remote surface */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(navy, sui_line_bg(&c[7], 2), 3);   /* main: <ip> port <port> */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(purple, sui_line_bg(&c[7], 3), 3); /* service: WiFi <ssid> */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(purple, sui_line_bg(&c[7], 4), 3); /* service: Pass <pass> */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(wait, sui_line_bg(&c[3], 0), 3);   /* Finding Companion */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(navy, sui_line_bg(&c[3], 2), 3);   /* the box's IP */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bad, sui_line_bg(&c[0], 0), 3);    /* No network */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(navy, sui_line_bg(&c[0], 1), 3);   /* Setup page */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(purple, sui_line_bg(&c[0], 4), 3); /* No password */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(navy, sui_line_bg(&c[5], 2), 3);   /* choose: the IP */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(c[8].bg, sui_line_bg(&c[8], 0), 3); /* a message: its own */

    /* drawn: a word's key all its line's tint around the glyphs, a blank key c->bg */
    static uint8_t buf[96 * 96 * 3];
    sui_render_key(8, 4, 96, &c[7], 0, 0, 2, buf); /* "1" of the IP */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(navy, buf, 3);
    sui_render_key(8, 4, 96, &c[7], 0, 0, 3, buf); /* "WiFi", the bottom row */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(purple, buf + 3 * (96 * 96 - 1), 3);
    sui_render_key(8, 4, 96, &c[7], 0, 7, 0, buf); /* blank */
    TEST_ASSERT_EQUAL_INT(96 * 96, count_px(buf, 96 * 96, c[7].bg));
}

/* every word the screens draw stands out from its key at WCAG AA (4.5:1) or more, and the tints
 * are very dark. */
static void test_palette_contrast(void)
{
    static const uint32_t tint[] = {SUI_RGB_TINT_BAD, SUI_RGB_TINT_WAIT, SUI_RGB_TINT_OK, SUI_RGB_TINT_MAIN, SUI_RGB_TINT_SERVICE};
    static const uint32_t text[] = {SUI_RGB_VALUE, SUI_RGB_LABEL, SUI_RGB_ACCENT, SUI_RGB_BAD, SUI_RGB_WAIT, SUI_RGB_OK, SUI_RGB_PLAIN};
    uint8_t a[3], b[3];
    for (size_t t = 0; t < sizeof tint / sizeof tint[0]; t++) {
        rgb_of(tint[t], b);
        TEST_ASSERT_TRUE(luminance(b) < 0.03); /* very dark */
        for (size_t k = 0; k < sizeof text / sizeof text[0]; k++) { /* any text on any tint */
            rgb_of(text[k], a);
            TEST_ASSERT_TRUE(contrast(a, b) >= 4.5);
        }
    }
    /* and what the screens really draw, the OK state included */
    sui_content_t c[10];
    int n = 0;
    fx_screen_no_network(&c[n++], "192.168.4.1", "CompanionBox-ABCD", "abcdefghjk");
    fx_screen_no_network(&c[n++], "192.168.4.1", "CompanionBox-ABCD", NULL);
    fx_screen_looking(&c[n++], "192.168.8.225", NULL, "CompanionBox-ABCD", "abcdefghjk");
    fx_screen_looking(&c[n++], "192.168.8.225", "192.168.8.10", NULL, NULL);
    fx_screen_choose(&c[n++], "192.168.8.225", NULL, NULL);
    fx_screen_too_old(&c[n++], "4.2.0", NULL, NULL);
    sui_screen_reconnecting(&c[n++], NULL);
    fx_screen_dock(&c[n++], "192.168.8.225", 5343, "CompanionBox-ABCD", "abcdefghjk");
    fx_screen_no_dock(&c[n++]);
    fx_screen_looking(&c[n], "192.168.8.225", NULL, "S", "P");
    c[n++].state = SUI_STATE_OK;
    static sui_layout_t L;
    for (int i = 0; i < n; i++) {
        sui_layout(8, 4, 96, &c[i], &L);
        for (int k = 0; k < L.n; k++) {
            const sui_cell_t *e = &L.cell[k];
            char msg[48];
            snprintf(msg, sizeof msg, "screen %d word %d", i, k);
            TEST_ASSERT_TRUE_MESSAGE(contrast(sui_role_rgb(&c[i], e->role), sui_line_bg(&c[i], e->line)) >= 4.5, msg);
        }
    }
}

/* each screen in the fewest words that still say what to do (the zones add a label to main).
 * Its words, the IP, port, SSID, password and versions left out (value and accent), are at most these;
 * they can't creep back. */
static void test_screen_word_counts(void)
{
    sui_content_t c[12];
    static const int most[12] = {5, 6, 7, 4, 6, 3, 4, 5, 1, 6, 8, 6};
    int n = 0;
    fx_screen_no_network(&c[n++], "192.168.4.1", NULL, NULL);                        /* No network, Plug in network */
    fx_screen_no_network(&c[n++], "192.168.4.1", "CompanionBox-ABCD", "abcdefghjk"); /* + Setup page, WiFi, Pass */
    fx_screen_no_network(&c[n++], "192.168.4.1", "CompanionBox-ABCD", NULL);         /* + Setup page, WiFi, No password */
    fx_screen_looking(&c[n++], "192.168.8.225", NULL, NULL, NULL);                   /* Finding Companion, Box IP */
    fx_screen_looking(&c[n++], "192.168.8.225", NULL, "CompanionBox-ABCD", "p");     /* + WiFi, Pass */
    fx_screen_looking(&c[n++], "192.168.8.225", "192.168.8.10", NULL, NULL);         /* Connecting, Box IP */
    fx_screen_choose(&c[n++], "192.168.8.225", NULL, NULL);                          /* Choose Companion, Web page */
    fx_screen_too_old(&c[n++], "4.2.0", NULL, NULL);                                 /* Companion too old, Needs, Has */
    sui_screen_reconnecting(&c[n++], NULL);                                           /* Reconnecting */
    fx_screen_dock(&c[n++], "192.168.8.225", 5343, NULL, NULL);                      /* Waiting for Companion, Remote surface, port */
    fx_screen_dock(&c[n++], "192.168.8.225", 5343, "CompanionBox-ABCD", "p");        /* + WiFi, Pass */
    fx_screen_no_dock(&c[n++]);                                                      /* No free dock, Max 6 decks */
    static sui_layout_t L;
    for (int i = 0; i < n; i++) {
        sui_layout(8, 4, 96, &c[i], &L);
        int words = 0;
        for (int k = 0; k < L.n; k++) words += L.cell[k].role != SUI_ROLE_VALUE && L.cell[k].role != SUI_ROLE_ACCENT;
        char msg[16];
        snprintf(msg, sizeof msg, "screen %d", i);
        TEST_ASSERT_TRUE_MESSAGE(words <= most[i], msg);
    }
}

/* ---- zones ---- */

/* The groups of c's lines as letters: S status, M main, V service, - none. */
static const char *zones_of(const sui_content_t *c)
{
    static char out[8];
    static const char letter[] = {'-', 'S', 'M', 'V'};
    int i = 0;
    for (; i < c->n; i++) out[i] = c->group[i] < 4 ? letter[c->group[i]] : '?';
    out[i] = '\0';
    return out;
}

/* Each status screen: a status line on top, the main lines, the hotspot's lines as service. */
static void test_zones_per_screen(void)
{
    sui_content_t c;
    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", "abcdefghjk");
    TEST_ASSERT_EQUAL_STRING("SMMVV", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("No network", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Setup page", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", c.lines[2]);
    TEST_ASSERT_EQUAL_STRING("WiFi CompanionBox-8D1C", c.lines[3]);
    TEST_ASSERT_EQUAL_STRING("Pass abcdefghjk", c.lines[4]);
    fx_screen_no_network(&c, "192.168.4.1", NULL, NULL); /* the hotspot off: a hint, no service */
    TEST_ASSERT_EQUAL_STRING("SM", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("Plug in network", c.lines[1]);
    fx_screen_no_network(&c, "", "S", NULL); /* no address for the setup page: the hint, not "Setup page" */
    TEST_ASSERT_EQUAL_STRING("SMVV", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("Plug in network", c.lines[1]);
    fx_screen_no_network(&c, NULL, "S", NULL);
    TEST_ASSERT_EQUAL_STRING("Plug in network", c.lines[1]);
    fx_screen_looking(&c, "192.168.8.225", NULL, "S", NULL);
    TEST_ASSERT_EQUAL_STRING("SMMVV", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("Finding Companion", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Box IP", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225", c.lines[2]);
    TEST_ASSERT_EQUAL_STRING("No password", c.lines[4]);
    fx_screen_looking(&c, "192.168.8.225", "companion.local", "S", "P");
    TEST_ASSERT_EQUAL_STRING("SMMMVV", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("Connecting", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("companion.local", c.lines[1]);
    fx_screen_choose(&c, "192.168.8.225", NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("SMM", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("Choose Companion", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Web page", c.lines[1]);
    fx_screen_too_old(&c, "4.2.0", "S", "P");
    TEST_ASSERT_EQUAL_STRING("SMMVV", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("Companion too old", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Needs 5.0+", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("Has 4.2.0", c.lines[2]);
    TEST_ASSERT_EQUAL_INT(SUI_STATE_BAD, c.state);
    fx_screen_dock(&c, "192.168.8.225", 5343, "S", "P");
    TEST_ASSERT_EQUAL_STRING("SMMVV", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("Waiting for Companion", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Remote surface", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225 port 5343", c.lines[2]);
    fx_screen_no_dock(&c);
    TEST_ASSERT_EQUAL_STRING("SM", zones_of(&c));
    sui_screen_reconnecting(&c, "192.168.8.10");
    TEST_ASSERT_EQUAL_STRING("SM", zones_of(&c));
    TEST_ASSERT_EQUAL_STRING("192.168.8.10", c.lines[1]);
    sui_screen_reconnecting(&c, NULL);
    TEST_ASSERT_EQUAL_STRING("S", zones_of(&c));
    sui_screen_message(&c, "Hello\nWorld"); /* a custom message has no zones */
    TEST_ASSERT_EQUAL_STRING("--", zones_of(&c));
}

/* The status row on every page; the service at the bottom; empty rows between main and service. */
static void test_zones_layout(void)
{
    sui_content_t c;
    static sui_layout_t L;
    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", "abcdefghjk");
    sui_layout(8, 4, 96, &c, &L); /* Module 32: everything on one page, the hotspot's lines in one row */
    TEST_ASSERT_EQUAL_INT(1, L.pages);
    TEST_ASSERT_EQUAL_STRING("No|network\nSetup|page\n192|168|4|1\nWiFi|Companion|Box-8D1C|Pass|abcde|fghjk",
                             page_text(&L, &c, 8, 4, 0));

    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", NULL);
    sui_layout(5, 3, 72, &c, &L); /* MK.2: status + main, then status + service at the bottom */
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("No|network\nSetup|page\n192|168|4|1", page_text(&L, &c, 5, 3, 0));
    TEST_ASSERT_EQUAL_STRING("No|network\n\nWiFi|Companion|Box-8D1C|No|password", page_text(&L, &c, 5, 3, 1)); /* a spacer row */
    for (int k = 0; k < 5; k++) TEST_ASSERT_NULL(sui_layout_cell(&L, 1, k, 1));

    /* a spare row on one page: status, one main line, the spacer, the service at the bottom */
    sui_screen_reconnecting(&c, "192.168.8.10");
    sui_content_t d = c;
    d.lines[2] = "WiFi S";
    d.lines[3] = "No password";
    d.group[2] = d.group[3] = SUI_GROUP_SERVICE;
    d.role[2][0] = d.role[3][0] = d.role[3][1] = SUI_ROLE_LABEL;
    d.role[2][1] = SUI_ROLE_VALUE;
    d.n = 4;
    sui_layout(8, 4, 96, &d, &L);
    TEST_ASSERT_EQUAL_INT(1, L.pages);
    TEST_ASSERT_EQUAL_STRING("Reconnecting\n192|168|8|10\n\nWiFi|S|No|password", page_text(&L, &d, 8, 4, 0));

    /* the hotspot off: no service, main right under the status */
    fx_screen_dock(&c, "192.168.8.225", 5343, NULL, NULL);
    sui_layout(8, 4, 96, &c, &L);
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nRemote|surface\n192|168|8|225|port|5343\n", page_text(&L, &c, 8, 4, 0));
}

/* Small decks page through it: every page keeps the status row; main first, then the service. */
static void test_zones_small_decks(void)
{
    sui_content_t c;
    static sui_layout_t L;
    fx_screen_dock(&c, "192.168.8.225", 5343, "CompanionBox-8D1C", "abcdefghjk");
    sui_layout(5, 3, 72, &c, &L); /* MK.2 */
    TEST_ASSERT_EQUAL_INT(3, L.pages);
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nRemote|surface\n", page_text(&L, &c, 5, 3, 0));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\n192|168|8|225\nport|5343", page_text(&L, &c, 5, 3, 1));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nWiFi|Companion|Box-8D1C\nPass|abcde|fghjk", page_text(&L, &c, 5, 3, 2));

    sui_layout(3, 2, 80, &c, &L); /* Mini: one row under the status */
    for (int p = 0; p < L.pages; p++) {
        TEST_ASSERT_NOT_NULL(sui_layout_cell(&L, p, 0, 0));
        TEST_ASSERT_EQUAL_STRING_LEN("Waiting|for|Companion\n", page_text(&L, &c, 3, 2, p), 22);
    }
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nRemote|surface", page_text(&L, &c, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nWiFi|Companion|Box-8D1C", page_text(&L, &c, 3, 2, L.pages - 2));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nPass|abcde|fghjk", page_text(&L, &c, 3, 2, L.pages - 1));

    /* a deck of one key (the overlay's layout): the status word alone */
    sui_screen_reconnecting(&c, "192.168.8.10");
    sui_layout(1, 1, 72, &c, &L);
    const sui_cell_t *e = sui_layout_cell(&L, 0, 0, 0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(0, e->line);
}

/* Nothing the old screens showed is lost: the IPs, port, SSID, password, versions, the target. */
static void test_zones_keep_information(void)
{
    static const struct { int cols, rows, px; } D[] = {{3, 2, 80}, {5, 3, 72}, {8, 4, 96}, {4, 2, 120}};
    for (size_t d = 0; d < sizeof D / sizeof D[0]; d++) {
        sui_content_t c[6];
        static const char *const want[6][6] = {
            {"No", "network", "192", "1", "CompanionBox-8D1C", "abcdefghjk"},
            {"Finding", "Companion", "225", "CompanionBox-8D1C", "No", "password"},
            {"Connecting", "companion.local", "225", NULL},
            {"Choose", "Companion", "192", "225", NULL},
            {"too", "old", "5.0+", "4.2.0", NULL},
            {"Waiting", "Remote", "surface", "port", "5343", "abcdefghjk"},
        };
        fx_screen_no_network(&c[0], "192.168.4.1", "CompanionBox-8D1C", "abcdefghjk");
        fx_screen_looking(&c[1], "192.168.8.225", NULL, "CompanionBox-8D1C", NULL);
        fx_screen_looking(&c[2], "192.168.8.225", "companion.local", NULL, NULL);
        fx_screen_choose(&c[3], "192.168.8.225", NULL, NULL);
        fx_screen_too_old(&c[4], "4.2.0", NULL, NULL);
        fx_screen_dock(&c[5], "192.168.8.225", 5343, "S", "abcdefghjk");
        static sui_layout_t L;
        for (int s = 0; s < 6; s++) {
            sui_layout(D[d].cols, D[d].rows, D[d].px, &c[s], &L);
            /* every page's text, pieces of a word joined back */
            char all[1024] = "";
            for (int k = 0, end = -1, line = -1; k < L.n; k++) {
                const sui_cell_t *e = &L.cell[k];
                if (e->line != line || e->start > end) strcat(all, " ");
                strncat(all, c[s].lines[e->line] + e->start, e->len);
                end = e->start + e->len;
                line = e->line;
            }
            strcat(all, " ");
            for (int w = 0; w < 6 && want[s][w]; w++) {
                char tok[48];
                snprintf(tok, sizeof tok, " %s ", want[s][w]);
                TEST_ASSERT_NOT_NULL_MESSAGE(strstr(all, tok), want[s][w]);
            }
        }
    }
}

/* an IPv4 address and a "port <number>" pair stay together: on one row when it has the room, else
 * from a new row in balanced rows (never a lone octet), on one page when the deck has the rows; a label
 * line stays with the value line it introduces. */
static void test_value_groups_together(void)
{
    sui_content_t c;
    static sui_layout_t L;
    /* Mini: the hotspot's address 2 + 2 over two pages, never "1" alone */
    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", "k7m2xq9fhd");
    sui_layout(3, 2, 80, &c, &L);
    TEST_ASSERT_EQUAL_INT(5, L.pages);
    TEST_ASSERT_EQUAL_STRING("No|network\nSetup|page", page_text(&L, &c, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("No|network\n192|168", page_text(&L, &c, 3, 2, 1));
    TEST_ASSERT_EQUAL_STRING("No|network\n4|1", page_text(&L, &c, 3, 2, 2));
    /* MK.2 dock: the address on one row, "port" with its number on the next, on one page */
    fx_screen_dock(&c, "192.168.8.225", 5343, "CompanionBox-8D1C", "k7m2xq9fhd");
    sui_layout(5, 3, 72, &c, &L);
    TEST_ASSERT_EQUAL_INT(3, L.pages);
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nRemote|surface\n", page_text(&L, &c, 5, 3, 0));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\n192|168|8|225\nport|5343", page_text(&L, &c, 5, 3, 1));
    /* Mini dock: 2 + 2, then the pair */
    fx_screen_dock(&c, "192.168.8.225", 5343, NULL, NULL);
    sui_layout(3, 2, 80, &c, &L);
    TEST_ASSERT_EQUAL_INT(4, L.pages);
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\n192|168", page_text(&L, &c, 3, 2, 1));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\n8|225", page_text(&L, &c, 3, 2, 2));
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nport|5343", page_text(&L, &c, 3, 2, 3));
    /* a label line goes with its value line when they fit on a page together: "Box IP" and the IP */
    fx_screen_looking(&c, "192.168.8.225", "192.168.8.10", NULL, NULL);
    sui_layout(5, 3, 72, &c, &L);
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("Connecting\n192|168|8|10\n", page_text(&L, &c, 5, 3, 0));
    TEST_ASSERT_EQUAL_STRING("Connecting\nBox|IP\n192|168|8|225", page_text(&L, &c, 5, 3, 1));
    fx_screen_no_network(&c, "192.168.4.1", NULL, NULL);
    TEST_ASSERT_EQUAL_HEX8(0, c.keep_next);
    fx_screen_no_network(&c, "192.168.4.1", "S", NULL);
    TEST_ASSERT_EQUAL_HEX8((1u << 1) | (1u << 3), c.keep_next); /* Setup page + its address; WiFi + No password */
    fx_screen_looking(&c, "1.2.3.4", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_HEX8(1u << 1, c.keep_next);
    fx_screen_choose(&c, "1.2.3.4", NULL, NULL);
    TEST_ASSERT_EQUAL_HEX8(1u << 1, c.keep_next);
    fx_screen_dock(&c, "1.2.3.4", 5343, NULL, NULL);
    TEST_ASSERT_EQUAL_HEX8(1u << 1, c.keep_next);

    /* on every model: an address's octets on one page, no row with a lone octet of a 4-octet address
     * that had room for two, "port" and its number on one row and page */
    static const struct { int cols, rows, px; } D[] = {{3, 2, 80}, {5, 3, 72}, {8, 4, 96}, {4, 2, 96}, {4, 2, 120}, {9, 4, 112}};
    for (size_t d = 0; d < sizeof D / sizeof D[0]; d++) {
        sui_content_t s[4];
        fx_screen_dock(&s[0], "192.168.100.200", 5353, "CompanionBox-8D1C", "k7m2xq9fhd");
        fx_screen_no_network(&s[1], "192.168.4.1", "CompanionBox-8D1C", NULL);
        fx_screen_looking(&s[2], "10.0.0.7", "192.168.100.200", "S", NULL);
        fx_screen_choose(&s[3], "255.255.255.255", NULL, NULL);
        for (int i = 0; i < 4; i++) {
            sui_layout(D[d].cols, D[d].rows, D[d].px, &s[i], &L);
            for (int k = 0; k < L.n; k++) {
                const sui_cell_t *e = &L.cell[k];
                const char *w = s[i].lines[e->line] + e->start;
                if (e->len == 4 && !memcmp(w, "port", 4) && k + 1 < L.n) { /* the pair */
                    TEST_ASSERT_EQUAL_INT(e->page, L.cell[k + 1].page);
                    TEST_ASSERT_EQUAL_INT(e->row, L.cell[k + 1].row);
                }
                if (e->role == SUI_ROLE_ACCENT && e->piece == 0 && k + 3 < L.n && L.cell[k + 3].piece == 3) { /* an address */
                    int on_row[64] = {0};
                    for (int j = 0; j < 4; j++) on_row[L.cell[k + j].page * 8 + L.cell[k + j].row]++;
                    for (int j = 0; j < 64; j++) TEST_ASSERT_TRUE(on_row[j] == 0 || on_row[j] >= 2);
                    if (D[d].rows - 1 >= (D[d].cols >= 4 ? 1 : 2)) /* rows for it on a page */
                        for (int j = 1; j < 4; j++) TEST_ASSERT_EQUAL_INT(e->page, L.cell[k + j].page);
                }
            }
        }
    }
}

/* ---- paging ---- */

/* 3 s per page, 2 s on a screen of more than 3 pages. */
static void test_page_time(void)
{
    TEST_ASSERT_EQUAL_INT64(3000000, sui_page_us(2));
    TEST_ASSERT_EQUAL_INT64(3000000, sui_page_us(3));
    TEST_ASSERT_EQUAL_INT64(2000000, sui_page_us(4));
    TEST_ASSERT_EQUAL_INT64(2000000, sui_page_us(16));
    sui_pager_t p = {0};
    TEST_ASSERT_EQUAL_INT64(1000 + 2000000, sui_pager_shown(&p, 0, 5, 1000));
    TEST_ASSERT_EQUAL_INT(-1, sui_pager_next(&p, false, 1000 + 1999999, NULL));
    TEST_ASSERT_EQUAL_INT(1, sui_pager_next(&p, false, 1000 + 2000000, NULL));
    TEST_ASSERT_EQUAL_INT64(1000 + 3000000, sui_pager_shown(&p, 0, 3, 1000));
}

/* Content that fits on one page needs no timer; more pages turn every SUI_PAGE_US, in a loop. */
static void test_pager(void)
{
    sui_pager_t p = {0};
    int64_t due = -1;
    TEST_ASSERT_EQUAL_INT(0, sui_pager_next(&p, true, 1000, &due));
    TEST_ASSERT_EQUAL_INT64(0, due);

    TEST_ASSERT_EQUAL_INT64(0, sui_pager_shown(&p, 0, 1, 1000)); /* one page: no timer */
    TEST_ASSERT_EQUAL_INT(-1, sui_pager_next(&p, false, 1000 + 10 * SUI_PAGE_US, &due));
    TEST_ASSERT_EQUAL_INT64(0, due);

    TEST_ASSERT_EQUAL_INT64(1000 + SUI_PAGE_US, sui_pager_shown(&p, 0, 3, 1000));
    TEST_ASSERT_EQUAL_INT(-1, sui_pager_next(&p, false, 1000 + SUI_PAGE_US - 1, &due));
    TEST_ASSERT_EQUAL_INT64(1000 + SUI_PAGE_US, due);
    TEST_ASSERT_EQUAL_INT(1, sui_pager_next(&p, false, 1000 + SUI_PAGE_US, &due));
    const int64_t t1 = 1000 + SUI_PAGE_US + 500;
    TEST_ASSERT_EQUAL_INT64(t1 + SUI_PAGE_US, sui_pager_shown(&p, 1, 3, t1));
    TEST_ASSERT_EQUAL_INT(2, sui_pager_next(&p, false, t1 + SUI_PAGE_US, NULL));
    sui_pager_shown(&p, 2, 3, t1 + SUI_PAGE_US);
    TEST_ASSERT_EQUAL_INT(0, sui_pager_next(&p, false, t1 + 2 * SUI_PAGE_US, NULL)); /* after the last, the first */
    TEST_ASSERT_EQUAL_INT(0, sui_pager_next(&p, true, t1 + SUI_PAGE_US + 1, NULL));  /* new content: the first */
}

/* ---- rendering ---- */

/* Each word anti-aliased in its role's colour on the background, centred, inside the margin. */
static void test_render_word_per_key(void)
{
    sui_content_t c;
    fx_screen_dock(&c, "192.168.8.225", 5343, NULL, NULL);
    static uint8_t buf[96 * 96 * 3];
    const uint8_t *status = sui_role_rgb(&c, SUI_ROLE_STATUS), *accent = sui_role_rgb(&c, SUI_ROLE_ACCENT);
    const uint8_t *tint = sui_line_bg(&c, 0), *navy = sui_line_bg(&c, 1);
    sui_render_key(8, 4, 96, &c, 0, 0, 0, buf); /* "Waiting": the waiting state's colour on its tint */
    assert_blends(buf, 96, tint, status);
    TEST_ASSERT_TRUE(count_px(buf, 96 * 96, status) > 50); /* solid strokes */
    int partial = 0;                                       /* and smooth edges */
    for (int i = 0; i < 96 * 96; i++) partial += memcmp(buf + 3 * i, status, 3) && memcmp(buf + 3 * i, tint, 3);
    TEST_ASSERT_TRUE(partial > 20);
    sui_render_key(8, 4, 96, &c, 0, 5, 2, buf); /* "5343": an accent on navy (the main zone) */
    assert_blends(buf, 96, navy, accent);
    TEST_ASSERT_TRUE(count_px(buf, 96 * 96, accent) > 50);
    sui_render_key(8, 4, 96, &c, 0, 7, 3, buf); /* blank */
    TEST_ASSERT_EQUAL_INT(96 * 96, count_px(buf, 96 * 96, c.bg));

    /* centred: "192" (digits as tall as the capitals) in the middle of the key, at the cap size */
    sui_render_key(8, 4, 96, &c, 0, 0, 2, buf);
    int x0, y0, x1, y1;
    TEST_ASSERT_TRUE(ink_box(buf, 96, navy, &x0, &y0, &x1, &y1));
    TEST_ASSERT_INT_WITHIN(1, 95, x0 + x1);
    TEST_ASSERT_INT_WITHIN(2, 95, y0 + y1);
    TEST_ASSERT_INT_WITHIN(1, sui_text_width(40, "192", 3, NULL), x1 - x0 + 1);
    TEST_ASSERT_INT_WITHIN(1, font_px(40)->cap, y1 - y0 + 1);

    /* a custom message is drawn in the content's fg */
    sui_content_t m;
    sui_screen_message(&m, "Hello");
    m.fg[0] = 10;
    m.fg[1] = 200;
    m.fg[2] = 30;
    sui_render_key(1, 1, 96, &m, 0, 0, 0, buf);
    assert_blends(buf, 96, m.bg, m.fg);
    TEST_ASSERT_TRUE(count_px(buf, 96 * 96, m.fg) > 50);
}

/* Two lines of one word: one above the other, each centred, the pair centred in the key. */
static void test_render_two_lines(void)
{
    sui_content_t c;
    one_word(&c, "Reconnecting");
    const sui_fit_t f = fit_of("Reconnecting", 72);
    TEST_ASSERT_EQUAL_INT(2, f.nl);
    static uint8_t buf[72 * 72 * 3];
    sui_render_key(1, 1, 72, &c, 0, 0, 0, buf);
    int x0, y0, x1, y1;
    TEST_ASSERT_TRUE(ink_box(buf, 72, c.bg, &x0, &y0, &x1, &y1));
    const char *word = c.lines[0];
    const int w0 = sui_text_width(f.px, word, f.cut[0], NULL);
    const int w1 = sui_text_width(f.px, word + f.cut[0], f.cut[1], NULL);
    TEST_ASSERT_INT_WITHIN(1, w0 > w1 ? w0 : w1, x1 - x0 + 1);
    TEST_ASSERT_INT_WITHIN(1, 71, x0 + x1);
    const sui_font_size_t *fs = font_px(f.px);
    const int top = (72 - (fs->line + fs->cap)) / 2; /* the first line's cap top */
    TEST_ASSERT_INT_WITHIN(1, top, y0);
    assert_key_inside(1, 1, 72, &c, 0, 0, 0);
}

static void test_render_pages(void)
{
    sui_content_t c;
    fx_screen_dock(&c, "192.168.8.225", 5343, NULL, NULL);
    static uint8_t a[80 * 80 * 3], b[80 * 80 * 3];
    sui_render_key(3, 2, 80, &c, 1, 0, 1, a); /* "192" on the Mini's second page, under the status */
    TEST_ASSERT_TRUE(count_px(a, 80 * 80, sui_role_rgb(&c, SUI_ROLE_ACCENT)) > 0);
    sui_render_key(3, 2, 80, &c, 5, 0, 1, b); /* page 5 of 4 is page 1 */
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof a);
    sui_render_key(3, 2, 80, &c, 0, 0, 1, b); /* "Remote" on the first */
    TEST_ASSERT_EQUAL_INT(0, count_px(b, 80 * 80, sui_role_rgb(&c, SUI_ROLE_ACCENT)));
    TEST_ASSERT_TRUE(count_px(b, 80 * 80, sui_role_rgb(&c, SUI_ROLE_LABEL)) > 0);
    for (int p = 0; p < 4; p++) { /* "Waiting" on every page */
        sui_render_key(3, 2, 80, &c, p, 0, 0, b);
        TEST_ASSERT_TRUE(count_px(b, 80 * 80, sui_role_rgb(&c, SUI_ROLE_STATUS)) > 0);
    }
}

static void test_overlay_keeps_background(void)
{
    sui_content_t c;
    sui_screen_reconnecting(&c, NULL);
    c.lines[0] = "X";
    static uint8_t buf[96 * 96 * 3];
    const uint8_t keep[3] = {10, 20, 30};
    fill_key(buf, 96 * 96, keep[0], keep[1], keep[2]);
    sui_render_overlay_key(96, &c, buf);
    TEST_ASSERT_TRUE(count_px(buf, 96 * 96, sui_role_rgb(&c, SUI_ROLE_STATUS)) > 0); /* the glyph */
    TEST_ASSERT_TRUE(count_px(buf, 96 * 96, c.bg) > 0);                              /* its outline */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(keep, buf + 3 * (95 * 96 + 95), 3); // bottom-right corner
    TEST_ASSERT_EQUAL_UINT8_ARRAY(keep, buf, 3);                      // top-left corner
    TEST_ASSERT_EQUAL_UINT8_ARRAY(keep, buf + 3 * (20 * 96 + 20), 3); // well outside the outline

    // content with no word leaves the key alone entirely
    const sui_content_t blank = {.lines = {"  "}, .n = 1, .fg = {255, 255, 255}, .bg = {0, 0, 0}};
    fill_key(buf, 72 * 72, keep[0], keep[1], keep[2]);
    sui_render_overlay_key(72, &blank, buf);
    TEST_ASSERT_EQUAL_INT(72 * 72, count_px(buf, 72 * 72, keep));
}

/* "Reconnecting" on every key over its own dimmed image, each in the same per-key layout. */
static void test_overlay_on_every_key(void)
{
    sui_content_t c;
    sui_screen_reconnecting(&c, NULL);
    const uint8_t guard = 0xA5;
    static uint8_t buf[72 * 72 * 3 + 16];
    static uint8_t first[72 * 72];
    for (int key = 0; key < 5 * 3; key++) { /* every key of an MK.2, each with an image of its own */
        const uint8_t v = (uint8_t)(60 + 8 * key), grey[3] = {v, v, v};
        memset(buf + 72 * 72 * 3, guard, 16);
        fill_key(buf, 72 * 72, grey[0], grey[1], grey[2]);
        sui_render_overlay_key(72, &c, buf);
        TEST_ASSERT_TRUE(count_px(buf, 72 * 72, sui_role_rgb(&c, SUI_ROLE_STATUS)) > 0);
        TEST_ASSERT_TRUE(count_px(buf, 72 * 72, c.bg) > 0);
        TEST_ASSERT_TRUE(count_px(buf, 72 * 72, grey) > 72 * 72 / 4);
        for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_HEX8(guard, buf[72 * 72 * 3 + i]);
        for (int i = 0; i < 72 * 72; i++) { /* the same solid glyph pixels on every key */
            const uint8_t solid = (uint8_t)(memcmp(buf + 3 * i, sui_role_rgb(&c, SUI_ROLE_STATUS), 3) == 0);
            if (key == 0) first[i] = solid;
            else TEST_ASSERT_EQUAL_UINT8(first[i], solid);
        }
    }
}

/* The outline: the glyphs' alpha spread sui_halo_px(size) px (Chebyshev) in c->bg, under the glyphs. A
 * pixel next to a solid glyph pixel is solid outline; nothing changes farther from the glyphs. */
static void test_overlay_outline(void)
{
    sui_content_t c;
    sui_screen_reconnecting(&c, NULL);
    c.lines[0] = "XR";
    const uint8_t grey[3] = {128, 128, 128};
    static uint8_t ovl[120 * 120 * 3], ink[120 * 120 * 3];
    fill_key(ovl, 120 * 120, grey[0], grey[1], grey[2]);
    sui_render_overlay_key(120, &c, ovl);
    sui_render_key(1, 1, 120, &c, 0, 0, 0, ink); /* the glyphs alone, on the line's tint */
    const uint8_t *paper = sui_line_bg(&c, 0);
    const sui_fit_t f = fit_of("XR", 120);
    const int r = sui_halo_px(f.px);
    TEST_ASSERT_TRUE(r >= 1);
    const uint8_t *white = sui_role_rgb(&c, SUI_ROLE_STATUS);
    for (int y = 0; y < 120; y++)
        for (int x = 0; x < 120; x++) {
            const uint8_t *p = ovl + 3 * (y * 120 + x), *g = ink + 3 * (y * 120 + x);
            if (!memcmp(g, white, 3)) TEST_ASSERT_EQUAL_UINT8_ARRAY(white, p, 3); /* solid glyph */
            int near = 0, solid = 0;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= 120 || ny < 0 || ny >= 120) continue;
                    const uint8_t *q = ink + 3 * (ny * 120 + nx);
                    if (memcmp(q, paper, 3)) near = 1;
                    if (!memcmp(q, white, 3) && dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) solid = 1;
                }
            if (!near) TEST_ASSERT_EQUAL_UINT8_ARRAY(grey, p, 3);        /* far from the glyphs: untouched */
            if (solid && !memcmp(g, paper, 3)) TEST_ASSERT_EQUAL_UINT8_ARRAY(c.bg, p, 3); /* solid outline */
        }
}

/* ---- screen builders ---- */

static void test_screen_text_copy(void)
{
    sui_content_t c;
    fx_screen_too_old(&c, "4.2.0", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(3, c.n);
    TEST_ASSERT_EQUAL_STRING("Companion too old", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Needs 5.0+", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("Has 4.2.0", c.lines[2]);
    TEST_ASSERT_EQUAL_UINT8(SUI_ROLE_STATUS, c.role[0][0]); /* the colours come from the roles */
    TEST_ASSERT_EQUAL_UINT8(SUI_ROLE_LABEL, c.role[2][0]);
    TEST_ASSERT_EQUAL_UINT8(SUI_ROLE_VALUE, c.role[2][1]);
}

/* The web page's address as an IP on keys of its own ("http://" would make it one broken word). */
static void test_screen_choose_text(void)
{
    sui_content_t c;
    fx_screen_choose(&c, "192.168.8.225", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(3, c.n);
    TEST_ASSERT_EQUAL_STRING("Choose Companion", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Web page", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225", c.lines[2]);
    static sui_layout_t L;
    sui_layout(5, 3, 72, &c, &L);
    TEST_ASSERT_EQUAL_INT(1, L.pages);
    TEST_ASSERT_EQUAL_STRING("Choose|Companion\nWeb|page\n192|168|8|225", page_text(&L, &c, 5, 3, 0));
}

static void test_screen_reconnecting_text(void)
{
    sui_content_t c;
    sui_screen_reconnecting(&c, NULL);
    TEST_ASSERT_EQUAL_INT(1, c.n);
    TEST_ASSERT_EQUAL_STRING("Reconnecting", c.lines[0]);
    const uint8_t white[3] = {255, 255, 255}, black[3] = {0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(white, c.fg, 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(black, c.bg, 3);
    TEST_ASSERT_EQUAL_INT(1, sui_page_count(3, 2, 80, &c)); /* the overlay never pages */
}

static void test_screen_message_splits_lines(void)
{
    sui_content_t c;
    sui_screen_message(&c, "A\nB\nC\nD\nE");
    TEST_ASSERT_EQUAL_INT(4, c.n);
    TEST_ASSERT_EQUAL_STRING("A", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("B", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("C", c.lines[2]);
    TEST_ASSERT_EQUAL_STRING("D", c.lines[3]);
    const uint8_t white[3] = {255, 255, 255}, black[3] = {0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(white, c.fg, 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(black, c.bg, 3);

    sui_screen_message(&c, "Hello\n\nWorld");
    TEST_ASSERT_EQUAL_INT(3, c.n);
    TEST_ASSERT_EQUAL_STRING("Hello", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("World", c.lines[2]);
}

static void test_screen_message_truncates(void)
{
    char line[51];
    memset(line, 'a', 50);
    line[50] = '\0';
    sui_content_t c;
    sui_screen_message(&c, line);
    TEST_ASSERT_EQUAL_INT(1, c.n);
    TEST_ASSERT_EQUAL_size_t(40, strlen(c.lines[0]));
    TEST_ASSERT_EQUAL_STRING_LEN(line, c.lines[0], 40);

    // the longest message the box accepts (160 bytes: 40+40+40+37 chars and 3 newlines,
    // 161 bytes of line storage) is kept whole
    static const int want[4] = {40, 40, 40, 37};
    char big[161];
    size_t at = 0;
    for (int i = 0; i < 4; i++) {
        memset(big + at, (char)('a' + i), (size_t)want[i]);
        at += (size_t)want[i];
        if (i < 3) big[at++] = '\n';
    }
    TEST_ASSERT_EQUAL_size_t(160, at);
    big[at] = '\0';
    sui_screen_message(&c, big);
    TEST_ASSERT_EQUAL_INT(4, c.n);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_size_t((size_t)want[i], strlen(c.lines[i]));
        TEST_ASSERT_EQUAL_CHAR('a' + i, c.lines[i][want[i] - 1]);
    }
}

/* The most words a message can have (4 lines of 20 one-char words) are all laid out, over pages. */
static void test_message_most_words(void)
{
    char text[161];
    size_t at = 0;
    for (int l = 0; l < 4; l++) {
        for (int w = 0; w < 20; w++) {
            text[at++] = (char)('a' + w % 26);
            if (w < 19) text[at++] = ' ';
        }
        if (l < 3) text[at++] = '\n';
    }
    text[at] = '\0';
    TEST_ASSERT_TRUE(at <= 160);
    sui_content_t c;
    sui_screen_message(&c, text);
    static sui_layout_t L;
    sui_layout(3, 2, 80, &c, &L);
    TEST_ASSERT_EQUAL_INT(SUI_CELLS_MAX, L.n);
    TEST_ASSERT_EQUAL_INT(16, L.pages); /* each line: 7 rows = 4 pages of the Mini's 2 */
    TEST_ASSERT_EQUAL_STRING("a|b|c\nd|e|f", page_text(&L, &c, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("s|t\n", page_text(&L, &c, 3, 2, 3));
    TEST_ASSERT_EQUAL_STRING("a|b|c\nd|e|f", page_text(&L, &c, 3, 2, 4));
}

static void test_screen_dock_text(void)
{
    sui_content_t c;
    fx_screen_dock(&c, "192.168.8.225", 5343, NULL, NULL); /* hotspot off */
    TEST_ASSERT_EQUAL_INT(3, c.n);
    TEST_ASSERT_EQUAL_STRING("Waiting for Companion", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Remote surface", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225 port 5343", c.lines[2]);
    const uint8_t white[3] = {255, 255, 255}, black[3] = {0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(white, c.fg, 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(black, c.bg, 3);

    /* hotspot on: SSID and password one line each, in the service zone */
    fx_screen_dock(&c, "10.0.0.7", 5345, "CompanionBox-ABCD", "abcdefghjk");
    TEST_ASSERT_EQUAL_INT(5, c.n);
    TEST_ASSERT_EQUAL_STRING("10.0.0.7 port 5345", c.lines[2]);
    TEST_ASSERT_EQUAL_STRING("WiFi CompanionBox-ABCD", c.lines[3]);
    TEST_ASSERT_EQUAL_STRING("Pass abcdefghjk", c.lines[4]);
    /* an 8-column deck shows it all at once, the hotspot's lines together in the bottom row */
    static sui_layout_t L;
    sui_layout(8, 4, 96, &c, &L);
    TEST_ASSERT_EQUAL_INT(1, L.pages);
    TEST_ASSERT_EQUAL_STRING("Waiting|for|Companion\nRemote|surface\n10|0|0|7|port|5345\nWiFi|Companion|Box-ABCD|Pass|abcde|fghjk",
                             page_text(&L, &c, 8, 4, 0));
}

/* The hotspot name and password are kept whole (a deck never shows a wrong password). */
static void test_screen_credentials_whole(void)
{
    char ssid[33], pass[65], host[64], want[80];
    memset(ssid, 's', 32);
    ssid[32] = '\0';
    memset(pass, 'p', 64);
    pass[64] = '\0';
    memset(host, 'h', 63);
    host[63] = '\0';
    sui_content_t c;
    fx_screen_dock(&c, NULL, 5353, ssid, pass);
    TEST_ASSERT_EQUAL_INT(5, c.n);
    TEST_ASSERT_EQUAL_STRING("Remote surface", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING(" port 5353", c.lines[2]);
    snprintf(want, sizeof want, "WiFi %s", ssid);
    TEST_ASSERT_EQUAL_STRING(want, c.lines[3]);
    snprintf(want, sizeof want, "Pass %s", pass);
    TEST_ASSERT_EQUAL_STRING(want, c.lines[4]);

    /* the longest screen: connecting, with the longest host, IP, SSID and password: all 6 lines whole */
    fx_screen_looking(&c, "255.255.255.255", host, ssid, pass);
    TEST_ASSERT_EQUAL_INT(6, c.n);
    TEST_ASSERT_EQUAL_STRING(host, c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", c.lines[3]);
    TEST_ASSERT_EQUAL_STRING(want, c.lines[5]);
    fx_screen_no_network(&c, "192.168.4.1", ssid, pass);
    TEST_ASSERT_EQUAL_STRING(want, c.lines[4]);
}

/* an open hotspot shows its name and "No password" instead of a password, kept on one page. */
static void test_screen_hotspot_open(void)
{
    sui_content_t c;
    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-ABCD", NULL);
    TEST_ASSERT_EQUAL_INT(5, c.n);
    TEST_ASSERT_EQUAL_STRING("No network", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("WiFi CompanionBox-ABCD", c.lines[3]);
    TEST_ASSERT_EQUAL_STRING("No password", c.lines[4]);
    TEST_ASSERT_EQUAL_HEX8((1u << 1) | (1u << 3), c.keep_next);

    fx_screen_looking(&c, "1.2.3.4", NULL, "CompanionBox-ABCD", NULL);
    TEST_ASSERT_EQUAL_INT(5, c.n);
    TEST_ASSERT_EQUAL_STRING("WiFi CompanionBox-ABCD", c.lines[3]);
    TEST_ASSERT_EQUAL_STRING("No password", c.lines[4]);
    TEST_ASSERT_EQUAL_HEX8((1u << 1) | (1u << 3), c.keep_next);

    fx_screen_dock(&c, "10.0.0.7", 5345, "CompanionBox-ABCD", NULL);
    TEST_ASSERT_EQUAL_INT(5, c.n);
    TEST_ASSERT_EQUAL_STRING("WiFi CompanionBox-ABCD", c.lines[3]);
    TEST_ASSERT_EQUAL_STRING("No password", c.lines[4]);
    TEST_ASSERT_EQUAL_HEX8((1u << 1) | (1u << 3), c.keep_next);

    /* the same as a password: "No password" stays with the name, at the bottom */
    static sui_layout_t L;
    fx_screen_looking(&c, "192.168.8.225", NULL, "CompanionBox-ABCD", NULL);
    sui_layout(5, 3, 72, &c, &L); /* MK.2 */
    TEST_ASSERT_EQUAL_INT(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\nBox|IP\n192|168|8|225", page_text(&L, &c, 5, 3, 0));
    TEST_ASSERT_EQUAL_STRING("Finding|Companion\n\nWiFi|Companion|Box-ABCD|No|password", page_text(&L, &c, 5, 3, 1));
    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-ABCD", NULL);
    sui_layout(3, 2, 80, &c, &L); /* Mini: one row under the status */
    TEST_ASSERT_EQUAL_INT(5, L.pages);
    TEST_ASSERT_EQUAL_STRING("No|network\nSetup|page", page_text(&L, &c, 3, 2, 0));
    TEST_ASSERT_EQUAL_STRING("No|network\nWiFi|Companion|Box-ABCD", page_text(&L, &c, 3, 2, 3));
    TEST_ASSERT_EQUAL_STRING("No|network\nNo|password", page_text(&L, &c, 3, 2, 4));

    /* a password is shown as before; an empty one counts as none */
    fx_screen_no_network(&c, "192.168.4.1", "S", "abcdefghjk");
    TEST_ASSERT_EQUAL_STRING("Pass abcdefghjk", c.lines[4]);
    fx_screen_no_network(&c, "192.168.4.1", "S", "");
    TEST_ASSERT_EQUAL_STRING("No password", c.lines[4]);

    /* no SSID: the hotspot is off and nothing of it shows */
    fx_screen_no_network(&c, "192.168.4.1", NULL, "abcdefghjk");
    TEST_ASSERT_EQUAL_INT(2, c.n);
    TEST_ASSERT_EQUAL_HEX8(0, c.keep_next);
    fx_screen_looking(&c, "1.2.3.4", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(3, c.n);
    fx_screen_dock(&c, "1.2.3.4", 5343, NULL, "abcdefghjk");
    TEST_ASSERT_EQUAL_INT(3, c.n);
    TEST_ASSERT_EQUAL_STRING("Waiting for Companion", c.lines[0]);
}

/* ---- the divider between lines sharing a row ---- */

static void rgb3(uint32_t hex, uint8_t *o) { o[0] = (uint8_t)(hex >> 16); o[1] = (uint8_t)(hex >> 8); o[2] = (uint8_t)hex; }

/* The cell whose line is `line` and which starts it (piece 0, the line's first cell). */
static const sui_cell_t *line_start(const sui_layout_t *L, int line)
{
    for (int i = 0; i < L->n; i++)
        if (L->cell[i].line == line) return &L->cell[i];
    return NULL;
}

static void test_divider_geometry(void)
{
    int x, y, w, h;
    sui_divider_box(96, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_INT(4, w);
    TEST_ASSERT_EQUAL_INT(3, x);
    TEST_ASSERT_EQUAL_INT(58, h); /* 60 % of 96 */
    TEST_ASSERT_EQUAL_INT((96 - 58) / 2, y);
    sui_divider_box(72, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_INT(3, w);
    TEST_ASSERT_EQUAL_INT(43, h);
    sui_divider_box(32, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_INT(2, w); /* never thinner than 2 px */
    for (size_t i = 0; i < sizeof KEY_PX / sizeof KEY_PX[0]; i++) {
        sui_divider_box(KEY_PX[i], &x, &y, &w, &h);
        TEST_ASSERT_TRUE(w >= 2 && x >= 2 && x + w <= sui_divider_inset(KEY_PX[i]));
        TEST_ASSERT_TRUE(y >= 0 && y + h <= KEY_PX[i]);
    }
}

/* The second line's first cell (and only it) is marked divider-left, where two lines share a row. */
static void test_divider_marks_second_line(void)
{
    sui_content_t c;
    static sui_layout_t L;
    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", "abcdefghjk");
    sui_layout(8, 4, 96, &c, &L); /* Module 32: WiFi and Pass share the bottom row */
    const sui_cell_t *pass = line_start(&L, 4);
    TEST_ASSERT_NOT_NULL(pass);
    TEST_ASSERT_EQUAL_STRING_LEN("Pass", c.lines[4] + pass->start, 4);
    TEST_ASSERT_TRUE(pass->col > 0);
    TEST_ASSERT_EQUAL_INT(line_start(&L, 3)->row, pass->row);
    int n = 0;
    for (int i = 0; i < L.n; i++) n += L.cell[i].divider;
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT8(1, pass->divider);

    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", NULL); /* "No password" */
    sui_layout(8, 4, 96, &c, &L);
    const sui_cell_t *no = line_start(&L, 4);
    TEST_ASSERT_EQUAL_STRING_LEN("No", c.lines[4] + no->start, 2);
    TEST_ASSERT_EQUAL_UINT8(1, no->divider);
    TEST_ASSERT_EQUAL_UINT8(0, line_start(&L, 3)->divider);
}

/* No divider where a line starts a row, and none outside the service zone. */
static void test_divider_not_at_row_start(void)
{
    static const struct { int cols, rows, px; } D[] = {{3, 2, 80}, {5, 3, 72}, {8, 4, 96}, {4, 2, 96}, {4, 2, 120}, {9, 4, 112}};
    static sui_layout_t L;
    for (size_t d = 0; d < sizeof D / sizeof D[0]; d++)
        for (int variant = 0; variant < 3; variant++) {
            sui_content_t c;
            if (variant == 0) fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", "abcdefghjk");
            else if (variant == 1) fx_screen_dock(&c, "192.168.8.225", 5343, "CompanionBox-8D1C", NULL);
            else fx_screen_looking(&c, "192.168.8.225", NULL, "S", "p");
            sui_layout(D[d].cols, D[d].rows, D[d].px, &c, &L);
            for (int i = 0; i < L.n; i++) {
                const sui_cell_t *e = &L.cell[i];
                if (e->divider) {
                    TEST_ASSERT_EQUAL_UINT8(SUI_GROUP_SERVICE, c.group[e->line]);
                    TEST_ASSERT_TRUE(e->col > 0);
                    TEST_ASSERT_EQUAL_UINT8(0, e->piece);
                    TEST_ASSERT_EQUAL_PTR(line_start(&L, e->line), e);
                    const sui_cell_t *prev = sui_layout_cell(&L, e->page, e->col - 1, e->row);
                    TEST_ASSERT_NOT_NULL(prev); /* a different line sits just before it on the row */
                    TEST_ASSERT_TRUE(prev->line != e->line);
                } else if (c.group[e->line] == SUI_GROUP_SERVICE && line_start(&L, e->line) == e && e->col > 0) {
                    TEST_FAIL_MESSAGE("a line starting mid-row has no divider");
                }
            }
        }
    /* MK.2, service page: "WiFi ..." alone on a row of its own when Pass can't join it */
    sui_content_t c;
    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", "abcdefghjkmnpqrstuvwxyz23456789");
    sui_layout(5, 3, 72, &c, &L);
    for (int i = 0; i < L.n; i++)
        if (L.cell[i].col == 0) TEST_ASSERT_EQUAL_UINT8(0, L.cell[i].divider);
}

/* The render: the bar's pixels in its column band; the text clear of it. */
static void test_divider_render(void)
{
    sui_content_t c;
    static sui_layout_t L;
    static uint8_t buf[96 * 96 * 3];
    fx_screen_no_network(&c, "192.168.4.1", "CompanionBox-8D1C", "abcdefghjk");
    sui_layout(8, 4, 96, &c, &L);
    const sui_cell_t *pass = line_start(&L, 4);
    sui_render_key(8, 4, 96, &c, 0, pass->col, pass->row, buf);
    int bx, by, bw, bh;
    sui_divider_box(96, &bx, &by, &bw, &bh);
    uint8_t bar[3], bg[3];
    rgb3(SUI_RGB_DIVIDER, bar);
    memcpy(bg, sui_line_bg(&c, 4), 3);
    TEST_ASSERT_TRUE(memcmp(bar, bg, 3));
    TEST_ASSERT_TRUE(contrast(bar, bg) >= 3.0); /* clearly visible on the purple */
    TEST_ASSERT_TRUE(luminance(bar) > luminance(bg));
    for (int y = 0; y < 96; y++)
        for (int x = 0; x < 96; x++) {
            const uint8_t *p = buf + 3 * (y * 96 + x);
            const bool in = x >= bx && x < bx + bw && y >= by && y < by + bh;
            if (in) TEST_ASSERT_EQUAL_UINT8_ARRAY(bar, p, 3);
            else if (x < bx + bw + SUI_MARGIN) TEST_ASSERT_EQUAL_UINT8_ARRAY(bg, p, 3); /* text stays clear of the bar */
        }
    int x0, y0, x1, y1; /* and the text is still there, inside the room left of the bar */
    uint8_t copy[96 * 96 * 3];
    memcpy(copy, buf, sizeof copy);
    for (int y = by; y < by + bh; y++) for (int x = bx; x < bx + bw; x++) memcpy(copy + 3 * (y * 96 + x), bg, 3);
    TEST_ASSERT_TRUE(ink_box(copy, 96, bg, &x0, &y0, &x1, &y1));
    TEST_ASSERT_TRUE(x0 >= bx + bw + SUI_MARGIN && x1 < 96 - SUI_MARGIN);

    /* a key that is not divider-left has no bar */
    const sui_cell_t *wifi = line_start(&L, 3);
    sui_render_key(8, 4, 96, &c, 0, wifi->col, wifi->row, buf);
    TEST_ASSERT_EQUAL_INT(0, count_px(buf, 96 * 96, bar));
}

/* The reduced width is rechecked: the text still fits (size from the narrower key). */
static void test_divider_text_fits(void)
{
    static const struct { int cols, rows, px; } D[] = {{3, 2, 80}, {5, 3, 72}, {8, 4, 96}, {4, 2, 96}, {4, 2, 120}, {9, 4, 112}};
    static sui_layout_t L;
    static uint8_t buf[120 * 120 * 3];
    for (size_t d = 0; d < sizeof D / sizeof D[0]; d++) {
        sui_content_t c;
        fx_screen_dock(&c, "192.168.8.225", 5343, "CompanionBox-8D1C", "abcdefghjk");
        sui_layout(D[d].cols, D[d].rows, D[d].px, &c, &L);
        for (int i = 0; i < L.n; i++) {
            const sui_cell_t *e = &L.cell[i];
            if (!e->divider) continue;
            TEST_ASSERT_TRUE(e->fit.px >= SUI_MIN_PX);
            const int pg = e->page == SUI_PAGE_ALL ? 0 : e->page, inset = sui_divider_inset(D[d].px);
            sui_render_key(D[d].cols, D[d].rows, D[d].px, &c, pg, e->col, e->row, buf);
            uint8_t bg[3];
            memcpy(bg, sui_line_bg(&c, e->line), 3);
            int bx, by, bw, bh;
            sui_divider_box(D[d].px, &bx, &by, &bw, &bh);
            for (int y = by; y < by + bh; y++) for (int x = bx; x < bx + bw; x++) memcpy(buf + 3 * (y * D[d].px + x), bg, 3);
            int x0, y0, x1, y1;
            if (ink_box(buf, D[d].px, bg, &x0, &y0, &x1, &y1))
                TEST_ASSERT_TRUE(x0 >= inset + SUI_MARGIN && x1 < D[d].px - SUI_MARGIN);
        }
    }
}

/* sui_font_find (a binary search) needs the sizes ascending, and finds what the linear search (font_px)
 * finds for every px. */
static void test_font_find_matches_linear(void)
{
    for (int i = 1; i < sui_font_sizes; i++) TEST_ASSERT_TRUE(sui_font[i - 1].px < sui_font[i].px);
    for (int px = -5; px <= 200; px++) TEST_ASSERT_TRUE(sui_font_find(px) == font_px(px));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_font_find_matches_linear);
    RUN_TEST(test_layout_dock_8x4);
    RUN_TEST(test_layout_dock_mini_pages);
    RUN_TEST(test_layout_ipv4_octets);
    RUN_TEST(test_layout_line_wraps_rows);
    RUN_TEST(test_layout_page_break_at_line_end);
    RUN_TEST(test_layout_long_line_spans_pages);
    RUN_TEST(test_layout_empty_lines);
    RUN_TEST(test_fit_cap);
    RUN_TEST(test_fit_sizes_and_min_split);
    RUN_TEST(test_fit_pinned);
    RUN_TEST(test_fit_breaks);
    RUN_TEST(test_fit_hyphenation);
    RUN_TEST(test_layout_one_size_per_line);
    RUN_TEST(test_comfort_px);
    RUN_TEST(test_value_across_keys);
    RUN_TEST(test_value_pieces_wrap);
    RUN_TEST(test_word_cells);
    RUN_TEST(test_fit_four_digits_whole);
    RUN_TEST(test_fit_nothing);
    RUN_TEST(test_layout_long_word_continues);
    RUN_TEST(test_layout_keep_with_next);
    RUN_TEST(test_layout_real_words_fit_every_model);
    RUN_TEST(test_roles_per_screen);
    RUN_TEST(test_role_colours);
    RUN_TEST(test_screen_states_and_groups);
    RUN_TEST(test_palette_contrast);
    RUN_TEST(test_screen_word_counts);
    RUN_TEST(test_zones_per_screen);
    RUN_TEST(test_zones_layout);
    RUN_TEST(test_zones_small_decks);
    RUN_TEST(test_zones_keep_information);
    RUN_TEST(test_value_groups_together);
    RUN_TEST(test_page_time);
    RUN_TEST(test_pager);
    RUN_TEST(test_render_word_per_key);
    RUN_TEST(test_render_two_lines);
    RUN_TEST(test_render_pages);
    RUN_TEST(test_divider_geometry);
    RUN_TEST(test_divider_marks_second_line);
    RUN_TEST(test_divider_not_at_row_start);
    RUN_TEST(test_divider_render);
    RUN_TEST(test_divider_text_fits);
    RUN_TEST(test_overlay_keeps_background);
    RUN_TEST(test_overlay_on_every_key);
    RUN_TEST(test_overlay_outline);
    RUN_TEST(test_screen_text_copy);
    RUN_TEST(test_screen_choose_text);
    RUN_TEST(test_screen_reconnecting_text);
    RUN_TEST(test_screen_message_splits_lines);
    RUN_TEST(test_screen_message_truncates);
    RUN_TEST(test_message_most_words);
    RUN_TEST(test_screen_dock_text);
    RUN_TEST(test_screen_credentials_whole);
    RUN_TEST(test_screen_hotspot_open);
    return UNITY_END();
}
