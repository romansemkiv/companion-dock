#pragma once
/* The maintenance UI's screens: a snapshot of the box as plain
 * values (dui_box_t), and one builder per page that turns it into a dui_screen_t for dui_plan. Pure: no
 * ESP-IDF header, and none of netmgr's or slotsync's either: the enums below mirror their values, and
 * the caller (surfaces) fills the snapshot from them. The wording follows the
 * status words used elsewhere. */
#include <stdbool.h>
#include <stdint.h>

#include "deckui/deckui.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- The pages (one builder each) ----
enum {
    DUI_PAGE_HOME,
    DUI_PAGE_MENU, // decks whose menu is one key (dui_home_menu): Back + the four menu buttons
    DUI_PAGE_NETWORK,
    DUI_PAGE_COMPANION,
    DUI_PAGE_HOTSPOT,
    DUI_PAGE_QR, // Scan to join: from Hotspot, back to it
    DUI_PAGE_BOX,
    DUI_PAGE_CHOOSE, // Choose: the Companions found, from Companion, back to it
    DUI_PAGES_N
};

// ---- The actions on the keys: ids below DUI_ACT_BACK (deckui.h), 0 = none ----
enum {
    DUI_A_NONE,
    DUI_A_GO_MENU, // the navigation's own: go to that page (dui_nav runs them, never hands them out)
    DUI_A_GO_NETWORK,
    DUI_A_GO_COMPANION,
    DUI_A_GO_HOTSPOT,
    DUI_A_GO_QR, // Show QR
    DUI_A_GO_BOX,
    DUI_A_GO_CHOOSE, // Choose: the pick list; nothing is saved until a pick (DUI_A_PICK*)
    DUI_A_MODE_SAT,  // hold: switch to Satellite mode (restarts); then home
    DUI_A_MODE_DOCK, // hold: switch to Network Dock mode (restarts); then home
    DUI_A_RETRY,     // Retry now: the satellite connects again now
    DUI_A_PICK0,     // a Companion found, picked from the Choose home or page (Companion.Choose, pins it):
                     // DUI_A_PICK0 + its index (0..5)
    DUI_A_PICK5 = DUI_A_PICK0 + 5,
    DUI_A_AP_ON,     // Always on: the hotspot setting on and kept (Ap.SetConfig apEnabled true); locked
    DUI_A_AP_OFF,    // hold: Turn off the hotspot
    DUI_A_DIMMER,    // brightness - 10 (not below 10)
    DUI_A_BRIGHTER,  // brightness + 10 (not above 100)
    DUI_A_SOUND,     // the speaker on / off
    DUI_A_RESTART,   // hold: restart the box; then home
    DUI_A_SYNC,      // Sync now (no force: never over a newer image)
    DUI_A_BOOT_OTHER, // hold: boot the other slot (restarts); then home
    DUI_A_FLASH,     // the status light's flashes on / off (ledFlash)
    DUI_A_USE_MAIN,  // hold: use the main Wi-Fi network now
    DUI_A_USE_BACKUP, // hold: use the backup Wi-Fi network now
    DUI_A_ETH_DHCP,   // hold: Ethernet by DHCP (ethDhcp true)
    DUI_A_ETH_STATIC, // hold: Ethernet static from the saved set, else the DHCP lease (dui_eth_static_pick)
    DUI_A_ETH_EDIT,   // the static-address editor opens (the navigation's caller runs it, never the worker)
    DUI_A_ETH_SAVE,   // the editor's Save: arg "ip mask gw dns" (gw 0.0.0.0: none; dns "-": none), ethDhcp false
    DUI_A_COMP_EDIT,  // Set IP (Satellite): the editor in host mode opens (the navigation's caller runs it)
    DUI_A_COMP_SAVE,  // ... its Save: arg the dotted IP; as Companion.SetConfig {companionAuto false, companionHost}
    DUI_A_COMP_AUTO,  // Auto find (a manual Companion): companionAuto on again
    DUI_A_WIFI_OFF,   // hold: the Wi-Fi station off (wifiEnabled false; the setup hotspot stays)
    DUI_A_WIFI_ON,    // the Wi-Fi station on again: from the main network
    DUI_A_COUNT
};

// the actions a settings password locks (the mode switch, Hotspot off and Always on, Restart, Sync now,
// Boot other, and the later ones listed in screens.c). The builders lock their buttons with it; surfaces and the action
// worker refuse them with it.
bool dui_action_locked(int action);
// a deck shows its locks while a secret is set and it isn't unlocked with the PIN.
bool dui_deck_locked(bool settings_locked, bool unlocked);
// an action runs: not a locked one, no secret set, or the deck it came from was unlocked.
bool dui_action_allowed(int action, bool secret_set, bool unlocked);

// The holds (everything else is a tap), and the hints shown when one is let go early (dui_hint_text):
// how long, then what it does, each line 14 px or more on a 72 px key.
#define DUI_HOLD_MODE_MS 1500    // the other mode: "Hold 1.5 s" / "restarts"
#define DUI_HOLD_AP_OFF_MS 1500  // Turn off (danger): "Hold 1.5 s" / "cuts users" (its clients)
#define DUI_HOLD_RESTART_MS 2000 // Restart (danger): "Hold 2 s" / "restarts"
#define DUI_HOLD_BOOT_MS 2000    // Boot other (danger): "Hold 2 s" / "restarts"
#define DUI_HOLD_USE_MS 1500     // Use (a Wi-Fi network): "Hold 1.5 s" / "Wi-Fi blips"
#define DUI_HOLD_ETH_MS 1500     // Use DHCP / Make static, Save: "Hold 1.5 s" / "net resets"
#define DUI_HOLD_WIFI_MS 1500    // Wi-Fi off: "Hold 1.5 s" / "Wi-Fi off"
#define DUI_HOLD_COMP_MS 1500    // the Companion IP's Save: "Hold 1.5 s" / "reconnects"
#define DUI_COMP_PORT 16622      // companionPort's default: the Companion IP's summary shows it when none is set

// ---- The snapshot: everything a screen shows ----
#define DUI_COMPS_MAX 6

enum { DUI_MODE_SAT, DUI_MODE_DOCK }; // the surface mode the box runs
// The satellite's state (satellite mode).
enum {
    DUI_SAT_NONET,     // no network
    DUI_SAT_FINDING,   // finding Companion
    DUI_SAT_CHOOSE,    // several found, none picked
    DUI_SAT_TOO_OLD,   // the Companion found is too old (sat.version)
    DUI_SAT_RECON,     // (re)connecting to the target
    DUI_SAT_CONNECTED, // connected
};
// The Wi-Fi network joined (= NETMGR_STA_NONE / MAIN / BACKUP, netmgr_status_t.sta_net).
enum { DUI_WIFI_NONE, DUI_WIFI_MAIN, DUI_WIFI_BACKUP };
// The recovery slot (= slotsync_state_t, in its order).
enum {
    DUI_SLOT_IN_SYNC,
    DUI_SLOT_SYNCING,
    DUI_SLOT_DIFFERS,     // a valid image built earlier: Sync now
    DUI_SLOT_OTHER_NEWER, // a valid image built in the same second or later: Boot other only
    DUI_SLOT_EMPTY,       // Sync now
    DUI_SLOT_INVALID,     // also another project's image: Sync now, never booted
    DUI_SLOT_WAITING,     // a copy is due but held
};

typedef struct {
    char name[64]; // what the person knows it by (its host name)
    char ip[16];   // "" when unknown
} dui_comp_t;

typedef struct {
    uint8_t mode; // DUI_MODE_*
    struct {
        uint8_t state;                   // DUI_SAT_*
        bool auto_disc;                  // companionAuto: Companions found by mDNS (else a manual address)
        uint8_t ncomps;                  // Companions found (Choose), up to DUI_COMPS_MAX
        dui_comp_t comp[DUI_COMPS_MAX];
        dui_comp_t target;               // the one it connects to; name "" when none
        char saved_ip[16];               // companionHost when it is an IPv4 address; "" otherwise (the prefill)
        uint16_t port;                   // companionPort (0: unknown, DUI_COMP_PORT shown)
        char version[24];                // the version of the Companion found (too old)
    } sat;
    struct { // this deck's dock (dock mode: the snapshot is per deck)
        uint16_t port;  // its port; 0 when it has none
        char peer[16];  // the address of the Companion connected to it; "" when none
        bool no_free;   // every dock taken (the deck limit's): "No free dock"
    } dock;
    struct {
        bool eth_link;
        char eth_ip[16];        // Ethernet's own address while linked (whatever routes); "" when none
        bool eth_dhcp;          // Ethernet's addressing: DHCP (else static), as the settings say
        bool eth_static_ok;     // a static set to switch to: the saved one is valid, or there is a lease
        bool eth_static_saved;  // ... and it is the saved one (else the lease): Make static says which
        char wifi_main[33];     // the main network's SSID; "" when Wi-Fi isn't set up
        char wifi_backup[33];   // the backup network's SSID; "" when none
        bool wifi_off;          // the station is off by the settings (wifiEnabled false): both rows "Off"
        uint8_t wifi_net;       // DUI_WIFI_*: the one joined
        int8_t rssi;            // its signal, dBm (0: unknown)
        uint8_t wifi_trying;    // DUI_WIFI_*: the one being tried while none is joined
        uint8_t wifi_not_found; // bit 1 << (DUI_WIFI_x - 1): the driver found no such network last time
        bool radio_down;        // the radio (the co-processor) isn't running: no Wi-Fi at all
        char wifi_ip[16];       // the joined network's own address (also behind Ethernet); "" when none
        bool ap_on;             // the hotspot
        char ap_ssid[33];
        char ap_pass[65];
        bool ap_secured;        // it has a password (false: open)
        char ap_ip[16];         // its address (the setup page)
        char name[33];          // the box name (<name>.local)
    } net;
    struct {
        char version[32];       // the running firmware
        uint32_t uptime_s;
        uint8_t slot;           // DUI_SLOT_*
        uint8_t pct;            // syncing: 0..100
        char other_version[32]; // the other slot's version; "" when none can be read
        bool other_valid;       // a valid image of this project (slotsync other_valid)
        bool other_foreign;     // another project's
        bool pending;           // the running image is pending verify: no Boot other
        bool flash_busy;        // an upload holds the flash (or a restart into the other slot): Sync says "wait"
        bool running_b;         // the running slot is the second one (ota_1): "Slot B" runs
    } box;
    uint8_t brightness;         // 10..100 %
    bool sound;                 // the speaker
    bool flash;                 // the status light flashes on each event (ledFlash)
    bool locked;                // a settings password is set
    bool ondemand;              // the maintenance screen opened on demand: a Close key on home
    bool connected;             // Companion drives this deck (the "Companion connected" home)
    uint8_t deck_limit;         // the Stream Decks the box takes at once, 2 or 4 ("Max N decks"; 0: 2)
    char toast[40];             // a short notice after an action ("Restarting"); "" none
    uint8_t toast_state;        // its outcome (DUI_STATE_OK / WAIT / BAD; NONE: OK)
} dui_box_t;

// ---- Ethernet's static address ----
typedef struct {
    char ip[16], mask[16], gw[16], dns[16]; // dotted quads; gw "" or 0.0.0.0: none; dns "" none
} dui_ipset_t;
// s is a dotted quad as the settings take it (4 parts, 1-3 digits each, 0-255): its octets into out (may be NULL).
bool dui_ipv4_parse(const char *s, uint8_t out[4]);
// A set Eth.SetConfig takes and that can work: ip, mask and gw quads (gw 0.0.0.0: none), dns "" or a quad;
// ip and mask not 0.0.0.0.
bool dui_ipset_valid(const dui_ipset_t *s);
enum { DUI_ETH_FROM_NONE, DUI_ETH_FROM_SAVED, DUI_ETH_FROM_LEASE };
// the static set Make static uses: the saved one when it is valid, else the DHCP lease (valid: an
// address and a mask; its gateway and DNS as given, 0.0.0.0 DNS as none), else none. out gets it (may be
// NULL; cleared for none). saved or lease may be NULL.
int dui_eth_static_pick(const dui_ipset_t *saved, const dui_ipset_t *lease, dui_ipset_t *out);

// ---- The builders: s is initialised and filled; every string copied (b may go away) ----
void dui_build(dui_screen_t *s, int page, const dui_box_t *b); // the builder of page (DUI_PAGE_*)
void dui_build_home(dui_screen_t *s, const dui_box_t *b);
void dui_build_menu(dui_screen_t *s, const dui_box_t *b);
void dui_build_network(dui_screen_t *s, const dui_box_t *b);
void dui_build_companion(dui_screen_t *s, const dui_box_t *b);
void dui_build_hotspot(dui_screen_t *s, const dui_box_t *b);
void dui_build_qr(dui_screen_t *s, const dui_box_t *b);
void dui_build_box(dui_screen_t *s, const dui_box_t *b);
void dui_build_choose(dui_screen_t *s, const dui_box_t *b);

#ifdef __cplusplus
}
#endif
