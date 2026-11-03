/* The deck maintenance UI's reference screens as test data (deckui_fixtures.h). */
#include "deckui_fixtures.h"

#include <stdio.h>
#include <string.h>

/* The simulated box (S). */
static const char *IP = "192.168.8.225", *AP_IP = "192.168.4.1", *AP_SSID = "CompanionBox-8D1C",
                  *AP_PASS = "k7m2p9xq", *NAME = "CompanionBox-8D1C", *VER = "0.1.0", *TARGET = "Studio-Mac",
                  *TARGET_IP = "192.168.8.155";
static const struct { const char *ssid; int rssi; } MAIN = {"Simbox-Home", -58}, BACKUP = {"Office-5G", -66};
static const struct { const char *name, *ip; } COMPS[] = {{"Studio-Mac", "192.168.8.155"}, {"FOH-PC", "192.168.8.40"}};

fx_box_t fx_box_default(void)
{
    return (fx_box_t){.scen = FX_DOCK, .ap = true, .slot = FX_SLOT_SYNCED};
}

static bool online(const fx_box_t *b) { return b->scen != FX_NONET; }

static int scen_state(int scen)
{
    switch (scen) {
    case FX_NONET:
    case FX_OLD:
    case FX_NODOCK: return DUI_STATE_BAD;
    case FX_CONNECTED: return DUI_STATE_OK;
    default: return DUI_STATE_WAIT;
    }
}

static int state_role(int state)
{
    return state == DUI_STATE_BAD ? DUI_ROLE_BAD : state == DUI_STATE_OK ? DUI_ROLE_OK : DUI_ROLE_WAIT;
}

void fx_status(dui_screen_t *s, const fx_box_t *b)
{
    static const struct { const char *words, *short_words, *text; } W[FX_SCENS] = {
        [FX_NONET] = {"No network", NULL, NULL},
        [FX_FINDING] = {"Finding Companion", NULL, NULL},
        [FX_CHOOSE] = {"Choose Companion", NULL, NULL},
        [FX_OLD] = {"Companion too old", "Too old", NULL},
        [FX_DOCK] = {"Waiting for Companion", "Waiting", NULL},
        [FX_RECON] = {"Reconnecting", NULL, "Reconnecting"},
        [FX_CONNECTED] = {"Connected", NULL, "Companion connected"},
        [FX_NODOCK] = {"No free dock", "No dock", NULL},
    };
    dui_status_words(s, scen_state(b->scen), W[b->scen].words, W[b->scen].short_words, W[b->scen].text);
    dui_status_t *st = &s->status;
    st->net = !online(b) ? DUI_NET_NONE : b->eth ? DUI_NET_ETH : DUI_NET_WIFI;
    st->rssi = (int8_t)(b->backup ? BACKUP.rssi : MAIN.rssi);
    st->wifi = online(b) ? DUI_SIGN_UP : DUI_SIGN_DOWN; /* joined when online (Ethernet: on standby) */
    st->eth_link = b->eth;
    st->hotspot = b->ap;
    st->close = b->close;
    st->toast = b->toast ? dui_str(s, b->toast) : NULL;
}

static void menu_buttons(dui_screen_t *s, const fx_box_t *b, bool as_group)
{
    static const struct { int icon; const char *label; uint16_t act; } M[4] = {
        {DUI_ICON_NET, "Network", FX_ACT_NETWORK},
        {DUI_ICON_COMPANION, "Companion", FX_ACT_COMPANION},
        {DUI_ICON_HOTSPOT, "Hotspot", FX_ACT_HOTSPOT},
        {DUI_ICON_INFO, "Box", FX_ACT_BOX},
    };
    const int dot[4] = {DUI_STATE_NONE, scen_state(b->scen), b->ap ? DUI_STATE_OK : DUI_STATE_NONE,
                        b->slot == FX_SLOT_OLDER ? DUI_STATE_WAIT : DUI_STATE_NONE};
    for (int i = 0; i < 4; i++) {
        if (as_group) dui_button(s, M[i].icon, M[i].label, M[i].act)->state = (uint8_t)dot[i];
        else dui_menu_button(s, M[i].icon, M[i].label, M[i].act, dot[i]);
    }
}

/* The hotspot's name and password: "1 Join" before them on the no-network home. */
static void hotspot_tiles(dui_screen_t *s, const fx_box_t *b, bool with_step)
{
    if (with_step) dui_step(s, 1, "Join", NULL);
    dui_value(s, "Hotspot", AP_SSID, DUI_ROLE_VALUE);
    if (b->ap_open) dui_word(s, "Open", DUI_ROLE_LABEL);
    else dui_value(s, "Password", AP_PASS, DUI_ROLE_VALUE)->flags |= DUI_F_WHOLE;
}

void fx_home(dui_screen_t *s, const fx_box_t *b)
{
    dui_screen_init(s);
    fx_status(s, b);
    s->menu = true;
    menu_buttons(s, b, false);
    switch (b->scen) {
    case FX_NONET:
        if (b->ap) {
            dui_group(s, DUI_ZONE_SVC, 0);
            hotspot_tiles(s, b, true);
            dui_group(s, DUI_ZONE_MAIN, 0);
            dui_step(s, 2, "Open", NULL);
            dui_ipv4(s, "Setup page", AP_IP);
        } else {
            dui_group(s, DUI_ZONE_MAIN, 0);
            dui_word(s, "Plug in", DUI_ROLE_LABEL);
            dui_word(s, "network", DUI_ROLE_LABEL);
            dui_word(s, "cable", DUI_ROLE_LABEL);
        }
        return; /* the hotspot is the main content */
    case FX_DOCK:
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_step(s, 1, "Add", "remote surface");
        dui_ipv4(s, "Box IP", IP);
        dui_port(s, "Port", "5343");
        break;
    case FX_FINDING:
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_ipv4(s, "Box IP", IP);
        dui_spin(s);
        dui_word(s, "Searching", DUI_ROLE_LABEL);
        break;
    case FX_CHOOSE:
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_step(s, 1, "Tap", "your Companion");
        for (size_t i = 0; i < sizeof COMPS / sizeof COMPS[0]; i++)
            dui_choice(s, COMPS[i].name, COMPS[i].ip, FX_ACT_PICK);
        break;
    case FX_OLD:
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_word(s, "Needs", DUI_ROLE_LABEL);
        dui_word(s, "5.0+", DUI_ROLE_VALUE);
        dui_word(s, "found", DUI_ROLE_LABEL);
        dui_value(s, NULL, "FOH-PC", DUI_ROLE_VALUE);
        dui_word(s, "4.2.0", DUI_ROLE_VALUE);
        break;
    case FX_RECON:
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_spin(s);
        dui_word(s, "to", DUI_ROLE_LABEL);
        dui_value(s, NULL, TARGET, DUI_ROLE_VALUE);
        break;
    case FX_CONNECTED: /* "Connected Companion" (a grey label), the Companion's address */
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_lead(s, "Connected Companion", "Companion IP");
        dui_ipv4(s, "IP", TARGET_IP);
        break;
    case FX_NODOCK: /* today's hint (statusui's "Max 6 decks") */
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_word(s, "Max", DUI_ROLE_LABEL);
        dui_word(s, "6", DUI_ROLE_LABEL);
        dui_word(s, "decks", DUI_ROLE_LABEL);
        break;
    }
    if (b->ap) { /* the service zone, anchored at the bottom */
        dui_group(s, DUI_ZONE_SVC, DUI_G_ANCHOR);
        hotspot_tiles(s, b, false);
    }
}

void fx_menu(dui_screen_t *s, const fx_box_t *b)
{
    dui_screen_init(s);
    fx_status(s, b);
    dui_title(s, "Menu");
    s->full = true;
    dui_group(s, DUI_ZONE_MAIN, 0);
    menu_buttons(s, b, true);
}

static void wifi_row(dui_screen_t *s, const fx_box_t *b, bool backup)
{
    const char *ssid = backup ? BACKUP.ssid : MAIN.ssid;
    const int rssi = backup ? BACKUP.rssi : MAIN.rssi;
    const bool on = online(b) && !b->eth && b->backup == backup;
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_tile_t *t = dui_icon_tile(s, DUI_ICON_WIFI, backup ? "Backup" : "Main", !on);
    t->rssi = (int8_t)rssi;
    dui_value(s, NULL, ssid, DUI_ROLE_VALUE);
    if (on) {
        char dbm[8];
        snprintf(dbm, sizeof dbm, "%d", rssi);
        dui_cap(s, "Signal", dbm, DUI_ROLE_VALUE);
        dui_ipv4(s, "IP", IP);
    } else if (!online(b)) {
        dui_word(s, "Not found", DUI_ROLE_BAD);
    } else {
        dui_word(s, "Standby", DUI_ROLE_LABEL);
    }
}

void fx_network(dui_screen_t *s, const fx_box_t *b)
{
    dui_screen_init(s);
    fx_status(s, b);
    dui_title(s, "Network");
    dui_group(s, DUI_ZONE_MAIN, 0);
    if (b->eth) {
        dui_icon_tile(s, DUI_ICON_ETH, "Ethernet", false);
        dui_word(s, "Up", DUI_ROLE_OK);
        dui_ipv4(s, "IP", IP);
    } else {
        dui_icon_tile(s, DUI_ICON_ETH, "Ethernet", true);
        dui_word(s, "No cable", DUI_ROLE_LABEL);
    }
    wifi_row(s, b, false);
    wifi_row(s, b, true);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_value(s, "Name", NAME, DUI_ROLE_VALUE);
    dui_word(s, ".local", DUI_ROLE_LABEL);
}

void fx_companion(dui_screen_t *s, const fx_box_t *b)
{
    dui_screen_init(s);
    fx_status(s, b);
    dui_title(s, "Companion");
    const bool dock = b->scen == FX_DOCK || b->scen == FX_NODOCK; /* here dock mode is the dock state */
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_word(s, "Mode", DUI_ROLE_LABEL);
    for (int m = 0; m < 2; m++) { /* Satellite, Dock: the other one is hold-to-switch, "restarts" */
        const bool is_dock = m == 1, sel = is_dock == dock;
        dui_tile_t *t = dui_button(s, is_dock ? DUI_ICON_DOCK : DUI_ICON_SAT, is_dock ? "Dock" : "Satellite",
                                   is_dock ? FX_ACT_DOCK : FX_ACT_SAT);
        if (sel) t->flags |= DUI_F_SELECTED;
        else {
            t->hold_ms = 1500;
            t->hint = dui_str(s, "restarts"); /* "Hold 1.5 s" / "restarts" */
            if (b->locked) t->flags |= DUI_F_LOCKED;
        }
    }
    dui_group(s, DUI_ZONE_MAIN, 0);
    if (dock) {
        dui_port(s, "Port", "5343");
        if (b->scen == FX_NODOCK) dui_word(s, "No dock", DUI_ROLE_BAD);
        else dui_word(s, "Waiting", DUI_ROLE_WAIT);
    } else {
        static const char *FIRST[FX_SCENS] = {"No", "Finding", "Choose", "Companion", "Waiting", "Reconnecting", "Connected", "No"};
        dui_value(s, "Companion", TARGET, DUI_ROLE_VALUE);
        dui_ipv4(s, "IP", TARGET_IP);
        dui_word(s, FIRST[b->scen], state_role(scen_state(b->scen)));
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_button(s, DUI_ICON_RETRY, "Retry now", FX_ACT_RETRY);
        dui_button(s, DUI_ICON_LIST, "Choose", FX_ACT_CHOOSE);
    }
}

void fx_hotspot(dui_screen_t *s, const fx_box_t *b)
{
    dui_screen_init(s);
    fx_status(s, b);
    dui_title(s, "Hotspot");
    if (b->ap) {
        dui_group(s, DUI_ZONE_SVC, 0);
        dui_value(s, "Hotspot", AP_SSID, DUI_ROLE_VALUE);
        dui_group(s, DUI_ZONE_SVC, 0);
        if (b->ap_open) {
            dui_word(s, "No", DUI_ROLE_LABEL);
            dui_word(s, "password", DUI_ROLE_LABEL);
        } else {
            dui_value(s, "Password", AP_PASS, DUI_ROLE_VALUE)->flags |= DUI_F_WHOLE;
        }
    } else {
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_word(s, "Hotspot", DUI_ROLE_LABEL);
        dui_word(s, "is off", DUI_ROLE_LABEL);
    }
    dui_group(s, DUI_ZONE_MAIN, 0);
    if (b->ap) {
        dui_tile_t *t = dui_button(s, DUI_ICON_POWER, "Turn off", FX_ACT_AP);
        t->flags |= DUI_F_DANGER | DUI_F_ON | (b->locked ? DUI_F_LOCKED : 0);
        t->hold_ms = 1500;
        t->hint = dui_str(s, "cuts users");
        dui_button(s, DUI_ICON_QR, "Show QR", FX_ACT_QR);
    } else {
        dui_button(s, DUI_ICON_POWER, "Always on", FX_ACT_AP);
    }
}

void fx_qr(dui_screen_t *s, const fx_box_t *b)
{
    dui_screen_init(s);
    fx_status(s, b);
    dui_title(s, "Scan to join");
    char join[128], url[40];
    dui_wifi_join(join, sizeof join, AP_SSID, b->ap_open ? NULL : AP_PASS);
    snprintf(url, sizeof url, "http://%s/", AP_IP);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_qr(s, join);
    dui_step(s, 1, "Join", "the hotspot");
    dui_group(s, DUI_ZONE_MAIN, DUI_G_PACK);
    dui_qr(s, url);
    dui_step(s, 2, "Open", "the setup page");
    dui_group(s, DUI_ZONE_NONE, 0);
    static const char *POINT[] = {"Point", "your", "phone", "camera", "at", "a code"};
    for (size_t i = 0; i < sizeof POINT / sizeof POINT[0]; i++) dui_word(s, POINT[i], DUI_ROLE_LABEL);
}

void fx_box(dui_screen_t *s, const fx_box_t *b)
{
    dui_screen_init(s);
    fx_status(s, b);
    dui_title(s, "Box");
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_value(s, "Name", NAME, DUI_ROLE_VALUE);
    dui_cap(s, "Uptime", "2h 14m", DUI_ROLE_VALUE);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_cap(s, "Version", VER, DUI_ROLE_VALUE);
    dui_cap(s, "Slot A", "Running", DUI_ROLE_VALUE);
    if (b->slot == FX_SLOT_SYNCED) {
        dui_cap(s, "Slot B", "In sync", DUI_ROLE_OK);
    } else if (b->slot == FX_SLOT_SYNCING) {
        char pct[8];
        snprintf(pct, sizeof pct, "%d%%", b->slot_pct);
        dui_cap(s, "Slot B", pct, DUI_ROLE_WAIT);
    } else {
        dui_cap(s, "Slot B", "0.0.9", DUI_ROLE_LABEL);
        dui_button(s, DUI_ICON_SYNC, "Sync now", FX_ACT_SYNC)->flags |= b->locked ? DUI_F_LOCKED : 0;
    }
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_button(s, DUI_ICON_SUN_LOW, "Dimmer", FX_ACT_DIM);
    dui_cap(s, "Bright", "70%", DUI_ROLE_VALUE);
    dui_button(s, DUI_ICON_SUN_HIGH, "Brighter", FX_ACT_BRIGHT);
    dui_button(s, DUI_ICON_SOUND, "Sound on", FX_ACT_SOUND)->flags |= DUI_F_SELECTED;
    dui_tile_t *t = dui_button(s, DUI_ICON_RESTART, "Restart", FX_ACT_RESTART);
    t->flags |= DUI_F_DANGER | (b->locked ? DUI_F_LOCKED : 0);
    t->hold_ms = 2000;
    t->hint = dui_str(s, "restarts");
}
