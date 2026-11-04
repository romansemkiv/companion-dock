#pragma once
/* Pure: the maintenance UI's glue inside surfaces: the snapshot the
 * deck's screens show (deckui's dui_box_t), made from what netmgr, slotsync, the Satellite client,
 * discovery and the settings say; who gets a deck's input in each state; and a deck's input as the
 * navigation's events. Host-tested (test/host/test_maintmap.c). */
#include <stdbool.h>
#include <stdint.h>

#include "deckmodel/model.h"
#include "deckproto/deckproto.h"
#include "deckui/screens.h"
#include "discovery/discovery.h"
#include "netmgr/netmgr.h"
#include "slotsync/slotsync.h"
#include "surfaces/surfaces.h" /* SURF_MAX_DECKS */

#ifdef __cplusplus
extern "C" {
#endif

/* What the surfaces task gathers, as plain values. Strings may be NULL (""). */
typedef struct {
    bool dock;                    /* Network Dock mode */
    const netmgr_status_t *ns;
    const slotsync_status_t *ss;  /* NULL: no slot information yet */
    /* the settings */
    const char *box_name, *wifi_main, *wifi_backup;
    bool ap_shown, ap_open;       /* the hotspot as surfaces_set_ap_info gives it (its SSID, no password) */
    const char *ap_ssid, *ap_pass; /* ap_ssid "": netmgr's */
    uint8_t brightness;           /* deckBrightness */
    bool sound, locked, ondemand;
    bool flash;                   /* ledFlash */
    /* Ethernet's addressing: ethDhcp, the saved static set and the lease now (NULL: none) */
    bool eth_dhcp;
    const dui_ipset_t *eth_saved, *eth_lease;
    /* the firmware */
    const char *version;
    uint32_t uptime_s;
    bool pending;                 /* the running image is pending verify */
    /* Satellite mode */
    sel_result_t sel;             /* the Companion selection (SEL_AMBIGUOUS: choose) */
    bool companion_auto;          /* companionAuto: auto-discovery (Choose only then) */
    const char *companion_host;   /* companionHost, the saved one (Set IP's prefill when an IPv4) */
    uint16_t companion_port;      /* companionPort (Set IP's summary) */
    bool too_old;                 /* the Companion found is too old */
    bool live;                    /* Companion's session is ready */
    const char *target_host;      /* the one it connects to ("" none) */
    const char *companion_version; /* the Companion found (too old) */
    const companion_instance_t *comps; /* seen by mDNS */
    int ncomps;
    /* this deck */
    bool connected;               /* Companion drives it */
    uint16_t dock_port;           /* its dock's port (0: none) */
    uint8_t deck_limit;           /* the decks the box takes at once (2 or 4) */
    const char *dock_peer;        /* the Companion connected to its dock */
    const char *toast;
    uint8_t toast_state;          /* its outcome: DUI_STATE_OK / WAIT / BAD */
} surf_maint_in_t;

/* The snapshot for one deck. The hotspot shows only while it is on and surfaces_set_ap_info had its SSID;
 * Ethernet's address is the routed one when Ethernet routes, the station's when Wi-Fi does; the
 * Satellite's state: no route, choose, too old, connected (live), reconnecting (a target and not live),
 * else finding; a dock port of 0 is "no free dock". */
void surf_maint_box(dui_box_t *b, const surf_maint_in_t *in);

/* Who gets a deck's input (key, dial, touch). */
typedef enum { SURF_TO_DROP, SURF_TO_COMPANION, SURF_TO_BOX, SURF_TO_WAKE } surf_input_to_t;

/* has_display: the deck has a screen (the Pedal has none: it never shows the maintenance UI);
 * ondemand: the maintenance screen on demand is asked for (box-wide); ui_shown: the deck shows the
 * maintenance UI now; companion_drives: Companion drives the deck (Satellite: the session is ready and
 * the deck added; dock mode: Companion has the deck's connection); handing_back: the screen on demand
 * just left this deck and Companion's images are not back on it yet; loss: Companion went a moment ago and the
 * deck still shows its last images (dimmed with "Reconnecting" after 2 s). The UI shown takes every press; on
 * demand asked for but not on this deck yet, or handing back: nobody (never Companion); Companion when it drives
 * the deck; in the loss phase, a press wakes the box's screens on the deck at once (WAKE: it does nothing else);
 * else nobody. */
surf_input_to_t surf_input_route(bool has_display, bool ondemand, bool ui_shown, bool companion_drives,
                                 bool handing_back, bool loss);

/* The hand-back after the screen on demand left a deck Companion drives; until it is over, the
 * deck's input goes to nobody (surf_input_route's handing_back). */
typedef enum { SURF_HB_WAIT, SURF_HB_BACK, SURF_HB_LATE } surf_handback_t;

/* The hand-back's cap: 3 s for every deck. A deck whose images are written sooner hands back sooner;
 * the slow full-speed BMP decks (the original Stream Deck, the Mini) get the time to rewrite every key. */
#define SURF_HANDBACK_US (3000 * 1000LL)

/* handed: every one of Companion's images for the deck has been handed to usbdeck (Satellite: nothing of
 * the deck's pending in the coalescer and imgwork not at one of its frames; dock mode: dockmode's cache
 * written back). drained: usbdeck has written all it was handed for the deck (usbdeck_writes_drained:
 * its command queue empty, nothing being sent), looked at after handed, so that a frame handed on
 * between the two looks is still waited for. until_us: the cap. BACK once both hold (the images are on
 * the deck); LATE at the cap whatever they say; else WAIT. */
surf_handback_t surf_handback_step(bool handed, bool drained, int64_t now_us, int64_t until_us);

/* ---- The decks the screen on demand (the box menu) is on ---- */
/* A deck the screen on demand could be on, as the registry has it. */
typedef struct {
    const deck_model_t *model; /* NULL: a free slot */
    const char *device_id;     /* "streamdeck:<serial>": the deck's identity, the same after a replug */
    uint32_t seq;              /* attach order: smaller is earlier */
} surf_od_cand_t;

/* The deck the service button and the RPC with no deck named open on: the biggest
 * attached deck with a screen, by most keys (cols x rows), then the larger key, then one with a strip or an info
 * bar, then the earliest attached. The Pedal (no screen) never counts. Its index in c; -1 when no deck has a
 * screen. */
int surf_od_pick(const surf_od_cand_t *c, int n);
/* The deck with this device_id among c, if it has a screen (attached; the same id after a replug); -1 none. */
int surf_od_find(const surf_od_cand_t *c, int n, const char *device_id);

/* ---- A box menu per deck ---- */
/* The decks the box menu (the screen on demand) is open on, by device_id: any number, each on its own. */
typedef struct {
    char id[SURF_MAX_DECKS][48];
    int n;
} surf_od_set_t;
/* device_id is in the set: the box menu is open on that deck. Every other deck stays as it is: Companion's
 * keeps its images and presses, one the box drives its own screen. */
bool surf_od_here(const surf_od_set_t *s, const char *device_id);
/* Opens it on device_id (no duplicates); false when it was there already, the id is empty, or the set is full. */
bool surf_od_add(surf_od_set_t *s, const char *device_id);
/* Closes it on device_id; false when it wasn't open there. */
bool surf_od_del(surf_od_set_t *s, const char *device_id);
/* Closes it on every deck in the set that c no longer has with a screen (unplugged: a replugged deck comes back
 * closed); each one's id into gone (up to max); returns how many. */
int surf_od_prune(surf_od_set_t *s, const surf_od_cand_t *c, int n, char gone[][48], int max);

/* A deck as the dock input hold sees it. */
typedef struct {
    const char *device_id;
    bool covered;      /* the screen on demand covers it over Companion now (surf.ui_od) */
    bool handing_back; /* it closed, Companion's images not written back yet (surf.handback) */
} surf_hold_cand_t;
/* The decks whose input stays held from Companion (dock mode): every deck the box menu is open on
 * (from the moment it opens), and every deck still covered or handing back: opening or closing one never cancels
 * another deck's hand-back. Their device_ids into out (pointers into c / s, no duplicates, at most max); returns
 * how many. */
int surf_held_decks(const surf_od_set_t *s, const surf_hold_cand_t *c, int n, const char **out, int max);

/* ---- The deck's brightness (-1: nothing to set) ---- */
/* While the deck shows the maintenance screen (the box drives it, or on demand over Companion), its
 * brightness is deckBrightness, set at once when it differs from what the box set last (applied, -1 none);
 * 0 (no setting yet): nothing. */
int surf_bright_apply(uint8_t deck_brightness, int applied);
/* Where a brightness goes. */
typedef enum {
    SURF_BRIGHT_DIRECT,    /* to the deck now (usbdeck) */
    SURF_BRIGHT_DOCKCACHE, /* through dockmode's cache: between Companion's image sequences */
    SURF_BRIGHT_KEEP,      /* kept for the hand-back, not shown */
} surf_bright_path_t;
/* The box's brightness for a deck: through dockmode's cache while Companion's dock connection
 * owns the deck (dock mode and owned, whatever an earlier look said), else straight to the deck. */
surf_bright_path_t surf_bright_box_path(bool dock, bool owned);
/* Companion's brightness for a deck (Satellite): kept for the hand-back while the screen on demand covers
 * it, else to the deck now. */
surf_bright_path_t surf_bright_comp_path(bool covered);
/* On demand closed over Companion's deck: Companion's last brightness for it (-1: unknown) goes back when
 * the box set one meanwhile. */
int surf_bright_handback(int companion_last, int applied);

/* A deck's input as the maintenance UI's navigation takes it (deckui/nav.h). */
typedef enum {
    SURF_NAV_NONE,
    SURF_NAV_KEY,   /* dui_nav_key(col, row, down) */
    SURF_NAV_DIAL,  /* dui_nav_dial_turn(steps): an encoder turned, the strip swiped sideways */
    SURF_NAV_BACK,  /* dui_nav_dial_press: an encoder pushed */
    SURF_NAV_TOUCH, /* dui_nav_touch(steps): the Neo's touch keys, -1 left, +1 right */
    SURF_NAV_BAR,   /* dui_nav_bar_tap(x): a tap on the + / + XL strip, x in its pixels */
} surf_nav_kind_t;

typedef struct {
    surf_nav_kind_t kind;
    int col, row;
    bool down;
    int steps;
    int x;
} surf_nav_ev_t;

/* Keys by their place (the Original v1 reports each row mirrored: deck_model_controls); the Neo's touch
 * keys on their press; encoders turned (their delta) or pushed (on the push); the strip swiped sideways
 * (swipe left = the next page, as Companion's page swipe), a tap on it its x (the indicators). */
surf_nav_ev_t surf_nav_event(const deck_model_t *m, const deck_event_t *ev);

#ifdef __cplusplus
}
#endif
