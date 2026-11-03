#pragma once
/* The deck maintenance UI's reference screens as test data (home, menu, network, companion, hotspot, qr,
 * box), with a simulated box. For the deckui tests and the render tool (deckui_renders); screens.c builds
 * the real ones from live data. */
#include <stdbool.h>
#include "deckui/deckui.h"

// The reference home states (SCEN), plus two more: connected (on demand)
// and no free dock (a deck past the sixth in dock mode).
enum { FX_NONET, FX_FINDING, FX_CHOOSE, FX_OLD, FX_DOCK, FX_RECON, FX_CONNECTED, FX_NODOCK, FX_SCENS };
// Its slot states.
enum { FX_SLOT_SYNCED, FX_SLOT_OLDER, FX_SLOT_SYNCING };

// Action ids the fixtures give their buttons.
enum {
    FX_ACT_NETWORK = 1, FX_ACT_COMPANION, FX_ACT_HOTSPOT, FX_ACT_BOX, FX_ACT_SAT, FX_ACT_DOCK, FX_ACT_RETRY,
    FX_ACT_CHOOSE, FX_ACT_PICK, FX_ACT_AP, FX_ACT_QR, FX_ACT_SYNC, FX_ACT_DIM, FX_ACT_BRIGHT, FX_ACT_SOUND,
    FX_ACT_RESTART,
};

typedef struct {
    int scen;         // FX_*
    bool ap;          // the hotspot is on
    bool ap_open;     // ... without a password
    bool backup;      // Wi-Fi in use: the backup network
    bool eth;         // Ethernet up
    int slot;         // FX_SLOT_*
    int slot_pct;     // syncing: how far
    bool locked;      // a settings password is set
    bool close;       // home opened on demand: the Close key
    const char *toast;
} fx_box_t;

// The defaults: Waiting for Companion (dock), hotspot on, main Wi-Fi, slot in sync.
fx_box_t fx_box_default(void);

void fx_home(dui_screen_t *s, const fx_box_t *b);
void fx_menu(dui_screen_t *s, const fx_box_t *b);
void fx_network(dui_screen_t *s, const fx_box_t *b);
void fx_companion(dui_screen_t *s, const fx_box_t *b); // dock mode when b->scen == FX_DOCK, else satellite
void fx_hotspot(dui_screen_t *s, const fx_box_t *b);
void fx_qr(dui_screen_t *s, const fx_box_t *b);
void fx_box(dui_screen_t *s, const fx_box_t *b);

// The home screen's status for b (also the status of every screen: the indicators, the toast).
void fx_status(dui_screen_t *s, const fx_box_t *b);
