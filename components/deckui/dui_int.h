#pragma once
/* Private to deckui: the colours, and text in the deck font (statusui's deck_font.h). */
#include <stdbool.h>
#include <stdint.h>

#include "deckui/deckui.h"

uint32_t dui__role_rgb(int role);
uint32_t dui__tint_rgb(int tint);
uint32_t dui__state_rgb(int state);

// The font size nearest px (ties: the smaller), of the sizes the deck font has.
int dui__font_near(float px);
// The largest size from min_px to max_px at which s[0..n) is at most max_w px of ink wide on one
// line; 0: none.
int dui__font_fit(const char *s, int n, int max_w, int max_px, int min_px, int spacing16);
// The ink width of s[0..n) at size px, letter-spaced spacing16 / 16 px; *left (may be NULL): where
// the ink starts from the pen. "\xC2\xB7" (a middle dot) counts as one glyph. Plain text (no spacing,
// no middle dot): statusui's sui_text_width.
int dui__text_w(int px, const char *s, int n, int spacing16, int *left);
// The cap height at size px (0: no such size).
int dui__cap(int px);
// The deepest descender under the baseline at size px (g, j, p, q, y, the comma), px.
int dui__descent(int px);
// How far s[0..n)'s own glyphs reach under the baseline at size px (0 or about 1 without a descender).
int dui__text_descent(int px, const char *s, int n);
// Host tests only (NULL in the firmware): dui__text calls it with each text's size and ink box (canvas px, x1
// and y1 past the last ink pixel), so a test can check sizes and that nothing is drawn outside the margins.
extern void (*dui__text_probe)(int px, int x0, int y0, int x1, int y1);

enum { DUI__LEFT, DUI__CENTRE };
// Draws s[0..n) at size px with its baseline at y: its ink starting at x (DUI__LEFT) or centred on x
// (DUI__CENTRE), alpha-blended in rgb, clipped to the canvas.
void dui__text(const dui_canvas_t *c, int px, float x, float y, const char *s, int n, uint32_t rgb, int spacing16,
               int align);
// A label in two lines where it reads best (after a '-', before a capital after a small letter, at a
// space, else in the middle): *a chars on the first line, the second from *b.
void dui__split2(const char *s, int n, int *a, int *b);
// The words w[0..] (up to DUI_STATUS_WORDS, to the first NULL) joined with one space into out (n bytes), as many
// whole words as fit with the NUL; its length (the status bar's text, the one-key and Back-key titles).
int dui__join_words(const char *const *w, char *out, int n);
// s's first max chars (s NULL: none) with a-z made A-Z into out, NUL-terminated; its length (the captions).
int dui__upper(const char *s, char *out, int max);
// FNV-1a over p[0..n) from h (fp.c's; the PIN pad's and the editor's bar fingerprints).
#define DUI__FNV_BASIS 2166136261u
uint32_t dui__fnv(uint32_t h, const void *p, size_t n);
// s[0..len) in 2 lines within inner px at the largest size from min_px to max_px: at a natural break
// (after '-', '.' or '/', at a space, before a capital after a small letter, between a letter and a digit)
// when one reaches min_px; else, with hyphen_ok (a word: a status word, a label), at a syllable-like point
// with a '-' added to the first line ("Recon-" / "necting"); else anywhere (with the '-' where it may be
// added); the most even of the breaks at that size. Returns the size (0: none reaches min_px), *cut the chars
// of the first line (without the '-'), *next where the second starts, *hyph (may be NULL) a '-' is added.
int dui__two_lines(const char *s, int len, int inner, int max_px, int min_px, bool hyphen_ok, int *cut, int *next,
                   bool *hyph);
// a value fits one key of key_px at DUI_TEXT_MIN_PX or more, on 1 or 2 lines (small decks keep it whole).
bool dui__value_fits(const char *s, int len, int key_px);
// the status row's one text size on a key of key_px (dui_align_status): the largest, up to 30 px on a
// 96 px key, that holds DUI_STATUS_WHOLE on one line: the longest word the box keeps whole in a status or a title
// ("Companion": 16 px on a 72 px key, 18 on 80, 21 on 96, 24 on 112, 28 on 120).
#define DUI_STATUS_WHOLE "Companion"
int dui__status_px(int key_px);
// A pad key's or the dots' caption (upper case, spaced) on a key of key_px: the size it is drawn at, up to U(15),
// DUI_CAP_MIN_PX or more; 0: it doesn't fit at that (one helper for draw.c and pin.c).
int dui__cap_px(const char *s, int key_px);
// the status bar's one text size, whatever the state: the info bar's (the Neo's 248 x 58) and the
// strip's (the + and + XL). dui__bar_px: a text of n chars in room px at that size when it fits, else the largest
// that does (16 px or more, then 10), never cut; 0 when nothing does.
#define DUI_BAR_INFO_PX 20
#define DUI_BAR_STRIP_PX 50
int dui__bar_px(const char *s, int n, int room, bool small);

// the status signs in their fixed order (left to right): the hotspot (always; crossed when off), Wi-Fi (its bars,
// or crossed: grey when off by choice, red when on and not joined), Ethernet (while its cable is linked). prio: how
// long each is kept when there is no room for all (the route's sign the longest, the hotspot the least).
typedef struct {
    uint8_t icon;    // DUI_ICON_*
    uint8_t prio;
    uint16_t action; // DUI_ACT_HOTSPOT, DUI_ACT_NET
} dui__sign_t;
int dui__signs(const dui_status_t *st, dui__sign_t out[3]);
// Of the n signs, the keep kept longest: shown[i] (their order stays).
void dui__signs_keep(const dui__sign_t *s, int n, int keep, bool shown[3]);
// A sign's ink: grey for the crossed-by-choice ones, the label's for the red-slashed Wi-Fi, else the value's.
uint32_t dui__sign_rgb(int icon);
// the size an octets tile (DUI_TILE_OCTETS) is drawn at on a key of key_px: the largest that keeps the
// line and the current octet's outline inside the margins, not below DUI_TEXT_MIN_PX.
int dui__octets_px(const dui_tile_t *t, int key_px);

// ---- The pads' layouts (the PIN pad; the static-address editor): shared, pad.c ----
extern const char *const dui__digit[10]; // "0".."9"
// L empty, for deck d: the whole grid, one page, no reserved keys, the status where the deck has it.
void dui__lay_init(dui_layout_t *L, const dui_deck_t *d);
// A zeroed tile of kind on key (col, row) of page; one out of the grid or past the tiles is a sink and sets
// L->overflow.
dui_tile_t *dui__lay_put(dui_layout_t *L, int page, int col, int row, int kind);
// Row 0's words take one size, on one baseline: they are read as one line; the status size.
void dui__lay_align_row0(dui_layout_t *L);
// A pad's key actions: what each of its ids shows (0: the pad has none).
typedef struct {
    uint16_t digit0;  // digit0 + 0..9: the digit
    uint16_t del, cancel, ok, down, up, add;
    const char *ok_text; // OK's text ("OK", "Next"), selected
    const char *add_text; // add's text (the roller's: "Next digit"; NULL: "Add")
} dui__pad_ids_t;
// A pad key (DUI_TILE_KEY, on the main tint) for action, shown as ids say.
dui_tile_t *dui__pad_key(dui_layout_t *L, int page, int col, int row, uint16_t action, const dui__pad_ids_t *ids);
