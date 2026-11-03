#pragma once
/* Glue between USB Stream Decks and Companion: the Satellite connection, or the Network Dock servers
 * (dock mode), and the box's own maintenance screens on the decks
 * (components/deckui).
 * The registry, coalescer, input map and message check are pure C (host-tested); the rest is
 * ESP-only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "deckmodel/model.h"
#include "deckproto/corner.h"
#include "deckproto/deckproto.h"
#include "satproto/session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SURF_MAX_DECKS 6
#define SURF_MAX_CTLS 64

/* ---- Registry (pure; the caller provides locking) ---- */

typedef struct {
    char device_id[48]; /* "" = free slot */
    void *deck;
    const deck_model_t *model;
    deck_ctl_t ctls[SURF_MAX_CTLS];
    int nctl;
    bool added; /* ADD-DEVICE issued in the current Companion session */
} surf_slot_t;

typedef struct {
    surf_slot_t slot[SURF_MAX_DECKS];
    uint8_t cap; /* the deck limit, the decks it takes at once (slots 0 .. cap - 1); 0: SURF_MAX_DECKS */
} surf_registry_t;

/* Fills ctls from the model. A device_id that is already registered keeps its slot (and its
 * `added` flag): the deck handle and model are replaced. A new one takes a free slot below the cap. Returns
 * the slot, -1 when full (the cap reached: the deck is ignored) or on bad arguments. */
int surfreg_attach(surf_registry_t *r, const char *device_id, void *deck, const deck_model_t *m);
/* Frees the slot holding `deck`; returns that slot, or -1 if no slot holds it. */
int surfreg_detach(surf_registry_t *r, void *deck);
int surfreg_find(const surf_registry_t *r, const char *device_id);
void surfreg_mark_all_unadded(surf_registry_t *r);
/* Writes up to max attached slots that are not added; returns the count. */
int surf_pending_adds(const surf_registry_t *r, int *slots, int max);
/* Routes a KEY-STATE: 0 with slot/ctl (pointer into the registry) and expected_bytes = w*h*3
 * (0 for colour-only controls), or -1 to drop it (unknown device or control, or a control
 * without any output such as a pedal input or an encoder). */
int surf_route_keystate(const surf_registry_t *r, const sat_key_state_t *ks, int *slot,
                        const deck_ctl_t **ctl, size_t *expected_bytes);
/* True when a decoded bitmap of `got` bytes may be drawn on a control expecting `expected`. */
bool surf_bitmap_ok(size_t expected, size_t got);

/* ---- Coalescer (pure; one pending item per control) ---- */

typedef struct {
    void *pending[SURF_MAX_DECKS][SURF_MAX_CTLS];
    uint8_t next[SURF_MAX_DECKS];
} coalesce_t; /* zero-initialise */

/* Stores item as the control's pending item. Returns the item it replaced (caller frees), or
 * NULL. Out-of-range slot/ctl: nothing is stored and `item` itself is returned. */
void *coalesce_put(coalesce_t *c, int slot, int ctl, void *item);
/* Removes and returns the slot's next pending item, round-robin from the control after the one
 * taken last; NULL when the slot has nothing pending. */
void *coalesce_take(coalesce_t *c, int slot, int *ctl);
/* Removes every pending item of the slot, passing each to free_fn (if not NULL). */
void coalesce_drop_slot(coalesce_t *c, int slot, void (*free_fn)(void *));

/* ---- Input map (pure) ---- */

typedef enum { ACT_PRESS, ACT_RELEASE, ACT_TAP, ACT_ROTATE, ACT_PAGE } act_kind_t;

typedef struct {
    act_kind_t kind;
    char control_id[8]; /* "row/col"; "" for ACT_PAGE */
    int delta;          /* ACT_ROTATE */
    bool forward;       /* ACT_PAGE */
} surf_action_t;

/* Translates one deck input event into Companion actions; returns the number written (<= max).
 * ACT_TAP means press then release. */
int inputmap_translate(const deck_model_t *m, const deck_event_t *ev, surf_action_t *out, int max);
/* A key-down (for the key click): a key, pedal or touch key pressed, an encoder pushed, the LCD strip
 * tapped or held. Not a release, a turn or a swipe. */
bool inputmap_key_down(const deck_event_t *ev);

/* ---- Custom deck message (pure) ---- */

#define SURF_MSG_MAX 160    /* bytes of message text */
#define SURF_MSG_MAX_S 3600 /* longest timed message, seconds */

/* True when text is 1..SURF_MSG_MAX bytes, each printable ASCII (0x20..0x7E) or '\n', and
 * seconds <= SURF_MSG_MAX_S (0 = until cleared). */
bool surf_message_valid(const char *text, uint32_t seconds);

typedef struct {
    bool on;          /* a message is shown */
    int64_t until_us; /* when it expires; 0 = until cleared */
    uint32_t seq;     /* bumped by every show, clear and expiry */
    uint32_t seen;    /* seq the surfaces task last acted on */
} surf_message_t;     /* zero-initialise; the caller provides locking */

typedef enum {
    SURF_MSG_IDLE, /* nothing to do */
    SURF_MSG_DRAW, /* a message is shown: make sure every deck has the one numbered seq */
    SURF_MSG_END,  /* a message was shown and is gone (perhaps before any step saw it): redraw */
} surf_message_action_t;

/* Shows (or replaces) the message until until_us (0 = until cleared). */
void surf_message_show(surf_message_t *m, int64_t until_us);
/* Clears the message; false when none was shown. */
bool surf_message_clear(surf_message_t *m);
/* One pass of the surfaces task at now_us: expires the message when its time has come (*expired,
 * if not NULL, says so), then returns what to do. */
surf_message_action_t surf_message_step(surf_message_t *m, int64_t now_us, bool *expired);

#ifdef ESP_PLATFORM
#include "discovery/discovery.h"
#include "esp_err.h"

/* Starts satclient and usbdeck with this module's callbacks plus the "surfaces" and "imgwork"
 * tasks. Call once, after the network stack is up. deck_limit (deckLimit read at start: 2 or 4): the
 * decks taken at once; one more is ignored (never shown, never added to Companion, no box menu; a problem). */
esp_err_t surfaces_start(int deck_limit);
/* Network Dock mode, instead of surfaces_start: starts elgdock (a dock's port opens and is
 * announced as mDNS _elg._tcp while it has a deck, unless announce is false; mDNS must be initialised)
 * and usbdeck. Every deck gets a dock and shows the dock status screen ("Remote surface", its IP and
 * port) until Companion connects to it; Companion then drives it directly. mac: Ethernet MAC (the
 * decks' report 0x85). */
esp_err_t surfaces_start_dock(const char *box_name, const uint8_t mac[6], bool announce, int deck_limit);
/* the decks attached and ignored over the deck limit (until each is unplugged); whether the deck with this
 * serial is one of them. Any task. */
int surfaces_decks_ignored(void);
bool surfaces_deck_ignored(const char *serial);
/* Dock mode: box name or mDNS setting changed (re-announces the docks). No-op in Satellite mode. */
void surfaces_dock_apply(const char *box_name, bool announce);
/* Network status changed: the maintenance screens show it. */
void surfaces_net_changed(void);

/* ---- The maintenance screens ----
 * Every deck with a screen that Companion doesn't drive shows them (in place of the status screens
 * before). The maintenance screen on demand shows them on one deck, the biggest, Companion's
 * too: its input then goes to the box only (never to Companion), Companion stays connected and its
 * images keep updating the cache, which comes back when it closes (Close, the service button again, the
 * RPC, or 60 s without a press on it); every other deck goes on as it was. A custom message
 * (surfaces_show_message) outranks the screens on a deck the box drives, and the screen on demand
 * outranks the message on its deck. */

/* What the screens show of the settings (main fills it from the config and the auth). */
typedef struct {
    char box_name[33];
    char wifi_main[33], wifi_backup[33]; /* the station networks' SSIDs ("" none) */
    uint8_t brightness;                  /* deckBrightness: applied while the box drives a deck */
    bool sound;                          /* speakerEnabled */
    bool led_flash;                      /* ledFlash: the status light flashes on each event */
    bool locked;                         /* a settings password is set */
    bool companion_auto;                 /* companionAuto: Companion found by mDNS (Choose only then) */
    char companion_host[64];             /* companionHost: the manual address (Set IP's prefill when an IPv4) */
    uint16_t companion_port;             /* companionPort (Set IP's summary) */
    bool pin;                            /* the settings secret is a PIN: the decks can be unlocked */
    bool eth_dhcp;                       /* ethDhcp: Ethernet by DHCP, else static */
    char eth_ip[16], eth_mask[16], eth_gw[16], eth_dns[16]; /* the saved static set */
    /* The box menu gesture on every deck Companion drives (gestureEnabled, gestureKeys, gestureTaps,
     * gestureHoldSeconds), both modes, at once; none given yet (all zero) is the default (corner_cfg_use) */
    corner_cfg_t gesture;
} surf_settings_t;

/* A deck PIN check's answer (surfaces_pin_result). BUSY: kept for the enum's order; a busy check is
 * now answered by pin_check returning false, never through the queue. */
enum { SURF_PIN_OK, SURF_PIN_WRONG, SURF_PIN_LOCKED, SURF_PIN_NONE, SURF_PIN_BUSY };

typedef struct {
    /* A maintenance action (deckui/screens.h DUI_A_*, never a GO one): queued for a worker of the
     * callee's (actions write flash, restart Wi-Fi or the box), never run on the caller's task. arg:
     * the Companion's id for DUI_A_PICK0..5, else "". The callee refuses the locked ones while a
     * settings PIN is set, unless unlocked: the deck it came from was unlocked with the PIN. */
    void (*action)(int action, const char *arg, bool unlocked);
    /* check pin (4-8 digits) entered on slot's deck, off the caller's task (PBKDF2); the answer
     * comes back by surfaces_pin_result(slot, ticket, ...). pin is copied; the caller wipes it. Never
     * blocks and never answers from inside: false means busy (another check running, or the worker's
     * queue full), and nothing comes back; the caller shows it at once and the user tries again. */
    bool (*pin_check)(int slot, uint32_t ticket, const char *pin);
    /* seconds the decks must still wait before a PIN try (0: they may). Short, never hashes. */
    int (*pin_wait_s)(void);
} surf_hooks_t;

/* The answer to a pin_check (SURF_PIN_*; wait_s for LOCKED), for the deck's PIN pad. Any task. */
void surfaces_pin_result(int slot, uint32_t ticket, int result, int wait_s);

/* Call before surfaces_start / surfaces_start_dock (copied). */
void surfaces_set_hooks(const surf_hooks_t *h);
/* The box menu (the maintenance screen on demand), one per deck: it opens and closes on each deck
 * with a screen on its own, any number open at once; every other deck stays as it is; a deck unplugged closes it
 * on that deck, and a deck plugged in (or replugged) never arrives with it open. Any task, never blocks.
 * surfaces_maint, no deck named: on opens it on the biggest attached deck with a screen (most keys, then
 * the larger key, then a strip or info bar, then the earliest attached; ESP_ERR_NOT_FOUND when there is none);
 * off closes it on every deck. */
esp_err_t surfaces_maint(bool on);
/* On one deck, by its serial (or device_id; NULL or "": as surfaces_maint): ESP_ERR_NOT_FOUND when no attached
 * deck with a screen has it. Opening where open, or closing where closed, is ESP_OK. */
esp_err_t surfaces_maint_deck_set(const char *serial, bool on);
/* Open on any deck. Lock-free. */
bool surfaces_maint_on(void);
/* The serials it is open on (33 bytes each), up to max; returns how many. Any task. */
int surfaces_maint_list(char serials[][33], int max);
/* Open on the deck with this serial; *screen (may be NULL): it is attached and has a screen. Any task. */
bool surfaces_deck_menu(const char *serial, bool *screen);
/* The service button's press, asked of the surfaces task by a caller with a small stack (its poll task):
 * counted and the task poked, nothing else here (no lock, no log). On an odd count the surfaces task, from its
 * own state: any deck open, it closes them all; none, it opens on the biggest (surfaces_maint). Two presses
 * before it gets to them cancel out; surfaces_maint logs it, a refusal (no deck with a screen) too. */
void surfaces_maint_toggle(void);
/* The first deck it is open on, else the one surfaces_maint(true) would open on now (Sys.GetStatus's
 * maintenanceDeck, kept for compatibility): its serial and model name (33 bytes each); false when there is none. */
bool surfaces_maint_deck(char serial[33], char model[33]);
/* The deck surfaces_maint(true) opens on: its serial and model name; false when there is none. */
bool surfaces_maint_pick(char serial[33], char model[33]);
/* A short notice in the decks' status for about 2 s ("Restarting"; an action's result), taking the status over on
 * every deck, in its outcome's colour: SURF_TOAST_DONE green, SURF_TOAST_BUSY amber (under way),
 * SURF_TOAST_REFUSED red (the text its two-word reason, dui_refusal). Any task. */
enum { SURF_TOAST_DONE, SURF_TOAST_BUSY, SURF_TOAST_REFUSED };
void surfaces_toast(int outcome, const char *text);
/* The settings the screens show, copied: whoever owns the config gives them on every change, so the
 * surfaces task never waits on the config lock (held across NVS saves). Any task. */
void surfaces_set_settings(const surf_settings_t *st);
/* Companion and the decks, for the status light and sound (the supervisor reports it to
 * components/feedback). driving: Satellite: Companion's session is ready; dock mode: Companion drives
 * at least one deck. lost: Companion went away after it drove them (Satellite: the session ended
 * after it was ready; dock mode: it let go of a deck that is still plugged in), until it drives that
 * one again (a deck unplugged is no loss). */
void surfaces_companion(bool *driving, bool *lost);
/* Latest Companion selection result, so the status screens can ask the user to choose. */
void surfaces_set_selection(sel_result_t sel);
/* The hotspot as the status screens show it while it is on. ssid_or_null NULL: no WiFi lines (the
 * hotspot is off). pass_or_null NULL: it is open, the screens say "No password". Strings are
 * copied; may be called before surfaces_start. */
void surfaces_set_ap_info(const char *ssid_or_null, const char *pass_or_null);
/* Most recent problems (newest first, no duplicates, at most 8); returns the count. */
int surfaces_problem_list(char out[][96], int max);
/* Draws text (see surf_message_valid) on every deck's LCD keys, one word per key (a deck with too few
 * keys pages through it every 3 s), also decks attached later,
 * for `seconds` (0 = until cleared); replaces a message already shown. Outranks everything else on
 * the keys: Companion's key images keep updating the cache but are drawn again only when the
 * message ends (cleared or expired). Input keeps going to Companion. ESP_ERR_INVALID_ARG for bad
 * text or seconds. */
esp_err_t surfaces_show_message(const char *text, uint32_t seconds);
void surfaces_clear_message(void);
/* True while a message is shown: copies its text (cap includes the NUL; "" when none) and the
 * seconds left (rounded up; 0 = until cleared). text and remaining_s may be NULL. */
bool surfaces_message_active(char *text, size_t cap, uint32_t *remaining_s);
#endif

#ifdef __cplusplus
}
#endif
