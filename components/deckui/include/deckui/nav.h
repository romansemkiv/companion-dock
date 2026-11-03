#pragma once
/* The maintenance UI's navigation, one per deck: it
 * turns presses, holds, dials, touch keys and time into page changes, redraws, actions and sounds. Pure:
 * time is passed in (ms, monotonic). The caller (surfaces) builds the screen of dui_nav_page() with
 * dui_build, plans it, calls dui_nav_layout, and renders each key from dui_layout_key at dui_nav_pg()
 * with dui_nav_decorate on the copy.
 *
 * The rules:
 * - a tap fires on its release, on the same key (a release on another key does nothing);
 * - a hold button fires once, when held for its hold_ms, and shows its fill while held; let go early,
 *   it shows its hint ("Hold 2 s" / "restarts", dui_hint_text) for DUI_NAV_HINT_MS;
 * - a locked button (DUI_F_LOCKED) fires nothing: its release opens the PIN pad (out->pin, the
 *   caller runs deckui/pin.h over this screen; the key does not fire after the unlock either);
 * - Back goes to the parent: sub-screens to home (to Menu on decks whose menu is one key), QR to
 *   Hotspot, Menu to home; Menu opens Menu; the pager ("›") turns the page, after the last the first;
 * - with DUI_PAGER_TAP (the Mini's home), a tap on any key that isn't a button turns the page: a
 *   content key (a blank one too) or the status row's glyph and words; Menu and Close are buttons;
 * - dial turn = page +/- n, dial press = Back; the touch keys = page -/+ 1;
 * - DUI_NAV_IDLE_MS without a press: home (on demand: close too);
 * - home turns its pages by itself while untouched, every 3 s (2 s with more than 3 pages,
 *   sui_page_us), until a press; again after DUI_NAV_IDLE_MS without one;
 * - the screen changing under a press (a new page, a new screen, or the key's tile changed): the press
 *   does nothing until its release; a key-down within DUI_NAV_SETTLE_MS of a change the person didn't
 *   make (the home's own paging, the idle return, on demand opening) is ignored until its release;
 * - every press clicks; a finished hold confirms; a
 *   key-down that is ignored (a second key while one is held, or in the settle window) is silent;
 * - only the release of the key it saw go down counts: any other release isn't a press. */
#include <stdbool.h>
#include <stdint.h>

#include "deckui/deckui.h"
#include "deckui/screens.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DUI_NAV_IDLE_MS 60000 // without a press: home, and the on-demand screen closes
#define DUI_NAV_HINT_MS 2500  // a hold let go early shows its hint this long (time to read two lines)
#define DUI_NAV_SETTLE_MS 400 // a key-down this soon after a change the person didn't make is ignored

enum { DUI_SOUND_NONE, DUI_SOUND_CLICK, DUI_SOUND_CONFIRM, DUI_SOUND_LOCKED };

// What one event asks of the caller. There is no "the bar alone": the strip and the info bar change with
// the page (its dots) or the screen, which come with `all`, or with the snapshot (rebuild).
typedef struct {
    bool rebuild;    // the page (DUI_PAGE_*) changed: build dui_nav_page() again, plan it, dui_nav_layout
    bool all;        // redraw every key and the strip / info bar (a new screen or page)
    uint64_t keys;   // redraw these keys: bit row * cols + col (their press state changed)
    uint16_t action; // run this box action (DUI_A_*, never a GO one); 0: none
    uint8_t sound;   // DUI_SOUND_*
    bool close;      // close the on-demand maintenance screen (its Close key, or idle on demand)
    bool pin;        // a locked key was pressed and let go: open the PIN pad for pin_action
    uint16_t pin_action;
} dui_nav_out_t;

typedef struct {
    uint8_t cols, rows;
    bool menu_key;       // the deck's menu is one key: sub-screens go back to Menu
    uint8_t page;        // DUI_PAGE_*
    uint8_t pg, pages;   // the page of the layout shown, of its pages (dui_nav_layout)
    bool ondemand;       // the maintenance screen is open on demand (idle closes it)
    bool touched;        // a press since the deck last went idle: the home doesn't page by itself
    bool idle;           // the idle return was made; cleared by a press
    int64_t last_press;  // ms
    int64_t shown_at;    // the page shown since (the home's own paging)
    int64_t settle_until; // key-downs before this are ignored until their release
    struct {
        int8_t col, row; // the key held down (-1: none)
        bool ignore;     // it does nothing (until its release)
        bool fired;      // a hold that fired
        bool tap_page;   // DUI_PAGER_TAP: its release turns the page
        bool locked;     // a locked button: its release opens the PIN pad
        uint8_t kind;    // its tile when it went down
        uint16_t action, hold_ms, flags;
        int64_t at;      // when it went down
    } held;
    int64_t hint_until[DUI_KEYS_MAX]; // per key: its hold hint shows until then (0: none)
    uint16_t pulse_action;            // the key just unlocked (its action) pulses cyan ...
    int64_t pulse_from, pulse_until;  // ... from, until (DUI_UNLOCK_PULSE_MS)
} dui_nav_t;

#define DUI_UNLOCK_PULSE_MS 3000 // the unlocked key pulses this long
#define DUI_PULSE_HALF_MS 375    // ... cyan this long, then not, and so on

// A deck's navigation at home, page 0, untouched; the idle clock starts at now.
void dui_nav_init(dui_nav_t *n, const dui_deck_t *d, int64_t now);
// After each dui_plan of the current page's screen, before drawing: the pages (a page past the last
// shows the first), and a held key whose tile changed stops counting (it does nothing on release).
void dui_nav_layout(dui_nav_t *n, const dui_layout_t *L);
// A key went down or up; L is the current layout.
void dui_nav_key(dui_nav_t *n, const dui_layout_t *L, int col, int row, bool down, int64_t now, dui_nav_out_t *out);
void dui_nav_dial_turn(dui_nav_t *n, int steps, int64_t now, dui_nav_out_t *out); // +/- steps pages
void dui_nav_dial_press(dui_nav_t *n, int64_t now, dui_nav_out_t *out);           // Back
void dui_nav_touch(dui_nav_t *n, int dir, int64_t now, dui_nav_out_t *out);       // -1 left, +1 right
// Time passes (call it every 50 ms or so): holds, hints, the home's own paging, the idle return.
void dui_nav_tick(dui_nav_t *n, int64_t now, dui_nav_out_t *out);
// The snapshot changed: rebuild the screen (the caller sends only the keys whose content changed).
void dui_nav_box_changed(dui_nav_t *n, dui_nav_out_t *out);
// The maintenance screen on demand opened (home, untouched, the idle clock from now) or closed. Either way a
// key held across it is dropped (dui_nav_cancel).
void dui_nav_ondemand(dui_nav_t *n, bool on, int64_t now, dui_nav_out_t *out);
// The deck changed hands: Companion took it or gave it back, or it was replugged (or re-init it). The key
// held is dropped, as its release may never come here: no hold fires, the next press works and the idle
// clock runs again. The hints go too; redraw the deck when it shows the maintenance UI again.
void dui_nav_cancel(dui_nav_t *n);

// a tap at x on the touch strip (+, + XL): on the network or hotspot icon, its screen; else nothing.
void dui_nav_bar_tap(dui_nav_t *n, const dui_status_t *st, int w, int h, int x, int64_t now, dui_nav_out_t *out);
// A press elsewhere for this deck (its PIN pad): the idle clock starts again.
void dui_nav_activity(dui_nav_t *n, int64_t now);

int dui_nav_page(const dui_nav_t *n); // DUI_PAGE_*: the screen to build
int dui_nav_pg(const dui_nav_t *n);   // the layout's page to draw
// Key (col, row)'s press state on t, a copy from dui_layout_key: DUI_F_DOWN, the hold's fill, DUI_F_HINT, and the
// unlocked key's pulse (a button's phase: 1 glowing cyan, 0 not).
void dui_nav_decorate(const dui_nav_t *n, int col, int row, int64_t now, dui_tile_t *t);
// after the right PIN, the key with action pulses cyan for DUI_UNLOCK_PULSE_MS (where it is shown).
void dui_nav_pulse(dui_nav_t *n, uint16_t action, int64_t now);
// The notice for that: "Unlocked · hold Restart" (a hold button), "Unlocked · press Sync" (a tap), "Unlocked" when
// layout L has no button with action. Returns its length.
int dui_unlock_note(const dui_layout_t *L, uint16_t action, char *out, int n);

#ifdef __cplusplus
}
#endif
