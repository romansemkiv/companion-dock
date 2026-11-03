#pragma once
/* The deck's static-address editor: Edit on the Network screen's Ethernet row opens it (the PIN
 * pad first while a settings PIN locks the deck). Pure: time is passed in (ms, monotonic); the save
 * itself is the caller's (Eth.SetConfig on a worker, ethDhcp false).
 *
 * Host mode (dui_ipe_open_host): Set IP on the Companion screen (Satellite mode) opens it for the Companion's
 * address: one field, "Companion IP" (complete and a host's: not 0/8, 127/8, 224/3; no subnet), then a summary with
 * the IP, the port it will use (read-only) and Save (a DUI_HOLD_COMP_MS hold, "Hold 1.5 s" / "reconnects"; locked
 * while the deck is), Back to the IP and Cancel. Keys, layouts, entry, cancel and idle as below; the save is the
 * caller's (Companion.SetConfig on a worker, companionAuto false). Everything below without "host mode" is the
 * Ethernet mode's, unchanged.
 *
 * - Fields, one at a time: the IP address, the Mask, the Gateway, the DNS, then a summary with Save.
 *   - IP, Gateway and DNS go in octet by octet, with no dot key: the current octet is outlined in cyan. A
 *     digit types it (the first digit typed replaces what was there); it moves on by itself after 3 digits,
 *     or when another digit would pass 255 (that digit starts the next octet) (only those two). Next:
 *     the next octet, then (the field checked) the next field; right after an auto-advance, with nothing
 *     typed yet, Next only confirms the move (it never skips an octet) and ⌫ goes back into the previous
 *     octet and deletes its last digit. Next on an empty Gateway or DNS skips it. ⌫ deletes the last
 *     digit, then goes back an octet (and a field).
 *   - The Mask is a prefix, /1 to /30, with − / + ("/24", shown as 255.255.255.0).
 *   - Prefilled with what dui_eth_static_pick gives (the static set if valid, else the DHCP lease, else
 *     empty).
 * - Checked on the deck: every octet 0-255 (by construction); the IP complete, a host's (not 0/8, 127/8,
 *   224/3), and not the network or broadcast address of its subnet (checked at the Mask); the Gateway empty,
 *   or complete, in the subnet, and neither the IP nor the subnet's network or broadcast address; the DNS
 *   empty or complete. An error shows in the status ("Gateway not in subnet"), with the
 *   locked sound, and keeps the field.
 * - The summary (IP / mask / gateway / DNS) is a sub-screen laid out by dui_plan: Back to the DNS, Save (a
 *   DUI_HOLD_ETH_MS hold, "Hold 1.5 s" / "net resets"; locked while the deck is) and Cancel.
 * - Cancel (✕, or ⌫ held DUI_PIN_CANCEL_HOLD_MS) leaves everything as it was; it cancels itself after
 *   DUI_IPE_IDLE_MS without a press.
 * - The layout per deck, the PIN pad's (dui_pin_style):
 *     PHONE (Module 32, XL)   status row ✕, the field's name, the 4 octets on 4 keys; the phone pad with
 *                              ⌫ / 0 / Next beside it
 *     PHONE_BAR (+ XL)        ✕, the phone pad; the field and its value on the strip
 *     ROWS (MK.2, 15)         status row ✕ ⌫, the value on 2 keys (2 octets each), Next; 1-5, 6-0
 *     PAGES (+, Neo)          the field and its value on the strip / info bar; the digits on two pages with
 *                              ⌫, Next and a page key; on the + a dial turn changes the current octet (the
 *                              prefix on the Mask) and a push is Next; the Neo's touch keys turn the page
 *     ROLLER (Mini, Module 6) − / the octet / +, then Next, ⌫, ✕; − / + held repeat, faster after a while
 *   The Mask puts − and + in place of the digits. */
#include <stdbool.h>
#include <stdint.h>

#include "deckui/deckui.h"
#include "deckui/screens.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DUI_IPE_IDLE_MS 60000 // without a press the editor cancels itself
#define DUI_IPE_ERR_MS 2500   // an error shows this long (or until the next press)
#define DUI_IPE_HINT_MS 2500  // Save let go early: "Hold 1.5 s" / "net resets" this long (time to read it)
#define DUI_IPE_REPEAT_MS 400 // − / + held: the first repeat
#define DUI_IPE_EVERY_MS 150  // ... then one this often
#define DUI_IPE_FAST_MS 1500  // ... held this long: 5 at a time (octets)

enum { DUI_IPE_IP, DUI_IPE_MASK, DUI_IPE_GW, DUI_IPE_DNS, DUI_IPE_SUM };
enum { DUI_IPE_ETH, DUI_IPE_HOST }; // what it edits: Ethernet's static set, the Companion's IP

// The editor's key actions (their tiles' action); the summary's also DUI_ACT_BACK and DUI_ACT_PAGER.
enum {
    DUI_IPE_A_DIGIT = 0x200, // + 0..9
    DUI_IPE_A_DEL = 0x20A,
    DUI_IPE_A_NEXT,
    DUI_IPE_A_CANCEL,
    DUI_IPE_A_PAGE,
    DUI_IPE_A_DOWN, // the octet (the prefix) - 1
    DUI_IPE_A_UP,   // ... + 1
    DUI_IPE_A_SAVE, // the summary's Save (a hold)
};

// The errors (dui_ipe_check), 0: none.
enum {
    DUI_IPE_OK,
    DUI_IPE_E_IP_PART,   // the IP needs its 4 octets
    DUI_IPE_E_NETWORK,   // the IP is its subnet's network address
    DUI_IPE_E_BROADCAST, // ... its broadcast address
    DUI_IPE_E_GW_PART,   // a Gateway begun but not finished
    DUI_IPE_E_GW_SUBNET, // the Gateway is not in the subnet
    DUI_IPE_E_DNS_PART,  // a DNS begun but not finished
    DUI_IPE_E_RESERVED,  // the IP is not a host's: 0/8, 127/8, multicast or reserved (224/3)
    DUI_IPE_E_GW_HOST,   // the Gateway is the IP itself, or the subnet's network or broadcast address
};

typedef struct {
    uint8_t v[4];
    uint8_t set; // bit i: octet i has a value
} dui_quad_t;

typedef struct {
    bool open;
    uint8_t kind;   // DUI_IPE_ETH, DUI_IPE_HOST
    uint16_t port;  // host mode: the port the summary shows (0: DUI_COMP_PORT)
    uint8_t style;  // DUI_PIN_PHONE ... (the PIN pad's)
    uint8_t field;  // DUI_IPE_*
    uint8_t oct;    // the current octet (IP, Gateway, DNS)
    bool fresh;     // ... not typed in yet: a digit replaces it
    uint8_t nd;     // ... digits typed into it (3: full)
    bool adv;       // ... reached by an auto-advance, nothing typed there yet (Next confirms, ⌫ goes back)
    dui_quad_t q[3]; // the IP, the Gateway, the DNS
    uint8_t prefix; // the Mask, 1..30
    uint8_t pg;     // PAGES, the summary: the page shown
    uint8_t pages;  // ... of the layout made last (dui_ipe_layout): paging wraps over them
    bool locked;    // the deck is locked (the caller keeps it up to date): Save shows a lock, does nothing
    uint8_t net;    // the summary's status signs, as the screen under it shows them (the caller's)
    int8_t rssi;
    bool hotspot;
    uint8_t wifi;   // DUI_SIGN_*
    bool eth_link;
    uint8_t err;    // DUI_IPE_E_* shown until err_until
    int64_t err_until;
    int64_t hint_until; // Save let go early: its hint until then
    int64_t last_press;
    struct {
        int8_t col, row; // the key held down (-1: none)
        uint16_t action;
        int64_t at, next; // when it went down; − / +: the next repeat
        bool fired;       // its hold fired (⌫: cancel, Save: saved)
    } held;
    char pool[64];      // the octets' text for the keys (dui_ipe_layout)
    dui_screen_t sum;   // the summary, for dui_plan (dui_ipe_layout)
} dui_ipe_t;

typedef struct {
    bool redraw;   // draw the editor again
    uint8_t sound; // DUI_SOUND_* (deckui/nav.h)
    bool closed;   // it closed (cancelled, saved, idle): the screen it came from comes back
    bool save;     // ... by Save held: save dui_ipe_values (ethDhcp false; host mode: its ip, companionAuto false)
    int64_t last_press; // with closed: its last press (an idle close is not a press of the screen's)
} dui_ipe_out_t;

// The mask of prefix (0..32) as octets; the prefix of a mask (-1: not a prefix mask).
void dui_mask_of_prefix(int prefix, uint8_t out[4]);
int dui_prefix_of_mask(const uint8_t m[4]);

// Opens it over the screen shown, prefilled from pre (may be NULL: empty), on the IP's first octet.
void dui_ipe_open(dui_ipe_t *e, const dui_deck_t *d, const dui_ipset_t *pre, int64_t now, dui_ipe_out_t *o);
// Host mode: the same for the Companion's IP, prefilled with ip (NULL, "", or not a dotted quad: empty);
// port: the summary's (0: DUI_COMP_PORT).
void dui_ipe_open_host(dui_ipe_t *e, const dui_deck_t *d, const char *ip, uint16_t port, int64_t now, dui_ipe_out_t *o);
void dui_ipe_close(dui_ipe_t *e);
// The editor's keys on deck d, as a layout for dui_layout_key (page: e->pg). It writes e's pool and summary
// screen, which L refers to.
void dui_ipe_layout(const dui_deck_t *d, dui_ipe_t *e, int64_t now, dui_layout_t *L);
// A key went down or up (L: dui_ipe_layout's).
void dui_ipe_key(dui_ipe_t *e, const dui_layout_t *L, int col, int row, bool down, int64_t now, dui_ipe_out_t *o);
void dui_ipe_dial_turn(dui_ipe_t *e, int steps, int64_t now, dui_ipe_out_t *o); // the octet / prefix; the summary's page
void dui_ipe_dial_press(dui_ipe_t *e, int64_t now, dui_ipe_out_t *o);           // Next
void dui_ipe_touch(dui_ipe_t *e, int dir, int64_t now, dui_ipe_out_t *o);       // the page
// Time passes (every 50 ms or so): the idle cancel, the error's end, ⌫ held, − / + repeating, Save held.
void dui_ipe_tick(dui_ipe_t *e, int64_t now, dui_ipe_out_t *o);
// The deck changed hands under a held key: drop it.
void dui_ipe_cancel(dui_ipe_t *e);
// Key (col, row)'s press state, Save's hold fill and hint on t (a copy from dui_layout_key).
void dui_ipe_decorate(const dui_ipe_t *e, int col, int row, int64_t now, dui_tile_t *t);

// The entry itself (what the keys do; for the tests too). A digit: false when refused (the last octet full).
bool dui_ipe_digit(dui_ipe_t *e, int digit);
void dui_ipe_del(dui_ipe_t *e);
// Next: DUI_IPE_OK, or the error that keeps the field.
int dui_ipe_next(dui_ipe_t *e);
// The current octet (or the prefix) up or down by steps (octets wrap 0-255; the prefix stays in 1-30).
void dui_ipe_step(dui_ipe_t *e, int steps);
// The field's check (DUI_IPE_* field): DUI_IPE_OK or an error.
int dui_ipe_check(const dui_ipe_t *e, int field);
// The set to save: ip, the mask dotted, gw "0.0.0.0" when empty, dns "" when empty.
void dui_ipe_values(const dui_ipe_t *e, dui_ipset_t *out);
// The status words: the field's name ("IP address", "Mask /24", "Gateway", "DNS", "Save?"; host mode
// "Companion IP", "Save?") or the error ("Gateway not in subnet").
int dui_ipe_message(const dui_ipe_t *e, int64_t now, char *out, int n);
// The strip / info bar while it is open: the field and its value (the current octet outlined), or the error.
uint32_t dui_ipe_bar_hash(const dui_ipe_t *e, int64_t now);
void dui_render_ipe_bar(const dui_ipe_t *e, int64_t now, int w, int h, uint8_t *rgb);

#ifdef __cplusplus
}
#endif
