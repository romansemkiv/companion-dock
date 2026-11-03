/* Pure: drawing a key's tile and the status bar into RGB888, in design units
 * (a 96 px key) scaled to the key. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dui_int.h"
#include "raster.h"

#define PI_F 3.14159265f
#define U(v) ((float)(v) * (float)key_px / 96.0f)

static int tlen(const dui_tile_t *t) { return t->text ? (t->len ? t->len : (int)strlen(t->text)) : 0; }

/* Canvas textBaseline "middle" at y for size px: the baseline 0.4 em under it (the font's ascent 1.0
 * and descent 0.2 em). */
static float mid(float y, int px) { return y + 0.4f * (float)px; }

/* The font size nearest px, not below the floor min (the minimums). */
static int near_min(float px, int min)
{
    const int n = dui__font_near(px);
    return n > min ? n : min;
}

/* ---- text ---- */

/* An aligned text (t->fit): one line on the row's baseline; two lines centred on cy. */
static void fitted_text(const dui_canvas_t *c, const dui_tile_t *t, int key_px, float cy, uint32_t rgb)
{
    const int len = tlen(t), px = t->fit.px;
    if (!len || !px) return;
    if (t->fit.nl != 2) {
        dui__text(c, px, U(48), t->fit.base, t->text, len, rgb, 0, DUI__CENTRE);
        return;
    }
    char first[SUI_FIT_MAX + 2];
    int n1 = t->fit.cut < SUI_FIT_MAX ? t->fit.cut : SUI_FIT_MAX, b = t->fit.cut;
    memcpy(first, t->text, (size_t)n1);
    if (t->fit.hyph) first[n1++] = '-';
    while (b < len && t->text[b] == ' ') b++;
    dui__text(c, px, U(48), cy - 0.1f * px, first, n1, rgb, 0, DUI__CENTRE);
    dui__text(c, px, U(48), cy + 0.92f * px, t->text + b, len - b, rgb, 0, DUI__CENTRE);
}

/* A label (button, toast): one line at its largest size up to max_px that fits max_w, on baseline base;
 * else two lines centred on cy, no larger than max_px. never below DUI_LABEL_MIN_PX (max_px is raised
 * to it); two lines at a natural break (dui__two_lines). */
static int label_size(const char *s, int len, int mw, int mx, int *nl, int *a, int *b, bool *h)
{
    if (mx < DUI_LABEL_MIN_PX) mx = DUI_LABEL_MIN_PX;
    const int px = dui__font_fit(s, len, mw, mx, DUI_LABEL_MIN_PX, 0);
    *nl = 1;
    *h = false;
    if (px) return px;
    *nl = 2;
    int p = dui__two_lines(s, len, mw, mx, DUI_LABEL_MIN_PX, true, a, b, h);
    if (!p) { /* too long even so: the best two lines below the floor, never cut */
        p = dui__two_lines(s, len, mw, mx, 10, true, a, b, h);
        if (!p) dui__split2(s, len, a, b), p = 10, *h = false;
    }
    return p;
}

/* A button's label box on a key of key_px: its width, U(74), and its largest size, U(18) (DUI_LABEL_MIN_PX or more). */
static void label_box(int key_px, int *mw, int *mx)
{
    *mw = (int)U(74);
    *mx = (int)(U(18) + 0.001f) > DUI_LABEL_MIN_PX ? (int)(U(18) + 0.001f) : DUI_LABEL_MIN_PX;
}

/* A hold's hint as drawn: the hold and what it does, on two lines split at its " · " ("Hold 2 s" /
 * "restarts"), at the largest size up to the label box's that both lines fit (DUI_LABEL_MIN_PX or more; below only
 * when nothing else does, never cut); no consequence: one line. *cut: chars on the first line (0: one line), *next:
 * where the second starts. */
static int hint_size(const char *s, int len, int key_px, int *cut, int *next)
{
    int mw, mx;
    label_box(key_px, &mw, &mx);
    *cut = *next = 0;
    for (int i = 1; i + 4 <= len; i++)
        if (!memcmp(s + i, " \xC2\xB7 ", 4)) {
            *cut = i;
            *next = i + 4;
            break;
        }
    for (int min = DUI_LABEL_MIN_PX; min >= 10; min -= DUI_LABEL_MIN_PX - 10) {
        if (!*cut) {
            const int p = dui__font_fit(s, len, mw, mx, min, 0);
            if (p) return p;
            continue;
        }
        const int p1 = dui__font_fit(s, *cut, mw, mx, min, 0), p2 = dui__font_fit(s + *next, len - *next, mw, mx, min, 0);
        if (p1 && p2) return p1 < p2 ? p1 : p2;
    }
    return 10;
}

/* A two-line label's centre (label()): cy, moved up so the second line's own descenders stay inside the
 * button's outline (U(91) less its stroke). */
static float label_cy(const char *s, int len, int b, int p, int key_px, float cy)
{
    const float bottom = U(89) - (float)dui__text_descent(p, s + b, len - b), over = cy + 0.92f * p - bottom;
    return over > 0 ? cy - over : cy;
}

/* A label sized already (label_size's p, nl, a, b, h), drawn as label() draws it. */
static void label_draw(const dui_canvas_t *c, const char *s, int len, int key_px, float cy, float base, int p, int nl,
                       int a, int b, bool h, uint32_t rgb)
{
    if (nl == 1) {
        dui__text(c, p, U(48), base, s, len, rgb, 0, DUI__CENTRE);
        return;
    }
    cy = label_cy(s, len, b, p, key_px, cy);
    char first[SUI_FIT_MAX + 2];
    const int n1 = a < SUI_FIT_MAX ? a : SUI_FIT_MAX;
    memcpy(first, s, (size_t)n1);
    if (h) first[n1] = '-'; /* a word broken at a syllable-like point */
    dui__text(c, p, U(48), cy - 0.1f * p, first, n1 + h, rgb, 0, DUI__CENTRE);
    dui__text(c, p, U(48), cy + 0.92f * p, s + b, len - b, rgb, 0, DUI__CENTRE);
}

static void label(const dui_canvas_t *c, const char *s, int len, int key_px, float cy, float base, float max_w,
                  float max_px, uint32_t rgb)
{
    if (!s || len <= 0) return;
    int nl, a = 0, b = 0;
    bool h;
    const int p = label_size(s, len, (int)max_w, (int)(max_px + 0.001f), &nl, &a, &b, &h);
    label_draw(c, s, len, key_px, cy, base, p, nl, a, b, h, rgb);
}

/* a sub-screen's title on its Back key (the decks with no status row and no bar): one size per model,
 * the largest up to U(18) that holds DUI_STATUS_WHOLE in U(74) (DUI_LABEL_MIN_PX or more); a longer title on two
 * lines at that size ("Scan to" / "join"), smaller only when even two lines can't hold it. */
static void nav_title(const dui_canvas_t *c, const char *s, int len, int key_px)
{
    int mw, mx;
    label_box(key_px, &mw, &mx);
    int px = dui__font_fit(DUI_STATUS_WHOLE, (int)strlen(DUI_STATUS_WHOLE), mw, mx, DUI_LABEL_MIN_PX, 0);
    if (!px) px = DUI_LABEL_MIN_PX;
    int a, b;
    if (dui__text_w(px, s, len, 0, NULL) <= mw || !dui__two_lines(s, len, mw, px, px, false, &a, &b, NULL)) {
        label(c, s, len, key_px, U(72), U(78), U(74), (float)px, SUI_RGB_LABEL); /* one line (or as a label fits) */
        return;
    }
    const float cy = label_cy(s, len, b, px, key_px, U(72));
    dui__text(c, px, U(48), cy - 0.1f * px, s, a, SUI_RGB_LABEL, 0, DUI__CENTRE);
    dui__text(c, px, U(48), cy + 0.92f * px, s + b, len - b, SUI_RGB_LABEL, 0, DUI__CENTRE);
}

/* ---- pieces ---- */

/* The state glyph: a filled circle in the state's colour, the mark in the background's (bg). */
static void glyph(const dui_canvas_t *c, int state, float cx, float cy, float r, uint32_t bg, uint8_t phase)
{
    rs_draw_disc(c, cx, cy, r, dui__state_rgb(state), 1);
    const float d = r * 0.38f;
    if (state == DUI_STATE_BAD) {
        const float x[8] = {cx - d, cy - d, cx + d, cy + d, cx + d, cy - d, cx - d, cy + d};
        rs_draw_segs(c, x, 2, r / 4, bg);
    } else if (state == DUI_STATE_OK) {
        const float v[8] = {cx - d, cy, cx - d * 0.3f, cy + d * 0.75f, cx - d * 0.3f, cy + d * 0.75f, cx + d * 1.1f, cy - d * 0.8f};
        rs_draw_segs(c, v, 2, r / 4, bg);
    } else { /* three dots, pulsing in turn */
        const float a = 2 * PI_F * phase / 256.0f;
        for (int i = 0; i < 3; i++)
            rs_draw_disc(c, cx + (float)(i - 1) * d, cy, r / 6, bg, 0.35f + 0.65f * (sinf(a - (float)i) * 0.5f + 0.5f));
    }
}

static void spinner(const dui_canvas_t *c, int key_px, uint8_t phase)
{
    const int step = phase * 10 / 256;
    for (int i = 0; i < 10; i++) {
        const float g = (float)i * PI_F / 5;
        rs_draw_disc(c, U(48 + cosf(g) * 18), U(48 + sinf(g) * 18), U(4), SUI_RGB_WAIT, (float)((i + step) % 10) / 10.0f);
    }
}

/* The button's body: its rounded inset rect, filled, the hold fill from the bottom, the outline. */
static void body(const dui_canvas_t *c, int key_px, uint32_t fill, float hold, uint32_t hold_rgb, uint32_t edge)
{
    rs_draw_rrect(c, U(5), U(5), U(86), U(86), U(14), 0, fill);
    if (hold > 0) {
        const float h = U(86) * (hold > 1 ? 1 : hold);
        rs_draw_rect_clipped(c, U(5), U(91) - h, U(86), h, U(5), U(5), U(86), U(86), U(14), hold_rgb);
    }
    rs_draw_rrect(c, U(5), U(5), U(86), U(86), U(14), U(2), edge);
}

static void button(const dui_canvas_t *c, const dui_tile_t *t, int key_px)
{
    const bool sel = t->flags & DUI_F_SELECTED, danger = t->flags & DUI_F_DANGER;
    /* dim: does nothing now. A locked key is drawn disabled too, with its padlock: a press opens the PIN pad */
    const bool locked = t->flags & DUI_F_LOCKED, dim = locked || (t->flags & DUI_F_DIM);
    const bool hint = (t->flags & DUI_F_HINT) && t->hold_ms;
    /* a Back / Close key carrying a toast (no status row): its outline and label in the outcome's colour */
    const bool nav_toast = (t->flags & DUI_F_NAV) && t->state != DUI_STATE_NONE && tlen(t);
    /* the key just unlocked (dui_nav_decorate): a button's phase makes it glow cyan, its HOLD tag kept */
    const bool glow = t->phase != 0;
    body(c, key_px, t->flags & DUI_F_DOWN ? DUI_RGB_BTN_DOWN : sel || glow ? DUI_RGB_BTN_SEL : DUI_RGB_BTN, t->hold / 255.0f,
         danger ? DUI_RGB_HOLD_DANGER : DUI_RGB_HOLD,
         nav_toast ? dui__state_rgb(t->state) : glow ? SUI_RGB_ACCENT : danger ? DUI_RGB_DANGER : sel ? SUI_RGB_ACCENT : DUI_RGB_BTN_EDGE);
    const int len = tlen(t);
    const bool tag = (t->flags & DUI_F_TAG) && t->caption && t->caption[0];
    if (t->caption && t->caption[0] && !tag) { /* a choice: the name, its address under it */
        label(c, t->text, len, key_px, U(42), U(50), U(76), U(26), SUI_RGB_VALUE);
        const int cl = (int)strlen(t->caption);
        const int mx = near_min(U(15), DUI_CAP_MIN_PX);
        int px = dui__font_fit(t->caption, cl, (int)U(80), mx, DUI_CAP_MIN_PX, 0);
        if (!px) px = DUI_CAP_MIN_PX;
        dui__text(c, px, U(48), mid(U(70), px), t->caption, cl, SUI_RGB_ACCENT, 0, DUI__CENTRE);
    } else if (t->flags & DUI_F_NAV) { /* Back or Close: the icon alone, or small with the title */
        if (!len) {
            dui_icon(c, t->icon, U(48), U(46), U(40), SUI_RGB_VALUE, 0, 0);
        } else {
            dui_icon(c, t->icon, U(48), U(36), U(30), SUI_RGB_VALUE, 0, 0);
            int n = len; /* a toast too long for the floor here: its first part ("Unlocked" of "Unlocked · hold ...") */
            const bool cut = nav_toast && dui_label_px(t->text, len, key_px) < DUI_LABEL_MIN_PX; /* once, not per char */
            for (int i = 1; cut && i + 4 <= len; i++)
                if (!memcmp(t->text + i, " \xC2\xB7 ", 4)) n = i;
            if (nav_toast) label(c, t->text, n, key_px, U(72), U(78), U(74), U(18), dui__state_rgb(t->state));
            else nav_title(c, t->text, len, key_px);
        }
    } else {
        const uint32_t ink = dim ? DUI_RGB_DIM : glow ? SUI_RGB_ACCENT : danger ? DUI_RGB_DANGER : sel ? SUI_RGB_ACCENT : SUI_RGB_VALUE;
        char h[48]; /* released early: the hold and what it does, in place of the label ("Hold 2 s" / "restarts") */
        const int k = hint ? dui_hint_text(t, h, sizeof h) : 0;
        int nl = 1, a = 0, b = 0, lp = 0;
        bool hy = false;
        float base2 = 0; /* the hint's second line: its baseline, its descenders U(5) clear of the outline's inside */
        if (hint) {
            lp = hint_size(h, k, key_px, &a, &b);
            nl = a ? 2 : 1;
            base2 = U(84) - (float)dui__text_descent(lp, h + b, k - b);
        } else if (len > 0) {
            lp = label_size(t->text, len, (int)U(74), (int)(U(18) + 0.001f), &nl, &a, &b, &hy);
        }
        /* every icon at one size (Companion's webhooks-logo and Satellite's satellite too); the satellite, whose
         * panel reaches the upper left, a size down and lower while the top band is taken (a lock, the HOLD tag, a
         * tag): clear of the padlock. A label on two lines: the icon fits between the top band and the
         * label's first line, never into either */
        const bool band = t->hold_ms || locked || tag;
        const bool low = band && t->icon == DUI_ICON_SAT;
        float isz = low ? U(30) : U(32), icy = low ? U(45) : U(38);
        if (nl == 2) {
            const float top = t->hold_ms || locked || tag ? U(25) : U(14);
            const float first_base = hint ? base2 - 1.02f * lp : label_cy(t->text, len, b, lp, key_px, U(72)) - 0.1f * lp;
            float room = first_base - (float)dui__cap(lp) - U(3) - top;
            if (room > U(24)) room = U(24);
            isz = room > U(12) ? room : U(12);
            icy = top + isz / 2;
        }
        dui_icon(c, t->icon, U(48), icy, isz, ink, t->rssi, t->flags);
        if (hint && nl == 2) {
            dui__text(c, lp, U(48), base2 - 1.02f * lp, h, a, SUI_RGB_WAIT, 0, DUI__CENTRE);
            dui__text(c, lp, U(48), base2, h + b, k - b, SUI_RGB_WAIT, 0, DUI__CENTRE);
        } else if (hint) {
            dui__text(c, lp, U(48), U(78), h, k, SUI_RGB_WAIT, 0, DUI__CENTRE);
        } else if (len > 0) { /* sized above already (label_size with label()'s own arguments) */
            label_draw(c, t->text, len, key_px, U(72), U(78), lp, nl, a, b, hy, dim ? DUI_RGB_DIM : glow ? SUI_RGB_ACCENT : SUI_RGB_VALUE);
        }
    }
    if (tag) { /* a small tag at the top right, clear of the outline's corner (which set Make static uses) */
        const int cl = (int)strlen(t->caption), px = near_min(U(12), DUI_CAP_MIN_PX);
        const int w = dui__text_w(px, t->caption, cl, 0, NULL);
        dui__text(c, px, U(82) - (float)w, mid(U(18), px), t->caption, cl, dim ? DUI_RGB_DIM : SUI_RGB_ACCENT, 0, DUI__LEFT);
    } else if (t->state != DUI_STATE_NONE && !(t->flags & DUI_F_NAV)) {
        rs_draw_disc(c, U(79), U(17), U(5), dui__state_rgb(t->state), 1); /* the state dot */
    }
    if (locked) { /* under the settings password: a padlock where the HOLD tag goes */
        dui_icon(c, DUI_ICON_LOCK, U(21), U(20), U(18), DUI_RGB_LOCK, 0, 0);
    } else if (t->hold_ms && !sel && !hint) {
        const int px = near_min(U(12), DUI_CAP_MIN_PX);
        dui__text(c, px, U(13), mid(U(18), px), "HOLD", 4, danger ? DUI_RGB_DANGER : SUI_RGB_LABEL, 0, DUI__LEFT);
    }
}

static void pager(const dui_canvas_t *c, const dui_tile_t *t, int key_px)
{
    body(c, key_px, t->flags & DUI_F_DOWN ? DUI_RGB_BTN_DOWN : DUI_RGB_BTN, 0, 0, DUI_RGB_BTN_EDGE);
    dui_icon(c, DUI_ICON_FWD, U(50), U(40), U(36), SUI_RGB_VALUE, 0, 0);
    const int n = t->num;
    if (n < 2) return;
    const float w = n > 7 ? 66.0f / (float)(n - 1) : 11, x0 = 48 - (float)(n - 1) * w / 2;
    for (int i = 0; i < n; i++)
        rs_draw_disc(c, U(x0 + (float)i * w), U(74), U(3.4f), i == t->cur ? SUI_RGB_ACCENT : DUI_RGB_DOT_OFF, 1);
}

static void caption(const dui_canvas_t *c, const dui_tile_t *t, int key_px);

/* The PIN's places: first..first + count, filled for the digits entered (n), an empty ring for the
 * ones still needed (up to 4, the shortest PIN), nothing beyond. */
#define PIN_SHORTEST 4
static void pin_places(const dui_canvas_t *c, float cx, float cy, int first, int count, int n, float pitch, float r,
                       float ring_w)
{
    const int need = n > PIN_SHORTEST ? n : PIN_SHORTEST;
    for (int i = 0; i < count; i++) {
        const int k = first + i;
        const float x = cx + ((float)i - (float)(count - 1) / 2) * pitch;
        if (k < n) rs_draw_disc(c, x, cy, r, SUI_RGB_ACCENT, 1);
        else if (k < need) rs_draw_ring(c, x, cy, r - ring_w / 2, ring_w, DUI_RGB_DOT_OFF);
    }
}

/* A PIN pad key: its text big in the middle (a digit, OK) or its icon (⌫, ✕); the dots under it with
 * DUI_F_DOTS (the Mini's roller). */
int dui__cap_px(const char *s, int key_px)
{
    char up[48];
    const int n = dui__upper(s, up, 47);
    return n ? dui__font_fit(up, n, (int)U(84), near_min(U(15), DUI_CAP_MIN_PX), DUI_CAP_MIN_PX, (int)lroundf(U(0.5f) * 16)) : 0;
}

static void pad_key(const dui_canvas_t *c, const dui_tile_t *t, int key_px)
{
    const bool sel = t->flags & DUI_F_SELECTED, dim = t->flags & DUI_F_DIM, dots = t->flags & DUI_F_DOTS;
    const bool cap = t->caption && t->caption[0]; /* a small caption at the top (the title, "Hold to cancel") */
    body(c, key_px, t->flags & DUI_F_DOWN ? DUI_RGB_BTN_DOWN : sel ? DUI_RGB_BTN_SEL : DUI_RGB_BTN, 0, 0,
         sel && !dim ? SUI_RGB_ACCENT : DUI_RGB_BTN_EDGE);
    if (cap) { /* at the size that fits the key (12 px at least), as a row's caption would be sized */
        const int px = dui__cap_px(t->caption, key_px);
        dui_tile_t tt = *t;
        tt.fit.cap_px = (uint8_t)(px ? px : DUI_CAP_MIN_PX);
        caption(c, &tt, key_px);
    }
    const uint32_t ink = dim ? DUI_RGB_DIM : sel ? SUI_RGB_ACCENT : SUI_RGB_VALUE;
    const float cy = (dots ? U(40) : U(48)) + (cap ? U(6) : 0);
    if (t->icon != DUI_ICON_NONE) {
        dui_icon(c, t->icon, U(48), cy, U(dots || cap ? 32 : 40), ink, 0, 0);
    } else if (tlen(t)) {
        const int len = tlen(t);
        int px = dui__font_fit(t->text, len, (int)U(76), dui__font_near(U(len == 1 ? (dots || cap ? 40 : 48) : 28)), 10, 0);
        if (!px) px = 10;
        dui__text(c, px, U(48), mid(cy, px), t->text, len, ink, 0, DUI__CENTRE);
    }
    if (dots) pin_places(c, U(48), U(cap ? 80 : 76), 0, 8, t->num, U(9.5f), U(3.4f), U(1.4f));
    if (t->flags & DUI_F_TAG) { /* ⌫ held cancels: "HOLD" top left as on a hold button, a small ✕ top right */
        const int px = near_min(U(12), DUI_CAP_MIN_PX);
        dui__text(c, px, U(13), mid(U(18), px), "HOLD", 4, dim ? DUI_RGB_DIM : SUI_RGB_LABEL, 0, DUI__LEFT);
        dui_icon(c, DUI_ICON_CLOSE, U(76), U(18), U(14), dim ? DUI_RGB_DIM : SUI_RGB_LABEL, 0, 0);
    }
}

/* The PIN entered as dots: 4 places, from cur; a small caption over them. */
static void pin_dots(const dui_canvas_t *c, const dui_tile_t *t, int key_px)
{
    const bool cap = t->caption && t->caption[0];
    if (cap) { /* at the size that fits the key, 12 px at least (the pad's title) */
        int px = t->fit.cap_px ? t->fit.cap_px : dui__cap_px(t->caption, key_px);
        if (!px) px = DUI_CAP_MIN_PX;
        dui_tile_t tt = *t;
        tt.fit.cap_px = (uint8_t)px;
        caption(c, &tt, key_px);
    }
    pin_places(c, U(48), U(cap ? 58 : 48), t->cur, 4, t->num, U(20), U(6.5f), U(2));
}

/* An address being edited: its pieces (the octets, "_" for an empty one, and the dots), *np of them;
 * oi[i] the octet index of piece i, -1 for a dot. */
static int octet_pieces(const dui_tile_t *t, char piece[9][4], int pl[9], int oi[9])
{
    const char *s = t->text ? t->text : "";
    const int len = tlen(t);
    int np = 0;
    for (int i = 0, k = 0; k < (t->num ? t->num : 1) && k < 4 && np < 8; k++) {
        int j = i;
        while (j < len && s[j] != '.') j++;
        const int n = j - i > 3 ? 3 : j - i;
        if (n) memcpy(piece[np], s + i, (size_t)n);
        else piece[np][0] = '_';
        pl[np] = n ? n : 1;
        oi[np++] = k;
        if (j < len && s[j] == '.') {
            piece[np][0] = '.';
            pl[np] = 1;
            oi[np++] = -1;
            j++;
        }
        i = j;
    }
    return np;
}

static int octet_gap(int px) { return px / 12 > 1 ? px / 12 : 1; }

/* A piece's width at px: an empty octet as wide as "00" (its outline a box, not a sliver). */
static int piece_w(int px, const char *p, int n, bool empty)
{
    const int w = dui__text_w(px, p, n, 0, NULL);
    if (!empty) return w;
    const int w00 = dui__text_w(px, "00", 2, 0, NULL);
    return w > w00 ? w : w00;
}

/* The current octet's outline: its pad around the digits, and the gap it needs on either side so the dots
 * next to it stay clear of it. */
#define OCT_PAD(key_px) U(2)
static int out_gap(int px, int key_px)
{
    const int g = (int)ceilf(OCT_PAD(key_px)) + 2;
    return g > octet_gap(px) ? g : octet_gap(px);
}

/* The gap before piece i (i >= 1) with the octet cur outlined (0xFF: none). */
static int gap_before(int i, const int *oi, int cur, int px, int key_px)
{
    return (oi[i] >= 0 && oi[i] == cur) || (oi[i - 1] >= 0 && oi[i - 1] == cur) ? out_gap(px, key_px) : octet_gap(px);
}

/* The line's width at px, the outline's room at both ends included; as wide as with the widest outlined
 * octet, so the size never changes as the outline moves. */
static int octets_w(int px, char piece[9][4], const int *pl, const int *oi, int np, int key_px)
{
    int total = (int)(2 * U(4)) + 2 * (out_gap(px, key_px) - octet_gap(px));
    for (int i = 0; i < np; i++)
        total += piece_w(px, piece[i], pl[i], oi[i] >= 0 && piece[i][0] == '_') + (i ? octet_gap(px) : 0);
    return total;
}

int dui__octets_px(const dui_tile_t *t, int key_px)
{
    char piece[9][4];
    int pl[9], oi[9];
    const int np = octet_pieces(t, piece, pl, oi);
    const int inner = key_px - 2 * SUI_MARGIN;
    for (int px = dui__font_near(U(t->num > 1 ? 30 : 40)); px >= DUI_TEXT_MIN_PX; px--)
        if (dui__cap(px) && octets_w(px, piece, pl, oi, np, key_px) <= inner) return px;
    return DUI_TEXT_MIN_PX; /* never smaller than the minimum */
}

/* An address being edited: its octets on one line at one size (fit.px when the row set it: the
 * two-octet pair's), the current one outlined in cyan and white, the others in its role's colour, an empty one a dim
 * "_"; a trailing '.' when the address goes on on the next key. */
static void octets(const dui_canvas_t *c, const dui_tile_t *t, int key_px)
{
    const bool cap = t->caption && t->caption[0];
    if (cap) caption(c, t, key_px);
    char piece[9][4];
    int pl[9], oi[9];
    const int np = octet_pieces(t, piece, pl, oi);
    const int px = t->fit.px ? t->fit.px : dui__octets_px(t, key_px);
    int total = 0; /* as drawn: the gaps by the outlined octet wider */
    for (int i = 0; i < np; i++)
        total += piece_w(px, piece[i], pl[i], oi[i] >= 0 && piece[i][0] == '_') + (i ? gap_before(i, oi, t->cur, px, key_px) : 0);
    float x = (float)key_px / 2 - (float)total / 2;
    const float base = cap ? U(66) : mid(U(48), px), ch = (float)dui__cap(px);
    for (int i = 0; i < np; i++) {
        const bool none = oi[i] >= 0 && piece[i][0] == '_', cur = oi[i] >= 0 && oi[i] == t->cur;
        const int w = piece_w(px, piece[i], pl[i], none), iw = dui__text_w(px, piece[i], pl[i], 0, NULL);
        if (i) x += (float)gap_before(i, oi, t->cur, px, key_px);
        if (cur) {
            const float bp = OCT_PAD(key_px);
            rs_draw_rrect(c, x - bp, base - ch - bp, (float)w + 2 * bp, ch + 2 * bp, bp, U(1.5f), SUI_RGB_ACCENT);
        }
        const uint32_t ink = oi[i] < 0 ? SUI_RGB_LABEL : none ? DUI_RGB_DIM : cur ? SUI_RGB_VALUE : dui__role_rgb(t->role);
        dui__text(c, px, x + (float)(w - iw) / 2, base, piece[i], pl[i], ink, 0, DUI__LEFT);
        x += (float)w;
    }
}

static void qr(const dui_canvas_t *c, const dui_tile_t *t, int key_px)
{
    uint8_t code[DUI_QR_BUF];
    char payload[160];
    const int len = tlen(t) < (int)sizeof payload - 1 ? tlen(t) : (int)sizeof payload - 1;
    memcpy(payload, t->text ? t->text : "", (size_t)len);
    payload[len] = '\0';
    rs_fill(c, 0, 0, key_px, key_px, DUI_RGB_QR_LIGHT);
    if (!dui_qr_encode(payload, code)) {
        label(c, "Too long", 8, key_px, U(48), U(56), U(84), U(24), SUI_RGB_BAD);
        return;
    }
    const int n = dui_qr_size(code), m = dui_qr_module_px(key_px, n), off = (key_px - m * n) / 2;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (dui_qr_dark(code, x, y)) rs_fill(c, off + x * m, off + y * m, m, m, DUI_RGB_QR_DARK);
}

static void caption(const dui_canvas_t *c, const dui_tile_t *t, int key_px)
{
    if (!t->caption || !t->caption[0]) return;
    char up[32];
    const int n = dui__upper(t->caption, up, 31);
    const int spacing = (int)lroundf(U(0.5f) * 16),
              px = t->fit.cap_px ? t->fit.cap_px : near_min(U(15), DUI_CAP_MIN_PX);
    dui__text(c, px, U(48), mid(U(17), px), up, n, SUI_RGB_LABEL, spacing, DUI__CENTRE);
}

static void step(const dui_canvas_t *c, const dui_tile_t *t, int key_px)
{
    rs_draw_ring(c, U(48), U(25), U(13), U(2.5f), SUI_RGB_ACCENT);
    char num[4];
    const int nn = t->num > 9 ? 2 : 1;
    num[0] = (char)(nn == 2 ? '0' + t->num / 10 % 10 : '0' + t->num);
    num[1] = (char)('0' + t->num % 10);
    const int npx = dui__font_near(U(19));
    dui__text(c, npx, U(48), mid(U(26.5f), npx), num, nn, SUI_RGB_ACCENT, 0, DUI__CENTRE);
    fitted_text(c, t, key_px, U(60), SUI_RGB_VALUE);
    if (t->caption && t->caption[0]) { /* the sub-line: inside the margins, descenders too, or left out */
        const int cl = (int)strlen(t->caption);
        const int mx = near_min(U(13), DUI_CAP_MIN_PX);
        const int px = dui__font_fit(t->caption, cl, key_px - 2 * SUI_MARGIN, mx, DUI_CAP_MIN_PX, 0);
        if (px) {
            float base = mid(U(87), px);
            const float low = (float)(key_px - SUI_MARGIN - dui__descent(px));
            if (base > low) base = low;
            dui__text(c, px, U(48), base, t->caption, cl, SUI_RGB_LABEL, 0, DUI__CENTRE);
        }
    }
}

void dui_render_key(const dui_tile_t *t0, int key_px, uint8_t *rgb)
{
    const dui_canvas_t c = {rgb, key_px, key_px};
    dui_tile_t t = *t0;
    const uint32_t bg = dui__tint_rgb(t.tint);
    rs_fill(&c, 0, 0, key_px, key_px, bg);
    if ((t.kind == DUI_TILE_WORD || t.kind == DUI_TILE_CAP || t.kind == DUI_TILE_STEP) && t.fit.nl == 0) {
        dui_tile_t *one = &t; /* not aligned in a row: a row of its own */
        dui_align_row(&one, 1, key_px);
    }
    switch (t.kind) {
    case DUI_TILE_WORD: fitted_text(&c, &t, key_px, U(t.fit.rowcap ? 57 : 48), dui__role_rgb(t.role)); break;
    case DUI_TILE_CAP:
        caption(&c, &t, key_px);
        fitted_text(&c, &t, key_px, U(57), dui__role_rgb(t.role));
        break;
    case DUI_TILE_STEP: step(&c, &t, key_px); break;
    case DUI_TILE_BUTTON: button(&c, &t, key_px); break;
    case DUI_TILE_ICON: {
        dui_icon(&c, t.icon, U(48), U(36), U(36), t.flags & DUI_F_DIM ? DUI_RGB_DIM : SUI_RGB_VALUE, t.rssi, t.flags);
        const int len = tlen(&t);
        const int mx = near_min(U(17), DUI_LABEL_MIN_PX);
        int px = dui__font_fit(t.text, len, key_px - 2 * SUI_MARGIN, mx, DUI_LABEL_MIN_PX, 0);
        if (!px) px = DUI_LABEL_MIN_PX;
        dui__text(&c, px, U(48), mid(U(76), px), t.text, len, SUI_RGB_LABEL, 0, DUI__CENTRE);
        break;
    }
    case DUI_TILE_GLYPH: glyph(&c, t.state, U(48), U(48), U(24), bg, t.phase); break;
    case DUI_TILE_IND: /* a status icon, a button too: no outline, brighter while held */
        if (t.flags & DUI_F_DOWN) rs_draw_rrect(&c, U(5), U(5), U(86), U(86), U(14), 0, DUI_RGB_BTN_DOWN);
        dui_icon(&c, t.icon, U(48), U(48), U(34), dui__sign_rgb(t.icon), t.rssi, t.flags);
        break;
    case DUI_TILE_PAGER: pager(&c, &t, key_px); break;
    case DUI_TILE_QR: qr(&c, &t, key_px); break;
    case DUI_TILE_KEY: pad_key(&c, &t, key_px); break;
    case DUI_TILE_DOTS: pin_dots(&c, &t, key_px); break;
    case DUI_TILE_OCTETS: octets(&c, &t, key_px); break;
    case DUI_TILE_SPIN: spinner(&c, key_px, t.phase); break;
    default: break; /* DUI_TILE_TOAST is never made: toasts are status words, the Back key's label or the bar */
    }
}

/* ---- the status bar: the strip, the info bar ---- */

int dui__signs(const dui_status_t *st, dui__sign_t out[3])
{
    /* net WIFI means joined unless the Wi-Fi sign says off (a caller that sets only net gets its bars) */
    const bool wifi_up = st->wifi == DUI_SIGN_UP || (st->net == DUI_NET_WIFI && st->wifi != DUI_SIGN_OFF);
    const bool eth = st->eth_link || st->net == DUI_NET_ETH;
    int n = 0;
    out[n++] = (dui__sign_t){(uint8_t)(st->hotspot ? DUI_ICON_HOTSPOT : DUI_ICON_HOTSPOT_OFF), 0, DUI_ACT_HOTSPOT};
    out[n++] = (dui__sign_t){(uint8_t)(wifi_up ? DUI_ICON_WIFI : st->wifi == DUI_SIGN_OFF ? DUI_ICON_WIFI_OFF : DUI_ICON_NONET),
                             (uint8_t)(st->net == DUI_NET_ETH ? 1 : 2), DUI_ACT_NET};
    if (eth) out[n++] = (dui__sign_t){DUI_ICON_ETH, (uint8_t)(st->net == DUI_NET_ETH ? 2 : 1), DUI_ACT_NET};
    return n;
}

void dui__signs_keep(const dui__sign_t *s, int n, int keep, bool shown[3])
{
    for (int i = 0; i < n; i++) { /* shown when fewer than keep outrank it (a tie: the one further right first) */
        int above = 0;
        for (int j = 0; j < n; j++) above += j != i && (s[j].prio > s[i].prio || (s[j].prio == s[i].prio && j > i));
        shown[i] = above < keep;
    }
}

uint32_t dui__sign_rgb(int icon)
{
    return icon == DUI_ICON_WIFI_OFF || icon == DUI_ICON_HOTSPOT_OFF ? DUI_RGB_DIM /* off by choice: a normal state */
           : icon == DUI_ICON_NONET                                    ? SUI_RGB_LABEL /* its slash red */
                                                                       : SUI_RGB_VALUE;
}

/* The bar's signs: their icons and actions in the fixed order, at most 2 on the info bar (its text's room),
 * the rest kept longest; each one's centre x. Returns how many. */
static int bar_signs(const dui_status_t *st, int w, int h, dui__sign_t *out, float *cx)
{
    const bool small = h < 80;
    const int pad = small ? 8 : 22;
    dui__sign_t sg[3];
    bool shown[3];
    const int n = dui__signs(st, sg);
    dui__signs_keep(sg, n, small ? 2 : 3, shown);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (shown[i]) out[k++] = sg[i];
    const float ix = (float)(w - pad - (small ? 11 : 24)), igap = small ? 26.0f : 60.0f;
    for (int i = 0; i < k; i++) cx[i] = ix - (float)(k - 1 - i) * igap; /* right-aligned */
    return k;
}

int dui__bar_px(const char *s, int n, int room, bool small)
{
    const int px = small ? DUI_BAR_INFO_PX : DUI_BAR_STRIP_PX;
    if (!s || n <= 0 || room <= 0) return 0;
    if (dui__text_w(px, s, n, 0, NULL) <= room) return px;
    const int fs = dui__font_fit(s, n, room, px, DUI_TEXT_MIN_PX, 0); /* 16 px or more ... */
    return fs ? fs : dui__font_fit(s, n, room, px, 10, 0);          /* ... else the best it can, never cut */
}

/* Words joined by spaces. */
/* The status's text: a sub-screen's title, else st->text or the words (form 0); the words (form 1); the short
 * words (form 2). 0: no such form. */
static int bar_text(const dui_status_t *st, bool sub, int form, char *out, int n)
{
    out[0] = '\0';
    if (sub) return form ? 0 : dui__join_words(st->title, out, n);
    if (form == 0 && st->text) {
        const int k = (int)strlen(st->text);
        const int m = k < n - 1 ? k : n - 1;
        memcpy(out, st->text, (size_t)m);
        out[m] = '\0';
        return m;
    }
    if (form == 2) return st->short_word[0] ? dui__join_words(st->short_word, out, n) : 0;
    return dui__join_words(st->word, out, n);
}

void dui_render_bar(const dui_status_t *st, bool sub, int page, int pages, int w, int h, uint8_t *rgb)
{
    const dui_canvas_t c = {rgb, w, h};
    /* a toast takes the bar over for its time: its outcome's tint, glyph and colour, its words alone */
    const bool small = h < 80, toast = st->toast && st->toast[0];
    const int state = toast ? (st->toast_state != DUI_STATE_NONE ? st->toast_state : DUI_STATE_OK) : st->state;
    const uint32_t tint = sub && !toast ? DUI_RGB_TINT_NEUTRAL
                                        : dui__tint_rgb(state == DUI_STATE_BAD    ? DUI_TINT_BAD
                                                        : state == DUI_STATE_WAIT ? DUI_TINT_WAIT
                                                        : state == DUI_STATE_OK   ? DUI_TINT_OK
                                                                                  : DUI_TINT_BLACK);
    rs_fill(&c, 0, 0, w, h, tint);
    const int pad = small ? 8 : 22, r = small ? 13 : 30;
    int x = pad;
    if ((toast || !sub) && state != DUI_STATE_NONE) {
        glyph(&c, state, (float)(x + r), h / 2.0f, (float)r, tint, st->phase);
        x += 2 * r + (small ? 8 : 18);
    }
    /* the signs at the right: the hotspot, Wi-Fi, Ethernet */
    const float isz = small ? 22.0f : 40.0f;
    dui__sign_t sg[3];
    float scx[3];
    const int nsg = bar_signs(st, w, h, sg, scx);
    for (int i = 0; i < nsg; i++)
        dui_icon(&c, sg[i].icon, scx[i], h / 2.0f, isz, dui__sign_rgb(sg[i].icon), sg[i].icon == DUI_ICON_WIFI ? st->rssi : 0, 0);
    /* the room the icons, and the page dots on the strip, keep at the right (the text stops 14 px short
     * of the first dot) */
    const float dot_dx = 13, dot_x0 = (float)w - 170 - (float)(pages - 1) * dot_dx - (float)(nsg - 2) * 60, dot_r = 4.5f;
    const int right = small ? 30 + 26 * (nsg - 1)
                      : pages > 1 ? (int)((float)w - dot_x0 + dot_r) + 14 - pad : 130 + 60 * (nsg - 2);
    char text[96] = "";
    const int room = w - x - right - pad;
    int n = 0;
    uint32_t ink = sub ? SUI_RGB_VALUE : dui__state_rgb(st->state);
    if (toast) { /* the toast in place of the status (or title) words, never after them */
        n = (int)strlen(st->toast) < 95 ? (int)strlen(st->toast) : 95;
        memcpy(text, st->toast, (size_t)n);
        ink = dui__state_rgb(state);
    } else { /* one size per bar; the first form of the status that fits at it ("Waiting for Companion",
              * else "Waiting"); none: the last, smaller */
        const int bar_px = small ? DUI_BAR_INFO_PX : DUI_BAR_STRIP_PX;
        for (int form = 0; form < 3; form++) {
            char f[96];
            const int k = bar_text(st, sub, form, f, sizeof f);
            if (!k) continue;
            memcpy(text, f, (size_t)k + 1);
            n = k;
            if (dui__text_w(bar_px, text, n, 0, NULL) <= room) break;
        }
    }
    const int fs = dui__bar_px(text, n, room, small); /* 16 px or more, never cut */
    if (fs) dui__text(&c, fs, (float)x, mid(h / 2.0f + 2, fs), text, n, ink, 0, DUI__LEFT);
    if (pages > 1 && !small) /* the page dots (the strip) */
        for (int i = 0; i < pages; i++)
            rs_draw_disc(&c, dot_x0 + (float)i * dot_dx, h / 2.0f, dot_r, i == page ? SUI_RGB_ACCENT : DUI_RGB_DOT_OFF, 1);
}

int dui_bar_hit(const dui_status_t *st, int w, int h, int x)
{
    /* dui_render_bar's signs, each igap apart: a tap within half a gap of one, the rightmost to the edge */
    const float half = (h < 80 ? 26.0f : 60.0f) / 2;
    dui__sign_t sg[3];
    float cx[3];
    const int n = bar_signs(st, w, h, sg, cx);
    for (int i = n - 1; i >= 0; i--)
        if ((float)x >= cx[i] - half && (i == n - 1 ? x < w : (float)x < cx[i] + half)) return sg[i].action;
    return 0;
}

int dui_hint_text(const dui_tile_t *t, char *out, int n)
{
    if (!t || !out || n < 8) return 0;
    const unsigned ms = t->hold_ms, sec = ms / 1000, tenth = ms % 1000 / 100;
    int k = !ms ? snprintf(out, (size_t)n, "Hold") : tenth ? snprintf(out, (size_t)n, "Hold %u.%u s", sec, tenth)
                                                           : snprintf(out, (size_t)n, "Hold %u s", sec);
    if (t->hint && t->hint[0] && k < n) k += snprintf(out + k, (size_t)(n - k), " \xC2\xB7 %s", t->hint);
    return k < n ? k : n - 1;
}

int dui_hint_px(const dui_tile_t *t, int key_px)
{
    char h[48];
    const int n = dui_hint_text(t, h, sizeof h);
    int a, b; /* as button() draws it */
    return n ? hint_size(h, n, key_px, &a, &b) : 0;
}

int dui_label_px(const char *s, int len, int key_px)
{
    int nl, a, b; /* a button's label: as button() draws it */
    bool h;
    return s && len > 0 ? label_size(s, len, (int)U(74), (int)(U(18) + 0.001f), &nl, &a, &b, &h) : 0;
}
