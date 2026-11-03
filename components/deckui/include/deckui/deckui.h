#pragma once
/* The deck maintenance UI's engine: the tiles, screens as
 * groups of tiles, the per-model plan and content flow, the row alignment, and the drawing of a key,
 * the touch strip and the Neo's info bar into RGB888. Pure: no ESP-IDF headers. What each screen
 * holds at run time, the navigation, the input and the wiring into surfaces are the caller's.
 *
 * Sizes are given for a 96 px key and scale with the key. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deckmodel/model.h"
#include "statusui/statusui.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- The palette: statusui's (SUI_RGB_*) plus the maintenance UI's own, 0xRRGGBB, here only ----
#define DUI_RGB_BTN 0x161C27          // a button's fill
#define DUI_RGB_BTN_DOWN 0x26324A     // ... while pressed (brighter)
#define DUI_RGB_BTN_EDGE 0x64758F     // ... its outline (3.65:1 against the fill: a button reads as one)
#define DUI_RGB_BTN_SEL 0x0C3340      // a selected button's fill (its outline and icon: SUI_RGB_ACCENT)
#define DUI_RGB_DANGER 0xFF5C5C       // a danger button's outline, icon and HOLD tag
#define DUI_RGB_HOLD 0x11505F         // a hold button filling from the bottom
#define DUI_RGB_HOLD_DANGER 0x5A1A1A  // ... a danger one
#define DUI_RGB_TINT_NEUTRAL 0x1B1F26 // the status of a sub-screen: Back, the title
#define DUI_RGB_DIM 0x5F6670          // an icon tile not in use; a button that does nothing now (DUI_F_DIM) or is locked
#define DUI_RGB_LOCK SUI_RGB_LABEL    // a locked button's padlock (grey): the key drawn disabled, the PIN pad on a press
#define DUI_RGB_DOT_OFF 0x4A515C      // a page dot not shown
#define DUI_RGB_QR_DARK 0x000000
#define DUI_RGB_QR_LIGHT 0xFFFFFF

// A text's role gives its colour: values white, labels grey, IPv4 octets and ports cyan, and the
// state colours for status words.
enum {
    DUI_ROLE_VALUE,  // SUI_RGB_VALUE: an SSID, a password, a version, a name
    DUI_ROLE_LABEL,  // SUI_RGB_LABEL: hints and labels
    DUI_ROLE_ACCENT, // SUI_RGB_ACCENT: IPv4 octets and port numbers
    DUI_ROLE_BAD,    // SUI_RGB_BAD
    DUI_ROLE_WAIT,   // SUI_RGB_WAIT
    DUI_ROLE_OK,     // SUI_RGB_OK
    DUI_ROLES
};

// A screen's state, as the status light shows it (= SUI_STATE_*).
enum { DUI_STATE_NONE, DUI_STATE_BAD, DUI_STATE_WAIT, DUI_STATE_OK };

// A key's background.
enum {
    DUI_TINT_BLACK,
    DUI_TINT_MAIN,    // SUI_RGB_TINT_MAIN: the main zone (navy)
    DUI_TINT_SVC,     // SUI_RGB_TINT_SERVICE: the service zone, the hotspot (purple)
    DUI_TINT_BAD,     // the status row of each state
    DUI_TINT_WAIT,
    DUI_TINT_OK,
    DUI_TINT_NEUTRAL, // a sub-screen's status row
    DUI_TINTS
};

// A group's zone: the background of its tiles.
enum { DUI_ZONE_MAIN, DUI_ZONE_SVC, DUI_ZONE_NONE };

// The icons in a 24-unit box, drawn anti-aliased at any size: Phosphor's Regular glyphs (phosphoricons.com, MIT),
// but SAT (Lucide's satellite, as strokes) and the box's nine-dot LOGO.
enum {
    DUI_ICON_NONE,
    DUI_ICON_WIFI,    // its bars by rssi (wifi-high / -medium / -low, the unlit bars dimmed)
    DUI_ICON_NONET,   // wifi-slash, its slash red
    DUI_ICON_ETH,
    DUI_ICON_NET,     // a globe: the Network menu
    DUI_ICON_DECK,    // a deck's keys (no longer used for Companion nor Satellite)
    DUI_ICON_DOCK,
    DUI_ICON_HOTSPOT,
    DUI_ICON_INFO,
    DUI_ICON_BACK,
    DUI_ICON_FWD,
    DUI_ICON_MENU,
    DUI_ICON_RETRY,
    DUI_ICON_RESTART,
    DUI_ICON_SYNC,
    DUI_ICON_POWER,   // a toggle: the knob right with DUI_F_ON
    DUI_ICON_QR,
    DUI_ICON_LIST,
    DUI_ICON_SUN_LOW,
    DUI_ICON_SUN_HIGH,
    DUI_ICON_SOUND,
    DUI_ICON_MUTE,
    DUI_ICON_LOCK,
    DUI_ICON_CLOSE,   // the on-demand maintenance screen's Close key
    DUI_ICON_LOGO,    // the box's nine-dot logo (the web page's i-keys); no deck screen uses it now
    DUI_ICON_DEL,     // the PIN pad's delete (⌫)
    DUI_ICON_SAT,     // a satellite: Satellite mode (Lucide's satellite)
    DUI_ICON_FLASH,   // a flash: the status light's flashes
    DUI_ICON_EDIT,    // a pencil: Edit the static address
    DUI_ICON_COMPANION, // Companion: Phosphor's webhooks-logo
    DUI_ICON_CIRCUITRY, // Phosphor's Circuitry (MIT): a board, its traces and two pads: the Box menu button
    DUI_ICON_SEARCH,  // a magnifying glass: Auto find
    DUI_ICON_WIFI_OFF,    // Wi-Fi off by choice: wifi-slash all in the ink given (grey in the status row)
    DUI_ICON_HOTSPOT_OFF, // the hotspot off: broadcast crossed with wifi-slash's slash, cut clear around it
    DUI_ICONS
};

// ---- Tiles: one per key ----
enum {
    DUI_TILE_EMPTY,  // black
    DUI_TILE_FILL,   // its tint
    DUI_TILE_WORD,   // text: one value or label, in its role's colour
    DUI_TILE_CAP,    // a small grey upper-case caption at the top, the value under it
    DUI_TILE_STEP,   // a cyan circled number, a verb, an optional small sub-line (caption)
    DUI_TILE_BUTTON, // pressable: rounded outline, icon at the top, label at the bottom
    DUI_TILE_ICON,   // a big icon and a small label (Ethernet, Wi-Fi Main/Backup); DUI_F_DIM
    DUI_TILE_GLYPH,  // the state: red X, amber pulsing dots, green tick on a filled circle
    DUI_TILE_IND,    // the network (icon WIFI + rssi, ETH, NONET) or the hotspot indicator
    DUI_TILE_PAGER,  // ">" with page dots under it
    DUI_TILE_QR,     // the payload (text) as a QR code, black on white
    DUI_TILE_SPIN,   // searching / reconnecting
    DUI_TILE_TOAST,  // a short green notice in the status row
    DUI_TILE_KEY,    // the PIN pad's key: its text big in the middle (a digit, OK) or its icon (⌫, ✕);
                     // DUI_F_DOTS: the dots entered under it (num: how many; the Mini's roller)
    DUI_TILE_DOTS,   // the PIN entered as dots, 4 places a key: num entered, cur the first place on
                     // this key; caption: a small word over them
    DUI_TILE_OCTETS, // an address being edited: num octets, text them joined by '.' (a trailing '.' when
                     // more follow on the next key; an empty one "", drawn "_"); cur the one outlined in cyan
                     // (0xFF: none); caption: a small word over them
};

#define DUI_F_OPTIONAL 0x0001 // dropped when its group doesn't fit its row (a step)
#define DUI_F_SELECTED 0x0002 // a button: cyan outline and icon
#define DUI_F_DANGER 0x0004   // a button: red outline and icon
#define DUI_F_DOWN 0x0008     // a button or the pager: pressed (brighter fill)
#define DUI_F_HINT 0x0010     // a hold button released early: its instruction in amber (dui_hint_text)
#define DUI_F_LOCKED 0x0020   // a button that does nothing under the settings password: a lock
#define DUI_F_DIM 0x0040      // an icon tile not in use; a button that does nothing now ("Set on web page")
#define DUI_F_NAV 0x0080      // Back or Close: its icon alone, or small with the title under it
#define DUI_F_OCTET 0x0100    // an IPv4 octet: an address stays on one row when a row holds 4, else
                              // in balanced rows, never a lone octet
#define DUI_F_PORT 0x0200     // a port right after an address: on its row, else the next row of the page
#define DUI_F_PIECES 0x0400   // a value that goes over keys when one key can't hold it: split by dui_plan
#define DUI_F_ON 0x0800       // the power toggle icon: on
#define DUI_F_DOTS 0x1000     // a PIN pad key: the dots entered under its text (num of them)
#define DUI_F_IP 0x2000       // small decks: an IPv4 address on one key, or two of its octets ("192.168." |
                              // "8.225"), on one line
#define DUI_F_TAG 0x4000      // a button (not a choice): its caption is a small tag at the top right (Make
                              // static's "saved" / "lease"); the icon, the label, the hold hint and the lock as ever
#define DUI_F_WHOLE 0x8000    // a value kept on one key on every deck (a Wi-Fi name's word): over keys only
                              // when it can't read at DUI_TEXT_MIN_PX on 2 lines; like every value, nothing is
                              // ever added to it (no '-')

// the smallest real sizes on every model (px); content that doesn't fit at them moves on, never shrinks.
#define DUI_TEXT_MIN_PX 16    // values and words
#define DUI_CAP_MIN_PX 12     // captions, the HOLD tag, a step's or a choice's sub-line
#define DUI_LABEL_MIN_PX 14   // button and icon labels, toasts, hints (13 asked: the font has 12, then 14)
#define DUI_SMALL_KEYS 15     // a deck with this many keys or fewer is small: no word, value or address over keys
// The size a button's label is drawn at on a key of key_px (one line, else two), as dui_render_key draws it.
int dui_label_px(const char *s, int len, int key_px);

// The action ids the engine gives the keys it adds; the caller's own ids are below DUI_ACT_BACK.
#define DUI_ACT_NONE 0
#define DUI_ACT_BACK 0xFFF0
#define DUI_ACT_MENU 0xFFF1
#define DUI_ACT_PAGER 0xFFF2
#define DUI_ACT_CLOSE 0xFFF3
#define DUI_ACT_NET 0xFFF4     // the status row's network indicator: the Network screen
#define DUI_ACT_HOTSPOT 0xFFF5 // ... the hotspot indicator: the Hotspot screen

// A text tile's size and lines in its row (dui_align_row), in key px.
typedef struct {
    uint8_t px;     // one line: the row's size of its role; two lines: their size
    uint8_t nl;     // 1 or 2 (0: not aligned yet)
    uint8_t cut;    // two lines: chars on the first
    uint8_t hyph;   // two lines: the first ends in an added '-'
    uint8_t base;   // the row's baseline, px from the key's top (one line)
    uint8_t rowcap; // the row has a caption or a step: the value area is under the caption line
    uint8_t cap_px; // the row's caption size (all its captions on one line, one size)
} dui_fit_t;

typedef struct {
    uint8_t kind;    // DUI_TILE_*
    uint8_t role;    // DUI_ROLE_* of its text (word, caption + value)
    uint8_t tint;    // DUI_TINT_*: dui_plan sets it from the group's zone
    uint8_t icon;    // DUI_ICON_*: button, icon tile, indicator
    uint8_t state;   // glyph: DUI_STATE_*; button: its state dot (DUI_STATE_NONE: none)
    uint8_t num;     // step: its number; pager: the page count
    uint8_t cur;     // pager: the page shown
    uint8_t phase;   // glyph dots, spinner: the animation, 0..255 = one cycle (DUI_PULSE_MS, DUI_SPIN_MS)
    int8_t rssi;     // Wi-Fi icons: dBm; 0: unknown (all bars)
    uint8_t hold;    // a hold button being held: its progress, 0..255 = 0..1
    uint8_t len;     // chars of text; 0: up to its NUL
    uint8_t map;     // a fixed key map's place (DUI_MAP: the Network screen); 0: the flow places it
    uint8_t run;     // the first tile of a run that stays together (a value's pieces, an address and its
                     // port): the run's tiles; 0 or 1: none
    uint16_t flags;  // DUI_F_*
    uint16_t action; // a button or the pager: an opaque id for the caller (DUI_ACT_* for the engine's)
    uint16_t hold_ms; // a button: 0 a tap, else the hold it needs (the HOLD tag)
    const char *text;    // word, value, step verb, button or icon label, QR payload, toast
    const char *caption; // the caption; a step's sub-line; a choice button's cyan sub-line (an IP)
    const char *hint;    // a hold button's hint, its consequence ("restarts", "cuts users": dui_hint_text)
    dui_fit_t fit;       // set by the row alignment
} dui_tile_t;
// A hold button's release-early hint: how long, then what it does: "Hold 2 s · restarts" (its hold_ms,
// then its hint, a consequence: "restarts", "Wi-Fi blips", "cuts users"), drawn as "Hold 2 s" / "restarts"; "Hold 2 s"
// with no hint. Returns its length.
int dui_hint_text(const dui_tile_t *t, char *out, int n);
// The size the hint is drawn at on a key of key_px (two lines when larger than one), as dui_render_key draws it.
int dui_hint_px(const dui_tile_t *t, int key_px);

#define DUI_PULSE_MS 690 // the glyph's dots pulse once this often
#define DUI_SPIN_MS 1600 // the spinner turns once this often
#define DUI_ANIM_MS 200  // an animated tile changes at most this often (5 fps)

// The animation phase of a cycle of period_ms at now_ms, in DUI_ANIM_MS steps (so a key redrawn only
// when its fingerprint changes moves at 5 fps or less).
uint8_t dui_phase(int64_t now_ms, int period_ms);
// The strip and the info bar are drawn and encoded whole each time their pulse moves: at most 2 fps
// (DUI_BAR_ANIM_MS steps), the glyph's dots pulsing once every DUI_BAR_PULSE_MS (4 steps).
#define DUI_BAR_ANIM_MS 500
#define DUI_BAR_PULSE_MS 2000
uint8_t dui_bar_phase(int64_t now_ms);
// t moves: the spinner, and the glyph while waiting (its dots pulse). Its phase is then dui_phase of
// DUI_SPIN_MS / DUI_PULSE_MS.
bool dui_tile_animated(const dui_tile_t *t);
// A fingerprint of everything that shows of t (strings by content, not by pointer; the press state, the
// hold fill, the hint, the lock and the phase included), never 0: draw a key again only when it
// changes.
uint32_t dui_tile_hash(const dui_tile_t *t);

// ---- Screens: groups of tiles, and the status ----
#define DUI_STATUS_WORDS 4
enum { DUI_NET_NONE, DUI_NET_WIFI, DUI_NET_ETH };
// the Wi-Fi sign: crossed with a red slash (on and not joined: a problem), its bars (joined), crossed in grey
// (off by choice, or no network set up: nothing wrong).
enum { DUI_SIGN_DOWN, DUI_SIGN_UP, DUI_SIGN_OFF };

typedef struct {
    uint8_t state;                              // DUI_STATE_*: the glyph, the words' colour, the tint
    const char *word[DUI_STATUS_WORDS];         // the status words on the keys ("Waiting", "for", ...)
    const char *short_word[DUI_STATUS_WORDS];   // the short form, when a deck must ("Waiting"); optional
    const char *text;                           // the strip's and info bar's text; NULL: the words joined
    const char *title[DUI_STATUS_WORDS];        // a sub-screen's title words
    uint8_t net;                                // DUI_NET_*: the route (its sign kept the longest)
    int8_t rssi;                                // Wi-Fi dBm (0: unknown)
    bool hotspot;                               // the hotspot is on (its sign always there, crossed when off)
    uint8_t wifi;                               // the Wi-Fi sign, DUI_SIGN_* (net WIFI: UP unless OFF)
    bool eth_link;                              // the Ethernet cable is linked: its sign (net ETH: too)
    bool close;                                 // home opened on demand: a Close key at the top-left
    uint8_t phase;                              // the glyph's animation (DUI_PULSE_MS)
    const char *toast;                          // a short notice ("Restarting"); NULL: none
    uint8_t toast_state;                        // its outcome: DUI_STATE_OK done, WAIT under way, BAD
                                                // refused (its two-word reason the text); it takes the status over
} dui_status_t;

// a refused deck action's two-word reason from the RPC's error (its code and message): "Upload running",
// "Sync running", "Not valid", "Wi-Fi off", "No backup", "PIN needed"... ("Not done" when nothing matches).
const char *dui_refusal(int code, const char *msg);

// what a tap at x on the touch strip hits (dui_render_bar's layout): DUI_ACT_NET on the network icon,
// DUI_ACT_HOTSPOT on the hotspot icon, 0 elsewhere.
int dui_bar_hit(const dui_status_t *st, int w, int h, int x);
// dui_tile_hash for the touch strip or the info bar (dui_render_bar's arguments).
uint32_t dui_bar_hash(const dui_status_t *st, bool sub, int page, int pages);

enum { DUI_G_PACK = 1, DUI_G_ANCHOR = 2 }; // goes on in the row when it fits; at the bottom of the last page

// the Network screen's fixed key map. A tile's map is its row (Ethernet, Main, Backup, or the screen's
// own: the box's name) and its column's role; dui_plan puts each where the model's map says, the same keys in every
// state, instead of the flow (a model with no map: the flow, by the groups).
enum { DUI_MAP_ETH = 1, DUI_MAP_MAIN, DUI_MAP_BACKUP, DUI_MAP_TOP };
enum {
    DUI_MC_ICON = 1, // the network's icon tile
    DUI_MC_NAME,     // its name: the Wi-Fi name, Ethernet's ADDRESS (DHCP / Static)
    DUI_MC_STATE,    // Up, No cable, In use, Standby, Joining, Not found, Radio down, Off, Not set
    DUI_MC_ACTION,   // Use DHCP / Make static / Set on web page, Use
    DUI_MC_AUX,      // Ethernet's Edit; a Wi-Fi network's SIGNAL while in use
    DUI_MC_IP,       // its address (the four octets; one key or two, "192.168." | "8.225")
    DUI_MC_TOGGLE,   // the Wi-Fi on / off key (on the Main row)
    DUI_MC_LOCAL,    // the box's name and ".local" (DUI_MAP_TOP)
};
#define DUI_MAP(row, col) ((uint8_t)((row) << 4 | (col)))

typedef struct {
    uint8_t first, n; // its tiles: tile[first .. first + n)
    uint8_t zone;     // DUI_ZONE_*
    uint8_t flags;    // DUI_G_*
} dui_group_t;

#define DUI_GROUPS_MAX 8
#define DUI_SCREEN_TILES 40
#define DUI_MENU_TILES 4
#define DUI_POOL 640

typedef struct {
    dui_status_t status;
    bool sub;  // a sub-screen: Back at the top-left, the title on the neutral tint
    bool menu; // the menu (home): a row, one key or in the status row, per model
    bool full; // the whole grid on every deck (the Menu screen)
    bool keymap; // its tiles carry DUI_MAP places (the Network screen)
    uint8_t ngroups, ntiles, nmenu;
    dui_group_t group[DUI_GROUPS_MAX];
    dui_tile_t tile[DUI_SCREEN_TILES];
    dui_tile_t menu_tile[DUI_MENU_TILES]; // the menu row's buttons (decks with a menu row)
    bool overflow;   // something didn't fit (a tile, a group, a menu button, a string) and was dropped
    bool sinking;    // a group was refused: the tiles added after it go to the sink
    dui_tile_t sink; // where a dropped tile is written; never laid out
    uint16_t pool_used;
    char pool[DUI_POOL]; // the strings the builders copy
} dui_screen_t;

// Builders. Every string is copied into s->pool (NULL: ""; a full pool: "" and s->overflow), so the
// caller's buffers may go away. A tile past DUI_SCREEN_TILES, a group past DUI_GROUPS_MAX with every
// tile added after it, and a menu button past DUI_MENU_TILES are dropped: the builder returns s->sink
// and sets s->overflow.
void dui_screen_init(dui_screen_t *s);
const char *dui_str(dui_screen_t *s, const char *text); // a copy in the pool
// The status words, space-separated: words for the keys, short_words (may be NULL) for small decks,
// text (may be NULL: the words joined) for the strip and info bar.
void dui_status_words(dui_screen_t *s, int state, const char *words, const char *short_words, const char *text);
void dui_title(dui_screen_t *s, const char *words); // a sub-screen: sets s->sub and the title words
// Starts a group: the tiles added next are its, until the next dui_group.
void dui_group(dui_screen_t *s, int zone, int flags);
dui_tile_t *dui_add(dui_screen_t *s, int kind); // a zeroed tile of the current group
dui_tile_t *dui_word(dui_screen_t *s, const char *text, int role);
dui_tile_t *dui_cap(dui_screen_t *s, const char *caption, const char *text, int role);
// A lead label for the tiles right after it ("Connected Companion" before the address): a grey word on its
// own key (kept in the tile's hint: alone_caption). Where dui_plan can't put the address after it (dui_ipv4) on the
// label's page, it drops the label and the address's first key takes alone_caption as its caption ("Companion IP"),
// so the run stays whole. Before anything else the label always stays.
dui_tile_t *dui_lead(dui_screen_t *s, const char *label, const char *alone_caption);
// A value (an SSID, a password, a name) that goes over keys when one can't hold it, the caption
// (may be NULL) on its first key.
dui_tile_t *dui_value(dui_screen_t *s, const char *caption, const char *text, int role);
// s[0..n) split into words at whitespace only. Up to max words, each start[i] / len[i];
// the last takes the rest when there are more.
int dui_split_words(const char *s, int n, int *start, int *len, int max);
// a Wi-Fi name, one word a key on every deck (the caption, may be NULL, on the first), kept together on a
// row when they fit. A word is never split over keys: inside its key a long one wraps to 2 lines at its natural
// breaks; only one that can't read at DUI_TEXT_MIN_PX on 2 lines goes over keys (the last resort).
void dui_words(dui_screen_t *s, const char *caption, const char *text, int role);
// An IPv4 address as four cyan octets, the caption (may be NULL) on the first. Not an IPv4
// address: one accent value.
void dui_ipv4(dui_screen_t *s, const char *caption, const char *ip);
// A port after an address (caption + number, cyan): on the octets' row when it fits, else on the next
// row of the same page.
dui_tile_t *dui_port(dui_screen_t *s, const char *caption, const char *port);
dui_tile_t *dui_step(dui_screen_t *s, int n, const char *verb, const char *sub); // optional
dui_tile_t *dui_button(dui_screen_t *s, int icon, const char *label, uint16_t action);
// A choice: a button with a name and a small cyan sub-line under it (a Companion and its IP).
dui_tile_t *dui_choice(dui_screen_t *s, const char *label, const char *sub, uint16_t action);
dui_tile_t *dui_icon_tile(dui_screen_t *s, int icon, const char *label, bool dim);
dui_tile_t *dui_qr(dui_screen_t *s, const char *payload);
dui_tile_t *dui_spin(dui_screen_t *s);
// A menu button (the menu row, and the Menu screen's group).
dui_tile_t *dui_menu_button(dui_screen_t *s, int icon, const char *label, uint16_t action, int dot);

// ---- The per-model plan ----
enum { DUI_BAR_NONE, DUI_BAR_STRIP, DUI_BAR_INFO };
typedef struct {
    uint8_t cols, rows;
    uint16_t key_px;
    uint8_t bar;          // DUI_BAR_*: the + and + XL touch strip, the Neo's info bar
    uint16_t bar_w, bar_h;
    uint8_t dials;        // encoders: turn = page, press = Back
    bool touch_keys;      // the Neo's two touch keys: they turn the pages
} dui_deck_t;

// The deck of model m (false: no screen: the Pedal).
bool dui_deck_of(const deck_model_t *m, dui_deck_t *d);

enum { DUI_STATUS_KEYS, DUI_STATUS_STRIP, DUI_STATUS_INFO };       // where the status goes
enum { DUI_MENU_NONE, DUI_MENU_ROW, DUI_MENU_KEY, DUI_MENU_STATUS }; // the bottom row; one key; in the status row
enum { DUI_PAGER_MENUROW, DUI_PAGER_HW, DUI_PAGER_KEY, DUI_PAGER_TAP }; // ">" in the menu row; dials or touch
                                                                         // keys; a ">" key; tap a content key

#define DUI_KEYS_MAX 36   // 9 x 4: the + XL
#define DUI_PAGES_MAX 8
#define DUI_LAYOUT_TILES 96
#define DUI_NO_TILE 0xFF

typedef struct {
    dui_deck_t deck;
    uint8_t status_at;    // DUI_STATUS_*
    uint8_t menu_style;   // DUI_MENU_*
    uint8_t pager;        // DUI_PAGER_*: how pages turn
    bool full;            // the screen takes the whole grid (no status row)
    uint8_t status_rows;  // key rows the status takes at the top (0 or 1)
    uint8_t content_rows; // key rows of content on a page, under the status
    uint8_t pages;        // >= 1
    bool overflow;        // content was dropped: past DUI_LAYOUT_TILES or DUI_PAGES_MAX (or s->overflow)
    int8_t menu_col, menu_row;   // the Menu key; -1: none
    int8_t back_col, back_row;   // the Back (or Close) key; -1: none
    int8_t pager_col, pager_row; // the ">" key (when there are pages and it is a key); -1: none
    uint8_t ntiles;
    uint8_t key[DUI_PAGES_MAX][DUI_KEYS_MAX]; // per page, row by row: an index into tile[], DUI_NO_TILE: blank
    dui_tile_t tile[DUI_LAYOUT_TILES];        // the screen's tiles (values split), the status row, the menu
                                              // row, the reserved keys; each aligned in its row
    char title[48];                           // the Back key's title (full-grid sub-screens); the status row's
                                              // title in one key
} dui_layout_t;

// The home screen's menu on deck d: DUI_MENU_ROW, DUI_MENU_KEY or DUI_MENU_STATUS (a deck whose menu is
// one key opens a Menu screen, and its sub-screens go back to it).
int dui_home_menu(const dui_deck_t *d);
// Lays screen s out on deck d: the status row, the menu, the reserved keys (Back, Close, Menu, ">"),
// the content flowed into pages, every row aligned. L refers to s's strings: keep s.
void dui_plan(const dui_deck_t *d, const dui_screen_t *s, dui_layout_t *L);
// The tile on key (col, row) of page `page` (modulo the pages), page-dependent parts set (the pager's
// page); false and an EMPTY tile for a blank key.
bool dui_layout_key(const dui_layout_t *L, int page, int col, int row, dui_tile_t *out);

// ---- Row alignment ----
// The text tiles (word, caption + value, step) of one row of keys: when any has a caption or is a
// step, every value sits on one common baseline under the caption line, else all are centred on one
// baseline; the texts of one role take one size (the smallest that fits all of them); a word one key
// can't hold on one line takes two (sui_fit's split), centred in the value area. Other tiles
// are left alone. NULL entries are skipped.
void dui_align_row(dui_tile_t *const *row, int n, int key_px);
// the status row's text tiles (its words in every state, a toast's, a sub-screen's title, a pad's
// status words): one size per model, whatever the state (dui__status_px); a text too long for one line at it
// wraps inside its key at that size ("Recon-" / "necting"), smaller only when even two lines can't hold it.
void dui_align_status(dui_tile_t *const *row, int n, int key_px);

// ---- Drawing, RGB888 ----
// Key tile t into rgb (key_px * key_px * 3 bytes), as sui_render_key does a word.
void dui_render_key(const dui_tile_t *t, int key_px, uint8_t *rgb);
// The status on the touch strip (800 x 100, 1200 x 100) or the Neo's info bar (248 x 58) into rgb
// (w * h * 3 bytes): the state's tint, the glyph, the text, the toast, the network and hotspot icons, the
// page dots (the strip). A sub-screen: its title on the neutral tint.
void dui_render_bar(const dui_status_t *st, bool sub, int page, int pages, int w, int h, uint8_t *rgb);

// An RGB888 image to draw on.
typedef struct {
    uint8_t *rgb;
    int w, h;
} dui_canvas_t;
// Icon `icon` (DUI_ICON_*) centred at (cx, cy) px, size px across (its 24-unit box), in colour rgb
// (0xRRGGBB), alpha-blended; rssi for the Wi-Fi bars, flags DUI_F_ON for the power toggle.
void dui_icon(const dui_canvas_t *c, int icon, float cx, float cy, float size, uint32_t rgb, int8_t rssi,
              uint16_t flags);

// ---- QR codes (Nayuki qrcodegen, MIT: components/deckui/qrcodegen/LICENSE) ----
#define DUI_QR_VERSION_MAX 4 // one key per code
#define DUI_QR_BUF 138       // qrcodegen_BUFFER_LEN_FOR_VERSION(4)
// payload as a QR code of the smallest version up to DUI_QR_VERSION_MAX (the largest modules), at that
// version ECC M when it fits, else L, into qr (DUI_QR_BUF bytes). False: too long.
bool dui_qr_encode(const char *payload, uint8_t qr[DUI_QR_BUF]);
int dui_qr_size(const uint8_t *qr);                 // modules across (21 + 4 per version above 1)
bool dui_qr_dark(const uint8_t *qr, int x, int y); // module (x, y)
// The module size on a key: the code and a 1-module quiet zone, in whole px.
int dui_qr_module_px(int key_px, int size);
// The Wi-Fi join string for a phone camera: WIFI:T:WPA;S:<ssid>;P:<pass>;; (T:nopass when pass is NULL
// or ""), with \ ; , : " escaped. Returns the length it needs (as snprintf).
int dui_wifi_join(char *out, size_t n, const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
