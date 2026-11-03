#pragma once
/* The deck's PIN pad and its unlock: pressing a key the settings PIN locks opens the pad; the
 * right PIN unlocks the deck's locked actions for this maintenance session (dui_unlock_t), the screen it
 * came from comes back, and the key pressed does not fire (it is pressed or held again as usual).
 * Pure: time is passed in (ms, monotonic); the check itself is the caller's (auth, on a worker), its
 * answer comes back by dui_pin_result.
 *
 * - The digits show as dots only, never as digits (the keys' labels are the pad's, not what was typed).
 * - Keys: the digits, delete (⌫), OK, Cancel (✕, back to the screen it came from). OK needs DUI_PIN_MIN
 *   digits; past DUI_PIN_MAX a digit is ignored.
 * - Wrong: the status red with "Wrong PIN", the locked sound, the digits cleared. Locked out: "Locked ·
 *   wait N s", counting down, the keys dimmed. Busy (the check couldn't start, or got no answer in
 *   DUI_PIN_CHECK_MS): "Busy, try again" in amber, the digits kept when they can be. Right: the confirm sound, closed (the caller toasts
 *   "Unlocked"). No PIN set (none, or a text password from an older firmware): "Set a PIN on the web page"; any
 *   key closes it.
 * - It cancels itself after DUI_PIN_IDLE_MS without a press (never while a check runs).
 * - The layout per deck (dui_pin_style):
 *     PHONE (8 x 4: Module 32, XL)   status row ✕, the words, the dots over 2 keys; 1-9 as a phone pad
 *                                    with ⌫ / 0 / OK in the column beside it, centred
 *     PHONE_BAR (9 x 4 + strip: + XL) the same pad, ✕ top-left; the words and the dots on the strip
 *     ROWS (5 x 3: MK.2, 15)          status row ✕ ⌫ dots dots OK; then 1-5 and 6-0
 *     PAGES (4 x 2 + strip / info bar: +, Neo) the words and the dots on the bar; the digits on two pages
 *                                    (1-5, 6-0) with ⌫, OK and a page key; dials or touch keys turn
 *                                    the page too; ⌫ held (or a dial pushed) cancels, and ⌫ says so
 *                                    ("Hold to cancel")
 *     ROLLER (3 x 2: Mini, Module 6)  − / the digit (the title over it, the dots under it) / +, then ⌫, Next
 *                                    digit, OK; ⌫ held cancels, and says so. A roller over a paged pad: 10 digits + ⌫ + OK + a page key
 *                                    need three pages of 6 keys, and a digit's key would move between
 *                                    pages; the roller keeps every key in one place, and the digit to
 *                                    add is always in view. */
#include <stdbool.h>
#include <stdint.h>

#include "deckui/deckui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DUI_PIN_MIN 4
#define DUI_PIN_MAX 8
#define DUI_PIN_IDLE_MS 30000      // without a press the pad cancels itself
#define DUI_PIN_WRONG_MS 1500      // "Wrong PIN" shows this long
#define DUI_PIN_CANCEL_HOLD_MS 1000 // ⌫ held this long cancels (PAGES, ROLLER: no ✕ key)
#define DUI_UNLOCK_MS (5 * 60 * 1000) // an unlock lasts this long at most
#define DUI_PIN_CHECK_MS 15000     // a check with no answer this long ends as busy

enum { DUI_PIN_PHONE, DUI_PIN_PHONE_BAR, DUI_PIN_ROWS, DUI_PIN_PAGES, DUI_PIN_ROLLER };
// The pad's layout for deck d.
int dui_pin_style(const dui_deck_t *d);

// The pad's key actions (their tiles' action): the digits DUI_PIN_A_DIGIT + 0..9, then the others.
enum {
    DUI_PIN_A_DIGIT = 0x100,
    DUI_PIN_A_DEL = 0x10A,
    DUI_PIN_A_OK,
    DUI_PIN_A_CANCEL,
    DUI_PIN_A_PAGE,
    DUI_PIN_A_DOWN, // the roller's digit - 1
    DUI_PIN_A_UP,   // ... + 1
    DUI_PIN_A_ADD,  // add the roller's digit
};

enum { DUI_PIN_ENTRY, DUI_PIN_CHECKING, DUI_PIN_WRONG, DUI_PIN_LOCKED, DUI_PIN_NOPIN, DUI_PIN_BUSY };

typedef struct {
    bool open;
    uint8_t style;           // DUI_PIN_PHONE ...
    uint8_t phase;           // DUI_PIN_ENTRY ...
    uint8_t n;               // digits entered
    char d[DUI_PIN_MAX + 1]; // the digits (wiped when taken and on close)
    uint8_t roller;          // ROLLER: the digit shown
    uint8_t pg;              // PAGES: the page of digits shown
    int64_t last_press;
    int64_t until;           // WRONG: the flash ends; LOCKED: the wait ends
    int shown_s;             // LOCKED: the seconds drawn (a redraw when they change)
    struct {
        int8_t col, row;     // the key held down (-1: none)
        uint16_t action;
        int64_t at;
        bool fired;          // ⌫ held: the cancel fired
    } held;
    uint16_t from_action;    // the locked key's action that opened it (it does not fire after the unlock)
} dui_pin_t;

// A check's answer (dui_pin_result).
enum { DUI_PIN_R_OK, DUI_PIN_R_WRONG, DUI_PIN_R_LOCKED, DUI_PIN_R_NONE, DUI_PIN_R_BUSY };

typedef struct {
    bool redraw;   // draw the pad again: its keys and the strip / info bar
    uint8_t sound; // DUI_SOUND_* (deckui/nav.h)
    bool check;    // check the digits now: dui_pin_take them (they are wiped there); the pad shows "Checking"
    bool closed;   // the pad closed (cancelled, or unlocked): the screen it came from comes back
    bool unlocked; // ... by the right PIN: unlock the deck (dui_unlock_start), toast "Unlocked"
    int64_t last_press; // with closed: the pad's last press (an idle close is not a press of the screen's)
} dui_pin_out_t;

// Opens the pad over the screen shown, for the locked key from_action. have_pin: a PIN is set (else it
// says "Set a PIN on the web page"); wait_s: the deck is locked out that long (0: not).
void dui_pin_open(dui_pin_t *p, const dui_deck_t *d, uint16_t from_action, bool have_pin, int wait_s, int64_t now,
                  dui_pin_out_t *o);
// Closes it and wipes the digits.
void dui_pin_close(dui_pin_t *p);
// The pad's keys on deck d, as a layout for dui_layout_key (page: p->pg).
void dui_pin_layout(const dui_deck_t *d, const dui_pin_t *p, int64_t now, dui_layout_t *L);
// A key went down or up (L: dui_pin_layout's).
void dui_pin_key(dui_pin_t *p, const dui_layout_t *L, int col, int row, bool down, int64_t now, dui_pin_out_t *o);
void dui_pin_dial_turn(dui_pin_t *p, int steps, int64_t now, dui_pin_out_t *o); // the page (PAGES)
void dui_pin_dial_press(dui_pin_t *p, int64_t now, dui_pin_out_t *o);           // Cancel
void dui_pin_touch(dui_pin_t *p, int dir, int64_t now, dui_pin_out_t *o);       // the page (PAGES)
// Time passes (every 50 ms or so): the idle cancel, the flash's end, the countdown, ⌫ held.
void dui_pin_tick(dui_pin_t *p, int64_t now, dui_pin_out_t *o);
// The check's answer: DUI_PIN_R_*; wait_s for LOCKED.
void dui_pin_result(dui_pin_t *p, int result, int wait_s, int64_t now, dui_pin_out_t *o);
// The check couldn't be started now (the worker busy): "Busy, try again" for DUI_PIN_WRONG_MS, the digits
// taken (pin) given back for another OK; no locked sound.
void dui_pin_busy(dui_pin_t *p, const char *pin, int64_t now, dui_pin_out_t *o);
// The deck changed hands under a held key (its release may never come): drop it.
void dui_pin_cancel(dui_pin_t *p);
// After o->check: the digits into out (DUI_PIN_MAX + 1 bytes), wiped in p. The caller wipes out after use.
void dui_pin_take(dui_pin_t *p, char out[DUI_PIN_MAX + 1]);
// The words the pad shows: its title while digits go in (the locked key's action, "PIN to restart",
// "PIN: Wi-Fi off"; "Enter PIN" for one with no name), "Checking", "Wrong PIN", "Locked · wait N s", "Set a PIN on
// the web page". Never a digit typed.
int dui_pin_message(const dui_pin_t *p, int64_t now, char *out, int n);
// Its title alone (dui_pin_message's while digits go in).
int dui_pin_title(const dui_pin_t *p, char *out, int n);
// Key (col, row)'s press state on t (a copy from dui_layout_key): DUI_F_DOWN while held.
void dui_pin_decorate(const dui_pin_t *p, int col, int row, dui_tile_t *t);
// The strip / info bar while the pad is open: its words and the dots.
uint32_t dui_pin_bar_hash(const dui_pin_t *p, int64_t now);
void dui_render_pin_bar(const dui_pin_t *p, int64_t now, int w, int h, uint8_t *rgb);

// The unlock: the deck's locked actions work until it ends: the maintenance screen closes, the
// 60 s idle return home, or DUI_UNLOCK_MS after the right PIN.
typedef struct {
    bool on;
    int64_t until;
} dui_unlock_t;
void dui_unlock_start(dui_unlock_t *u, int64_t now);
// Ends it when one of those happened (closed: the screen closed; idle: the idle return was made); true
// when it ended now.
bool dui_unlock_step(dui_unlock_t *u, int64_t now, bool closed, bool idle);

#ifdef __cplusplus
}
#endif
