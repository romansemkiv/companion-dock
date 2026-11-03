#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- The palette: every colour the box's screens use, 0xRRGGBB, here only ----
// Text. Values are white; the state colours follow the status light's meaning (fbstatus.h); the
// accent is cyan, far in hue from every state colour, so an address never reads as a state.
#define SUI_RGB_VALUE 0xFFFFFF  // the SSID, the password, versions, a host name: what to read off exactly
#define SUI_RGB_LABEL 0x9AA0A6  // labels and hints: "WiFi", "Pass", "port", "No password"
#define SUI_RGB_ACCENT 0x5CD6F0 // IPv4 octets and port numbers: what to type into Companion (cyan)
#define SUI_RGB_BAD 0xFF5C5C    // state "something is wrong": no network, Companion too old (the light's red)
#define SUI_RGB_WAIT 0xFF9A1F   // state "waiting": finding/choosing Companion, reconnecting, dock waiting (amber)
#define SUI_RGB_OK 0x5CDB6E     // state "connected" (the light's green; no screen says so yet)
#define SUI_RGB_PLAIN 0xFFFFFF  // the status words of a screen with no state
// Key backgrounds: each line's keys share a very dark tint, so a line that wraps reads as one group.
// Text on them stays at WCAG AA (4.5:1) or more (test_palette_contrast).
#define SUI_RGB_TINT_BAD 0x3A1010     // a "something is wrong" status line (dark red)
#define SUI_RGB_TINT_WAIT 0x3A2408    // a "waiting" status line (dark amber)
#define SUI_RGB_TINT_OK 0x0F3016      // a "connected" status line (dark green)
#define SUI_RGB_TINT_MAIN 0x10234A    // the main zone: what the person needs now (deep navy)
#define SUI_RGB_TINT_SERVICE 0x2C1448 // the service zone: the hotspot's name and password (deep purple)
// The divider bar between two lines sharing a row: a lighter shade of the zone's tint.
#define SUI_RGB_DIVIDER_SERVICE 0x9468D0 // on the service tint (4.3:1 against it)
#define SUI_RGB_DIVIDER SUI_RGB_DIVIDER_SERVICE

// Each word of a screen has a role, which gives its colour (sui_role_rgb): set by the screen builders,
// SUI_ROLE_FG (c->fg) for custom messages and anything else.
enum {
    SUI_ROLE_FG,     // the content's fg: custom messages (Deck.ShowMessage)
    SUI_ROLE_STATUS, // the screen's state colour: the main message ("No network", "Finding Companion")
    SUI_ROLE_LABEL,  // grey: hints and labels ("WiFi", "Pass", "port", "No password")
    SUI_ROLE_ACCENT, // cyan: IPv4 octets and port numbers
    SUI_ROLE_VALUE,  // white: the SSID, the password, a version, a host name (an IPv4 address: accent)
    SUI_ROLES
};
#define SUI_LINE_ROLES 4 // roles kept per line: word w has role[line][w], the words after the last the last

// A screen's state, as the status light shows it: its status words' colour and their tint.
enum { SUI_STATE_NONE, SUI_STATE_BAD, SUI_STATE_WAIT, SUI_STATE_OK };

// The group of a line: its zone on a status screen and the background of its keys
// (sui_line_bg).
enum {
    SUI_GROUP_NONE,    // no zone, c->bg: custom messages
    SUI_GROUP_STATUS,  // the status zone, the top row on every page: the tint of the screen's state
    SUI_GROUP_MAIN,    // the main zone, the rows under it: what the person needs now (navy)
    SUI_GROUP_SERVICE, // the service zone, the bottom row(s): the hotspot's name and password (purple)
};

#define SUI_LINES 6 // lines of a screen at most

typedef struct {
    const char *lines[SUI_LINES];
    int n;
    uint8_t fg[3], bg[3];
    uint8_t keep_next;  // bit i: line i goes on the same page as line i + 1 (SUI_KEEP_NEXT)
    uint8_t role[SUI_LINES][SUI_LINE_ROLES]; // the words' roles (all SUI_ROLE_FG when zeroed)
    uint8_t state;      // SUI_STATE_* (NONE when zeroed)
    uint8_t group[SUI_LINES]; // each line's SUI_GROUP_* (NONE when zeroed: no zones)
    // backing store for lines[] set by the screen builders: the longest screen is "connecting" with a
    // 63-char host, a 15-char IP, a 32-char SSID and a 64-char password
    char storage[256];
} sui_content_t;

// The colour of role `role` in c: the palette's; the state's for SUI_ROLE_STATUS; c->fg for
// SUI_ROLE_FG and any unknown role.
const uint8_t *sui_role_rgb(const sui_content_t *c, int role);
// The background of the keys of line `line` of c: its group's tint, c->bg for SUI_GROUP_NONE (and for
// SUI_GROUP_STATUS with no state).
const uint8_t *sui_line_bg(const sui_content_t *c, int line);

// ---- Word-per-key layout ----
// Every text the box draws on the keys is laid out one word per key. Each line of the content starts
// a new row of keys; its words (split on spaces) fill the row left to right and wrap onto the next
// row. An IPv4 address takes four keys, one octet each, without the dots; every other word stays whole
// on one key. Empty lines are empty rows (none at the top or the bottom); keys left over show c->bg,
// a word's key its line's background (sui_line_bg).
// A word is drawn in the deck font (deck_font.h, anti-aliased), centred in its key, as large as fits
// within SUI_MARGIN px of the key's edges and no larger than SUI_CAP_PCT % of the key (sui_fit).
// It stays on one line unless that is smaller than SUI_MIN_PX; then it takes 2 text lines (never more),
// broken after its own '-' or '.' when that reads; else a status or label word is hyphenated (an
// added '-', 3 or more letters a side: "Recon-" / "necting") and a value is broken before a capital
// that follows a small letter ("Companion|Box"); else as evenly as it can be. Nothing is ever added to a
// value (SSID, password, version, IP, port). In a line, the words of one role that fit on one line all
// take the smallest of their sizes, so the line looks even; other roles keep theirs.
// A value (SSID, password, version, host) that doesn't reach the comfortable size (sui_comfort_px) on
// one line in one key goes over consecutive keys instead, one line each, all at one size: the smallest
// of the pieces' own sizes. It is cut at its natural breaks (before a capital that follows a small
// letter, after a '-' or '.', between a letter and a digit), not inside a part between them that reads
// on its own (SUI_MIN_PX); then in the fewest keys that make every piece comfortable, else at the
// largest size; then at the better breaks, then as evenly as it can be. Any other word one key can't
// hold readably goes on over the next keys of the row, then the next row, in the fewest pieces that
// read, ending after its own '-' or '.' where they can, else as even as they can be: in order, nothing
// dropped, never cut short. A word's pieces that don't fit in what is left of a row start the
// next row; so do an IPv4 address's octets and a label with the number after it ("port 5343"). An
// address wider than a row goes from a new row in balanced rows (2 + 2 on 3 keys), never a lone octet
//. A label line that introduces a value line keeps with it (keep_next).
// A status screen (lines in zones) is laid out in zones: the status lines at the top of every page,
// the main lines under them (each from a new row), the service lines (the hotspot's) packed into the
// bottom row(s), a line starting a new row only when it doesn't fit in what is left of one; the rows
// between main and service stay empty. When it doesn't all fit, the pages are status + main (main over
// as many pages as it needs), then status + service, anchored at the bottom.
// Content with more rows than the deck has is split into pages at row boundaries, at the end of a
// line where it can be: a line goes on the next page when it doesn't fit on this one, and is split
// over pages only when it alone needs more rows than the deck has. Lines marked keep-with-next
// (c->keep_next) go on the next page together, unless together they need more rows than the deck
// has. No page starts with an empty row.

#define SUI_CELLS_MAX 80 // words laid out: 4 lines of 40 chars hold at most 80
#define SUI_PAGE_ALL 0xFF // sui_cell_t.page of a cell drawn on every page: the status row
#define SUI_KEY_LINES 2  // text lines of one word in its key, at most
#define SUI_MARGIN 4     // px between a key's edge and its text
#define SUI_CAP_PCT 42   // a word is never larger than this % of the key ("8" doesn't dwarf "192")
#define SUI_MIN_PX 14    // the smallest size that reads well on a 72 px key: smaller words are split
#define SUI_FIT_MAX 128  // chars of a word sui_fit looks at (longer: it doesn't fit)
#define SUI_PAGE_US (3000 * 1000LL)      // a screen with more than one page shows the next one this often
#define SUI_PAGE_FAST_US (2000 * 1000LL) // ... and this often when it has more than SUI_PAGE_MANY pages
#define SUI_PAGE_MANY 3

// A word in its key: its size and text lines.
typedef struct {
    uint8_t px;                 // the font size (a sui_font[] size); 0: nothing is drawn
    uint8_t nl;                 // text lines (1..SUI_KEY_LINES)
    uint8_t cut[SUI_KEY_LINES]; // chars on each text line, in order; they add up to the word
    uint8_t hyph;               // 1: the first line ends in an added '-' (a hyphenated word)
} sui_fit_t;

// The word s[0..len) in a key of key_px: the largest size at which it fits on one line
// within SUI_MARGIN of the edges (the font's tallest and deepest glyphs too), at most SUI_CAP_PCT % of
// key_px. When that is below SUI_MIN_PX, 2 lines: split after its own '-' or '.' at the largest size such
// a split reaches when that is SUI_MIN_PX or more; else, with hyphen (a status or label word), with an
// added '-' between two letters, 3 or more letters a side (then 1 or more), the same way; else, without
// hyphen (a value), between a small letter and a capital; else at the largest size any split reaches.
// Of the splits at that size (of that kind), the narrowest widest line, then the widest narrowest, then
// the longer first line. When neither reads (a word too long for one key, which sui_layout never hands
// it), the larger.
sui_fit_t sui_fit(const char *s, int len, int key_px, bool hyphen);

// sui_fit with the left `inset` px of the key not available to the text (a divider bar): the text
// fits within SUI_MARGIN of the right edge and of the inset. sui_fit is inset 0.
sui_fit_t sui_fit_inset(const char *s, int len, int key_px, bool hyphen, int inset);

// The divider bar on a key of key_px: x, y, w, h of its box. About 4 px wide on 96 (scaled, at
// least 2), 60 % of the key high, centred down, inset about 3 px (scaled) from the left edge.
void sui_divider_box(int key_px, int *x, int *y, int *w, int *h);
// The px from the key's left edge the bar takes (x + w): the text of a divider key starts SUI_MARGIN past it.
int sui_divider_inset(int key_px);
// The colour of the divider bar of a key of line `line` of c (a shade of its zone's tint).
const uint8_t *sui_line_divider_rgb(const sui_content_t *c, int line);

// The comfortable size on a key of key_px: max(18 px, a quarter of the key, rounded).
int sui_comfort_px(int key_px);

// The ink width in px of s[0..n) at font size px on one line; *left (may be NULL): where the ink starts,
// from the pen of its first char. 0 for an empty string or a size sui_font doesn't have.
int sui_text_width(int px, const char *s, int n, int *left);

typedef struct {
    uint8_t line, start, len; // the word: c->lines[line][start .. start + len)
    uint8_t page, row, col;   // the key it is drawn on
    uint8_t role;             // its colour (SUI_ROLE_*)
    uint8_t piece;            // 0: starts a group (a word, an IPv4 address, a label and its number);
                              // k: the group's k-th cell after that
    uint8_t even;             // its group, when wider than a row, goes from a new row in balanced rows
    uint8_t divider;          // 1: a divider bar at its key's left edge: it starts a line that shares a row
                              // with the line before; its text is fitted to the room right of the bar
    sui_fit_t fit;            // its size and text lines
} sui_cell_t;

typedef struct {
    int n;     // cells
    int pages; // >= 1
    sui_cell_t cell[SUI_CELLS_MAX];
} sui_layout_t;

// Lays c out on a deck of cols x rows keys of key_px px (lines beyond SUI_LINES and words beyond
// SUI_CELLS_MAX are dropped).
void sui_layout(int cols, int rows, int key_px, const sui_content_t *c, sui_layout_t *out);
// The word on key (key_col, key_row) of page page, or NULL for a key left blank.
const sui_cell_t *sui_layout_cell(const sui_layout_t *L, int page, int key_col, int key_row);
// Pages c takes on the deck (1: it fits, and nothing needs redrawing to page through it).
int sui_page_count(int cols, int rows, int key_px, const sui_content_t *c);

// The cells one word s[0..len) takes on keys of key_px, as sui_layout cuts a word of role `role` (no
// IPv4 split): one when it reads in one key; a value (SUI_ROLE_VALUE) that doesn't reach the
// comfortable size over consecutive keys at one size; any other word one key can't hold in the
// fewest pieces that read. Fills out[] (start: the offset in s, len, role, piece, fit; the rest
// zero) up to max cells and returns how many it filled. For the maintenance UI (deckui).
int sui_word_cells(const char *s, int len, int key_px, int role, sui_cell_t *out, int max);

// Renders key (key_col, key_row) of page page (taken modulo the page count) into rgb_out
// (key_px * key_px * 3 bytes, RGB888): its word in its role's colour, alpha-blended onto its line's
// background (sui_line_bg); a key with no word all c->bg.
void sui_render_key(int cols, int rows, int key_px, const sui_content_t *c, int page, int key_col,
                    int key_row, uint8_t *rgb_out);

// The overlay over Companion's dimmed images ("Reconnecting"): the same on every key. Draws c's first
// word, laid out in one key as on a deck of one key, over the existing key image rgb_inout
// (key_px * key_px * 3 bytes, RGB888): a dark outline in c->bg (the glyphs' alpha spread by
// sui_halo_px(size) px, Chebyshev), then the glyphs in their role's colour, both alpha-blended; every
// other pixel untouched (all of them when c has no word). Clips at the key's edges.
void sui_render_overlay_key(int key_px, const sui_content_t *c, uint8_t *rgb_inout);
// The overlay's outline width at font size px.
int sui_halo_px(int px);

// ---- Paging ----
// The page a deck shows of one screen, for the code that draws it (its 3 s timer).
typedef struct {
    int page, pages; // shown: page `page` of `pages`
    int64_t at_us;   // when it was drawn
} sui_pager_t;

// How long each page of a screen of `pages` pages shows: SUI_PAGE_US, SUI_PAGE_FAST_US past
// SUI_PAGE_MANY pages.
int64_t sui_page_us(int pages);
// The page to draw at now_us: 0 when the content is new (fresh), the next page (after the last, the
// first) once sui_page_us has passed since the shown one was drawn, else -1: nothing to draw.
// *due_us (may be NULL): with -1, when the next page is due (0: one page, no timer); else 0 (the
// caller learns it from sui_pager_shown once the page is drawn).
int sui_pager_next(const sui_pager_t *p, bool fresh, int64_t now_us, int64_t *due_us);
// Records that page `page` of `pages` was drawn at now_us; returns when the next page is due (0: one
// page, no timer).
int64_t sui_pager_shown(sui_pager_t *p, int page, int pages, int64_t now_us);

// ---- Screen builders ----
// The words' roles: the main message in status, hints and labels in label, IPv4 octets and port
// numbers in accent, values in value. Custom messages: SUI_ROLE_FG and no zones, white on black. (The
// status screens built here before were replaced by the maintenance screens, components/deckui; the
// layout's zones, keep-with-next and dividers are kept for the fitting rules, tested with them as test
// data in test/host/statusui_fixtures.c.)
// "Reconnecting"; main what it reconnects to (to; nothing when NULL). The overlay draws its first word.
void sui_screen_reconnecting(sui_content_t *c, const char *to);
// text split on '\n' into at most 4 lines of at most 40 chars (longer lines
// truncated, extra lines dropped), white on black. NULL = one empty line.
void sui_screen_message(sui_content_t *c, const char *text);

#ifdef __cplusplus
}
#endif
