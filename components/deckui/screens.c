/* Pure: the maintenance UI's screens from a snapshot of the box (screens.h): home,
 * menu, network, companion, hotspot, qr and box. */
#include "deckui/screens.h"

#include "rpc/refusals.h"

#include <stdio.h>
#include <string.h>

/* ---- the box's state ---- */

/* The home screen's state: its status words and its main content. */
enum { H_NONET, H_FINDING, H_CHOOSE, H_OLD, H_RECON, H_CONNECTED, H_DOCK, H_NODOCK, H_STATES };

static const struct {
    uint8_t state;
    const char *words, *short_words, *text; /* the keys' words, the Mini's, the strip's (NULL: the words) */
    const char *word;                        /* one word for the Companion screen */
} HOME[H_STATES] = {
    [H_NONET] = {DUI_STATE_BAD, "No network", NULL, NULL, "No network"},
    [H_FINDING] = {DUI_STATE_WAIT, "Finding Companion", NULL, NULL, "Finding"},
    [H_CHOOSE] = {DUI_STATE_WAIT, "Choose Companion", NULL, NULL, "Choose"},
    [H_OLD] = {DUI_STATE_BAD, "Companion too old", "Too old", NULL, "Too old"},
    [H_RECON] = {DUI_STATE_WAIT, "Reconnecting", NULL, "Reconnecting", "Reconnecting"}, /* one word, whole */
    [H_CONNECTED] = {DUI_STATE_OK, "Connected", NULL, "Companion connected", "Connected"},
    [H_DOCK] = {DUI_STATE_WAIT, "Waiting for Companion", "Waiting", NULL, "Waiting"},
    [H_NODOCK] = {DUI_STATE_BAD, "No free dock", "No dock", NULL, "No dock"},
};

/* The box's address: Ethernet's when it has one (it takes the route), else the station's. */
static const char *box_ip(const dui_box_t *b) { return b->net.eth_ip[0] ? b->net.eth_ip : b->net.wifi_ip; }

static bool online(const dui_box_t *b)
{
    return box_ip(b)[0] && !(b->mode == DUI_MODE_SAT && b->sat.state == DUI_SAT_NONET);
}

static int home_state(const dui_box_t *b)
{
    if (!online(b)) return H_NONET;
    if (b->mode == DUI_MODE_DOCK) {
        if (b->dock.no_free) return H_NODOCK;
        return b->connected || b->dock.peer[0] ? H_CONNECTED : H_DOCK;
    }
    if (b->connected) return H_CONNECTED;
    switch (b->sat.state) {
    case DUI_SAT_CHOOSE: return H_CHOOSE;
    case DUI_SAT_TOO_OLD: return H_OLD;
    case DUI_SAT_RECON: return H_RECON;
    case DUI_SAT_CONNECTED: return H_CONNECTED;
    default: return H_FINDING;
    }
}

static int state_role(int state)
{
    return state == DUI_STATE_BAD ? DUI_ROLE_BAD : state == DUI_STATE_OK ? DUI_ROLE_OK : DUI_ROLE_WAIT;
}

static bool ap_open(const dui_box_t *b) { return !b->net.ap_secured || !b->net.ap_pass[0]; }

/* The recovery slot offers something to do: Sync now or Boot other (the Box menu button's amber dot). */
static bool slot_attention(const dui_box_t *b)
{
    const int s = b->box.slot;
    return s == DUI_SLOT_DIFFERS || s == DUI_SLOT_OTHER_NEWER || s == DUI_SLOT_EMPTY || s == DUI_SLOT_INVALID;
}

/* ---- parts every screen shares ---- */

/* the Wi-Fi sign: crossed in grey when off by choice, or with no network set up while Ethernet carries the
 * route (nothing wrong), its bars when joined (whichever carries the route), else crossed in red (on and not joined, or
 * no route at all: no network is a problem). */
static int wifi_sign(const dui_box_t *b)
{
    if (b->net.wifi_off) return DUI_SIGN_OFF;
    if (b->net.wifi_net != DUI_WIFI_NONE) return DUI_SIGN_UP;
    if (!b->net.wifi_main[0] && !b->net.wifi_backup[0] && !b->net.radio_down && online(b)) return DUI_SIGN_OFF;
    return DUI_SIGN_DOWN;
}

/* The home's status (every screen keeps it: the signs, the toast; a sub-screen adds its title). */
static void status(dui_screen_t *s, const dui_box_t *b, int h)
{
    dui_status_words(s, HOME[h].state, HOME[h].words, HOME[h].short_words, HOME[h].text);
    dui_status_t *st = &s->status;
    st->net = !online(b) ? DUI_NET_NONE : b->net.eth_ip[0] ? DUI_NET_ETH : DUI_NET_WIFI;
    st->wifi = (uint8_t)wifi_sign(b);
    st->rssi = st->wifi == DUI_SIGN_UP || st->net == DUI_NET_WIFI ? b->net.rssi : 0;
    st->eth_link = b->net.eth_link; /* its sign while the cable is linked */
    st->hotspot = b->net.ap_on;
    st->close = b->ondemand;
    st->toast = b->toast[0] ? dui_str(s, b->toast) : NULL;
    st->toast_state = b->toast_state != DUI_STATE_NONE ? b->toast_state : DUI_STATE_OK;
}

const char *dui_refusal(int code, const char *msg)
{
    static const struct {
        const char *in, *reason;
    } R[] = { /* the refusals' own words (rpc/refusals.h): the slot sync's and the uploads', the restart's, Wi-Fi's */
        {RPC_MSG_UPLOAD, "Upload running"},   {RPC_MSG_SYNCING, "Sync running"},      {RPC_MSG_CHECKING, "Still checking"},
        {RPC_MSG_SELF_CHECK, "Still checking"}, {RPC_MSG_NEWER, "Other newer"},       {RPC_MSG_EMPTY, "Slot empty"},
        {RPC_MSG_INVALID, "Not valid"},       {RPC_MSG_FOREIGN, "Not ours"},          {RPC_MSG_RESTARTING, "Restarting already"},
        {RPC_MSG_NO_BACKUP, "No backup"},     {RPC_MSG_WIFI_OFF, "Wi-Fi off"},        {RPC_MSG_NO_DECK, "No deck"},
        {RPC_MSG_NO_MEMORY, "No memory"},
    };
    /* the codes that say it all first, then the words, then the rest of the codes */
    if (code == RPC_E_UNAUTHORIZED) return "PIN needed";
    if (code == RPC_E_NOT_FOUND) return "Not found";
    for (size_t i = 0; msg && i < sizeof R / sizeof R[0]; i++)
        if (strstr(msg, R[i].in)) return R[i].reason;
    return code == RPC_E_INVALID_ARG ? "Not valid" : code == RPC_E_BAD_STATE ? "Busy now" : "Not done";
}

/* A sub-screen: the status, its title. */
static void sub_screen(dui_screen_t *s, const dui_box_t *b, const char *title)
{
    dui_screen_init(s);
    status(s, b, home_state(b));
    dui_title(s, title);
}

static void menu_buttons(dui_screen_t *s, const dui_box_t *b, bool as_group)
{
    static const struct { uint8_t icon; const char *label; uint16_t act; } M[4] = {
        {DUI_ICON_NET, "Network", DUI_A_GO_NETWORK},
        {DUI_ICON_COMPANION, "Companion", DUI_A_GO_COMPANION}, /* the webhooks logo */
        {DUI_ICON_HOTSPOT, "Hotspot", DUI_A_GO_HOTSPOT},
        {DUI_ICON_CIRCUITRY, "Box", DUI_A_GO_BOX}, /* Phosphor's Circuitry (the nine-dot logo is the web page's) */
    };
    const int dot[4] = {DUI_STATE_NONE, HOME[home_state(b)].state, b->net.ap_on ? DUI_STATE_OK : DUI_STATE_NONE,
                        slot_attention(b) ? DUI_STATE_WAIT : DUI_STATE_NONE};
    for (int i = 0; i < 4; i++) {
        if (as_group) dui_button(s, M[i].icon, M[i].label, M[i].act)->state = (uint8_t)dot[i];
        else dui_menu_button(s, M[i].icon, M[i].label, M[i].act, dot[i]);
    }
}

/* The hotspot's password on one key on every deck, as a Wi-Fi name's word is: whole whenever it reads at
 * 16 px on 1 line or 2 inside the key (the box's own, 10 characters, always does); over keys only past that. */
static void password_tile(dui_screen_t *s, const char *pass)
{
    dui_tile_t *t = dui_value(s, "Password", pass, DUI_ROLE_VALUE);
    if (t) t->flags |= DUI_F_WHOLE;
}

/* The hotspot's name and password, "Open" when it has none; "1 Join" before them (no network). The name's
 * caption says whose network it is (HOTSPOT; the station's networks are Main / Backup). */
static void hotspot_tiles(dui_screen_t *s, const dui_box_t *b, bool with_step)
{
    if (with_step) dui_step(s, 1, "Join", NULL);
    dui_words(s, "Hotspot", b->net.ap_ssid, DUI_ROLE_VALUE); /* one word a key */
    if (ap_open(b)) dui_word(s, "Open", DUI_ROLE_LABEL);
    else password_tile(s, b->net.ap_pass);
}

/* The hotspot's Wi-Fi join code (an open hotspot's without a password), and the setup page's URL code: the QR page's,
 * and the setup screen's. */
static void qr_join(dui_screen_t *s, const dui_box_t *b)
{
    char join[256];
    dui_wifi_join(join, sizeof join, b->net.ap_ssid, ap_open(b) ? NULL : b->net.ap_pass);
    dui_qr(s, join);
}

static void qr_url(dui_screen_t *s, const dui_box_t *b)
{
    char url[32];
    snprintf(url, sizeof url, "http://%s/", b->net.ap_ip);
    dui_qr(s, url);
}

static void port(dui_screen_t *s, uint16_t p)
{
    if (!p) return;
    char n[8];
    snprintf(n, sizeof n, "%u", (unsigned)p);
    dui_port(s, "Port", n);
}

bool dui_action_locked(int action)
{
    switch (action) {
    case DUI_A_MODE_SAT:
    case DUI_A_MODE_DOCK:
    case DUI_A_AP_OFF:
    case DUI_A_AP_ON: /* Always on changes a saved setting, as Turn off does */
    case DUI_A_RESTART:
    case DUI_A_SYNC:
    case DUI_A_BOOT_OTHER:
    case DUI_A_USE_MAIN: /* switching the Wi-Fi network drops it briefly */
    case DUI_A_USE_BACKUP:
    case DUI_A_ETH_DHCP: /* Ethernet's addressing restarts the network */
    case DUI_A_ETH_STATIC:
    case DUI_A_ETH_EDIT:
    case DUI_A_ETH_SAVE:
    case DUI_A_COMP_EDIT: /* the Companion's address, and auto find again */
    case DUI_A_COMP_SAVE:
    case DUI_A_COMP_AUTO:
    case DUI_A_WIFI_OFF: /* the station off, and on again */
    case DUI_A_WIFI_ON: return true;
    default: return false;
    }
}

bool dui_deck_locked(bool settings_locked, bool unlocked) { return settings_locked && !unlocked; }

bool dui_action_allowed(int action, bool secret_set, bool unlocked)
{
    return !dui_action_locked(action) || !secret_set || unlocked;
}

/* under the settings password a button whose action dui_action_locked names shows a lock and does
 * nothing. */
static dui_tile_t *lock(const dui_box_t *b, dui_tile_t *t)
{
    if (b->locked && dui_action_locked(t->action)) t->flags |= DUI_F_LOCKED;
    return t;
}

/* A hold button: its hold, its hint; danger; locked under the settings password. */
static dui_tile_t *hold_button(dui_screen_t *s, const dui_box_t *b, int icon, const char *label, uint16_t action,
                               uint16_t hold_ms, const char *hint, bool danger)
{
    dui_tile_t *t = dui_button(s, icon, label, action);
    t->hold_ms = hold_ms;
    t->hint = dui_str(s, hint);
    if (danger) t->flags |= DUI_F_DANGER;
    return lock(b, t);
}

/* ---- the pages ---- */

void dui_build_home(dui_screen_t *s, const dui_box_t *b)
{
    dui_screen_init(s);
    const int h = home_state(b);
    status(s, b, h);
    s->menu = true;
    menu_buttons(s, b, false);
    const dui_comp_t *target = &b->sat.target;
    switch (h) {
    case H_NONET:
        if (b->net.ap_on) { /* the hotspot is the main content: join it, open the setup page; each step's QR
                             * code after its words, for a camera (the words stay, for typing) */
            dui_group(s, DUI_ZONE_SVC, 0);
            hotspot_tiles(s, b, true);
            dui_group(s, DUI_ZONE_SVC, DUI_G_PACK); /* in the row after the words where it fits, else next (the Mini) */
            qr_join(s, b);
            dui_group(s, DUI_ZONE_MAIN, 0);
            dui_step(s, 2, "Open", NULL);
            dui_ipv4(s, "Setup page", b->net.ap_ip);
            dui_group(s, DUI_ZONE_MAIN, DUI_G_PACK);
            qr_url(s, b);
        } else {
            dui_group(s, DUI_ZONE_MAIN, 0); /* one sentence, one key ("Plug in" / "cable") */
            dui_word(s, "Plug in cable", DUI_ROLE_LABEL);
        }
        return;
    case H_DOCK:
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_step(s, 1, "Add", "remote surface");
        dui_ipv4(s, "Box IP", box_ip(b));
        port(s, b->dock.port);
        break;
    case H_NODOCK: { /* today's hint, one key ("Max 2" / "decks"), and what to do; the
                      * box's deck limit, 2 or 4 */
        char max[16];
        snprintf(max, sizeof max, "Max %u decks", (unsigned)(b->deck_limit ? b->deck_limit : 2));
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_word(s, max, DUI_ROLE_LABEL);
        dui_word(s, "Unplug one", DUI_ROLE_LABEL);
        break;
    }
    case H_FINDING:
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_ipv4(s, "Box IP", box_ip(b));
        dui_spin(s);
        dui_word(s, "Searching", DUI_ROLE_LABEL);
        break;
    case H_CHOOSE: /* one choice per Companion found; a tap pins it (as Companion.Choose) */
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_step(s, 1, "Tap", "your Companion");
        for (int i = 0; i < b->sat.ncomps && i < DUI_COMPS_MAX; i++) { /* no name: "Companion" over its IP */
            const dui_comp_t *c = &b->sat.comp[i];
            dui_choice(s, c->name[0] ? c->name : "Companion", c->ip, (uint16_t)(DUI_A_PICK0 + i));
        }
        break;
    case H_OLD: { /* the sentence in one key ("Needs 5.0+" / "has 4.2.0": "has", not "found", holds a
                   * 72 px key at 16 px), then which Companion */
        char why[40];
        snprintf(why, sizeof why, "Needs 5.0+ has %.12s", b->sat.version[0] ? b->sat.version : "?");
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_word(s, why, DUI_ROLE_LABEL);
        if (target->name[0]) dui_value(s, NULL, target->name, DUI_ROLE_VALUE);
        break;
    }
    case H_RECON:
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_spin(s);
        if (target->name[0] || target->ip[0]) {
            dui_word(s, "to", DUI_ROLE_LABEL);
            if (target->name[0]) dui_value(s, NULL, target->name, DUI_ROLE_VALUE);
            else dui_ipv4(s, NULL, target->ip);
        }
        break;
    case H_CONNECTED: /* on demand. "Connected Companion", a grey label (2 lines in its key), then the
                       * Companion's address (IP: octets, or two-octet pairs on the small decks); dock mode, its port after it.
                       * Where the label can't share a page with the address (the Mini in dock mode), no label: the
                       * address's first key says COMPANION IP (dui_lead) */
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_lead(s, "Connected Companion", "Companion IP");
        if (b->mode == DUI_MODE_DOCK) {
            if (b->dock.peer[0]) dui_ipv4(s, "IP", b->dock.peer);
            port(s, b->dock.port);
        } else if (target->ip[0]) {
            dui_ipv4(s, "IP", target->ip);
        } else if (target->name[0]) { /* no address known (not seen): its name */
            dui_value(s, NULL, target->name, DUI_ROLE_VALUE);
        }
        break;
    }
    if (b->net.ap_on) { /* the service zone, anchored at the bottom: wherever the hotspot shows, its code to join it */
        dui_group(s, DUI_ZONE_SVC, DUI_G_ANCHOR);
        hotspot_tiles(s, b, false);
        qr_join(s, b);
    }
}

void dui_build_menu(dui_screen_t *s, const dui_box_t *b)
{
    sub_screen(s, b, "Menu");
    s->full = true;
    dui_group(s, DUI_ZONE_MAIN, 0);
    menu_buttons(s, b, true);
}

/* The tiles added since `from`, placed at map (the Network screen's key map). */
static void place(dui_screen_t *s, int from, int row, int col)
{
    for (int i = from; i < s->ntiles; i++) s->tile[i].map = DUI_MAP(row, col);
}

/* A Wi-Fi network's row, at its fixed keys: the icon, the name, the state, Use, and while in
 * use its signal and address. The state: the one joined "In use" (green) with its signal and its own address whenever
 * it has one (also while Ethernet takes the route); the other "Standby". Neither joined: "Radio down" when the co-processor doesn't answer, "Not
 * found" only when the driver said so on its last attempt there, "Joining" while it is being tried, else "Standby"
 * (its turn hasn't come). A row not in use has a Use button (a 1.5 s hold, locked under the PIN), not while the radio
 * is down. A network not set up: "Not set", nothing else. Wi-Fi off: "Off". */
static void wifi_row(dui_screen_t *s, const dui_box_t *b, int which, const char *ssid)
{
    const int row = which == DUI_WIFI_MAIN ? DUI_MAP_MAIN : DUI_MAP_BACKUP;
    const bool on = !b->net.wifi_off && b->net.wifi_net == which && ssid[0];
    const bool not_found = b->net.wifi_not_found & (1u << (which - 1));
    dui_group(s, DUI_ZONE_MAIN, 0);
    int at = s->ntiles;
    dui_tile_t *t = dui_icon_tile(s, DUI_ICON_WIFI, which == DUI_WIFI_MAIN ? "Primary" : "Backup", !on);
    t->rssi = on ? b->net.rssi : 0;
    place(s, at, row, DUI_MC_ICON);
    if (ssid[0]) {
        at = s->ntiles;
        dui_value(s, NULL, ssid, DUI_ROLE_VALUE)->flags |= DUI_F_WHOLE; /* the map splits it to its keys, a word a key */
        place(s, at, row, DUI_MC_NAME);
    }
    at = s->ntiles;
    if (!ssid[0]) dui_word(s, "Not set", DUI_ROLE_LABEL);
    else if (b->net.wifi_off) dui_word(s, "Off", DUI_ROLE_LABEL); /* off by the settings: nothing to switch to */
    else if (on) dui_word(s, "In use", DUI_ROLE_OK);
    else if (b->net.wifi_net != DUI_WIFI_NONE) dui_word(s, "Standby", DUI_ROLE_LABEL);
    else if (b->net.radio_down) dui_word(s, "Radio down", DUI_ROLE_BAD);
    else if (not_found) dui_word(s, "Not found", DUI_ROLE_BAD);
    else if (b->net.wifi_trying == which) dui_word(s, "Joining", DUI_ROLE_WAIT);
    else dui_word(s, "Standby", DUI_ROLE_LABEL);
    place(s, at, row, DUI_MC_STATE);
    if (!ssid[0] || b->net.wifi_off) return;
    if (on) {
        char dbm[8];
        snprintf(dbm, sizeof dbm, "%d", b->net.rssi);
        at = s->ntiles;
        dui_cap(s, "Signal", dbm, DUI_ROLE_VALUE);
        place(s, at, row, DUI_MC_AUX);
        if (b->net.wifi_ip[0]) {
            at = s->ntiles;
            dui_ipv4(s, "IP", b->net.wifi_ip);
            place(s, at, row, DUI_MC_IP);
        }
        return;
    }
    if (!b->net.radio_down) {
        at = s->ntiles;
        hold_button(s, b, DUI_ICON_WIFI, "Use", which == DUI_WIFI_MAIN ? DUI_A_USE_MAIN : DUI_A_USE_BACKUP, DUI_HOLD_USE_MS,
                    "Wi-Fi blips", false);
        place(s, at, row, DUI_MC_ACTION);
    }
}

/* ---- Ethernet's static address ---- */

bool dui_ipv4_parse(const char *s, uint8_t out[4])
{
    if (!s) return false;
    int parts = 0;
    uint8_t o[4] = {0};
    for (;;) {
        if (*s < '0' || *s > '9') return false;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (++digits > 3 || v > 255) return false;
            s++;
        }
        if (parts == 4) return false;
        o[parts++] = (uint8_t)v;
        if (*s == '\0') break;
        if (*s != '.') return false;
        s++;
    }
    if (parts != 4) return false;
    if (out) memcpy(out, o, 4);
    return true;
}

static bool zero4(const uint8_t q[4]) { return !q[0] && !q[1] && !q[2] && !q[3]; }

bool dui_ipset_valid(const dui_ipset_t *s)
{
    uint8_t ip[4], mask[4];
    if (!s || !dui_ipv4_parse(s->ip, ip) || !dui_ipv4_parse(s->mask, mask) || !dui_ipv4_parse(s->gw, NULL)) return false;
    if (s->dns[0] && !dui_ipv4_parse(s->dns, NULL)) return false;
    return !zero4(ip) && !zero4(mask);
}

int dui_eth_static_pick(const dui_ipset_t *saved, const dui_ipset_t *lease, dui_ipset_t *out)
{
    dui_ipset_t t;
    memset(&t, 0, sizeof t);
    int from = DUI_ETH_FROM_NONE;
    if (saved && dui_ipset_valid(saved)) {
        t = *saved;
        from = DUI_ETH_FROM_SAVED;
    } else if (lease) {
        uint8_t ip[4], mask[4], dns[4];
        if (dui_ipv4_parse(lease->ip, ip) && !zero4(ip) && dui_ipv4_parse(lease->mask, mask) && !zero4(mask)) {
            t = *lease;
            if (!dui_ipv4_parse(t.gw, NULL)) snprintf(t.gw, sizeof t.gw, "0.0.0.0"); /* no router given */
            if (!dui_ipv4_parse(t.dns, dns) || zero4(dns)) t.dns[0] = '\0';
            from = DUI_ETH_FROM_LEASE;
        }
    }
    if (out) *out = t;
    return from;
}

/* The Ethernet row, at its fixed keys: the icon; ADDRESS DHCP / Static; its link; the switch
 * (a 1.5 s hold, locked under the PIN): Use DHCP when static, Make static when DHCP, or a disabled "Set on web page"
 * with no static set to use; Edit (the static-address editor, locked too); its address while linked with one (it
 * routes then). Each row's address has keys of its own in the map, so both show at once. */
static void eth_row(dui_screen_t *s, const dui_box_t *b)
{
    const int row = DUI_MAP_ETH;
    dui_group(s, DUI_ZONE_MAIN, 0);
    int at = s->ntiles;
    dui_icon_tile(s, DUI_ICON_ETH, "Ethernet", !b->net.eth_link);
    place(s, at, row, DUI_MC_ICON);
    at = s->ntiles;
    dui_cap(s, "Address", b->net.eth_dhcp ? "DHCP" : "Static", DUI_ROLE_VALUE);
    place(s, at, row, DUI_MC_NAME);
    at = s->ntiles;
    if (b->net.eth_link) dui_word(s, "Up", DUI_ROLE_OK);
    else dui_word(s, "No cable", DUI_ROLE_LABEL);
    place(s, at, row, DUI_MC_STATE);
    at = s->ntiles;
    if (!b->net.eth_dhcp)
        hold_button(s, b, DUI_ICON_ETH, "Use DHCP", DUI_A_ETH_DHCP, DUI_HOLD_ETH_MS, "net resets", false);
    else if (b->net.eth_static_ok) { /* it says which set it applies (an old saved one, or the lease) */
        dui_tile_t *t = hold_button(s, b, DUI_ICON_ETH, "Make static", DUI_A_ETH_STATIC, DUI_HOLD_ETH_MS, "net resets", false);
        t->caption = dui_str(s, b->net.eth_static_saved ? "saved" : "lease");
        t->flags |= DUI_F_TAG; /* a tag: the icon, the hold hint, the HOLD tag and the lock stay */
    }
    else
        dui_button(s, DUI_ICON_ETH, "Set on web page", DUI_A_NONE)->flags |= DUI_F_DIM; /* nothing to use */
    place(s, at, row, DUI_MC_ACTION);
    at = s->ntiles;
    lock(b, dui_button(s, DUI_ICON_EDIT, "Edit", DUI_A_ETH_EDIT));
    place(s, at, row, DUI_MC_AUX);
    if (b->net.eth_link && b->net.eth_ip[0]) {
        at = s->ntiles;
        dui_ipv4(s, "IP", b->net.eth_ip);
        place(s, at, row, DUI_MC_IP);
    }
}

/* a fixed key map per model (dui_plan): Ethernet, Main and Backup always in that order, each network's
 * keys in the same places in every state; the Wi-Fi on / off key always in one place; the box's name last. */
void dui_build_network(dui_screen_t *s, const dui_box_t *b)
{
    sub_screen(s, b, "Network");
    s->keymap = true;
    eth_row(s, b);
    wifi_row(s, b, DUI_WIFI_MAIN, b->net.wifi_main);
    wifi_row(s, b, DUI_WIFI_BACKUP, b->net.wifi_main[0] ? b->net.wifi_backup : ""); /* "Not set" when none */
    if (b->net.wifi_main[0]) { /* the station on / off: off is a 1.5 s hold, on a tap; both locked under the PIN */
        dui_group(s, DUI_ZONE_MAIN, 0);
        const int at = s->ntiles;
        if (b->net.wifi_off)
            lock(b, dui_button(s, DUI_ICON_POWER, "Wi-Fi", DUI_A_WIFI_ON));
        else
            hold_button(s, b, DUI_ICON_POWER, "Wi-Fi", DUI_A_WIFI_OFF, DUI_HOLD_WIFI_MS, "Wi-Fi off", false)->flags |=
                DUI_F_ON | DUI_F_SELECTED;
        place(s, at, DUI_MAP_MAIN, DUI_MC_TOGGLE);
    }
    dui_group(s, DUI_ZONE_MAIN, 0);
    const int at = s->ntiles;
    dui_value(s, "Name", b->net.name, DUI_ROLE_VALUE);
    dui_word(s, ".local", DUI_ROLE_LABEL);
    place(s, at, DUI_MAP_TOP, DUI_MC_LOCAL);
}

void dui_build_companion(dui_screen_t *s, const dui_box_t *b)
{
    sub_screen(s, b, "Companion");
    const int h = home_state(b);
    const bool dock = b->mode == DUI_MODE_DOCK;
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_word(s, "Mode", DUI_ROLE_LABEL);
    for (int m = 0; m < 2; m++) { /* Satellite, Dock: the current one selected, the other hold-to-switch */
        const bool is_dock = m == 1;
        const int icon = is_dock ? DUI_ICON_DOCK : DUI_ICON_SAT;
        const char *label = is_dock ? "Dock" : "Satellite";
        if (is_dock == dock)
            dui_button(s, icon, label, DUI_A_NONE)->flags |= DUI_F_SELECTED;
        else
            hold_button(s, b, icon, label, is_dock ? DUI_A_MODE_DOCK : DUI_A_MODE_SAT, DUI_HOLD_MODE_MS, "restarts", false);
    }
    dui_group(s, DUI_ZONE_MAIN, 0);
    if (dock) {
        port(s, b->dock.port);
        dui_word(s, HOME[h].word, state_role(HOME[h].state));
        return;
    }
    const dui_comp_t *target = &b->sat.target; /* captioned COMPANION (was TARGET) */
    if (target->name[0]) dui_value(s, "Companion", target->name, DUI_ROLE_VALUE);
    else dui_cap(s, "Companion", "None", DUI_ROLE_LABEL);
    if (target->ip[0]) dui_ipv4(s, "IP", target->ip);
    dui_word(s, HOME[h].word, state_role(HOME[h].state));
    /* Retry only while not connected; Choose only with auto-discovery, a list and no save; a manual
     * Companion: Auto find instead (locked); Set IP always (locked: the PIN pad first) */
    dui_group(s, DUI_ZONE_MAIN, 0);
    if (h != H_CONNECTED) dui_button(s, DUI_ICON_RETRY, "Retry now", DUI_A_RETRY);
    if (b->sat.auto_disc) dui_button(s, DUI_ICON_LIST, "Choose", DUI_A_GO_CHOOSE);
    else lock(b, dui_button(s, DUI_ICON_SEARCH, "Auto find", DUI_A_COMP_AUTO)); /* its own icon */
    lock(b, dui_button(s, DUI_ICON_EDIT, "Set IP", DUI_A_COMP_EDIT));
}

void dui_build_hotspot(dui_screen_t *s, const dui_box_t *b)
{
    sub_screen(s, b, "Hotspot");
    if (b->net.ap_on) {
        dui_group(s, DUI_ZONE_SVC, 0);
        dui_words(s, "Hotspot", b->net.ap_ssid, DUI_ROLE_VALUE); /* HOTSPOT (was WI-FI NAME) */
        dui_group(s, DUI_ZONE_SVC, 0);
        if (ap_open(b)) {
            dui_word(s, "No", DUI_ROLE_LABEL);
            dui_word(s, "password", DUI_ROLE_LABEL);
        } else {
            password_tile(s, b->net.ap_pass);
        }
        qr_join(s, b); /* the code to join it, after its password; Show QR adds the setup page's */
        dui_group(s, DUI_ZONE_MAIN, 0);
        hold_button(s, b, DUI_ICON_POWER, "Turn off", DUI_A_AP_OFF, DUI_HOLD_AP_OFF_MS, "cuts users", true)->flags |=
            DUI_F_ON;
        dui_button(s, DUI_ICON_QR, "Show QR", DUI_A_GO_QR);
    } else {
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_word(s, "Hotspot", DUI_ROLE_LABEL);
        dui_word(s, "is off", DUI_ROLE_LABEL);
        dui_group(s, DUI_ZONE_MAIN, 0);
        lock(b, dui_button(s, DUI_ICON_POWER, "Always on", DUI_A_AP_ON)); /* the setting on, kept; locked */
    }
}

void dui_build_qr(dui_screen_t *s, const dui_box_t *b)
{
    sub_screen(s, b, "Scan to join");
    if (!b->net.ap_on) { /* turned off meanwhile: nothing to join */
        dui_group(s, DUI_ZONE_MAIN, 0);
        dui_word(s, "Hotspot", DUI_ROLE_LABEL);
        dui_word(s, "is off", DUI_ROLE_LABEL);
        return;
    }
    dui_group(s, DUI_ZONE_MAIN, 0);
    qr_join(s, b);
    dui_step(s, 1, "Join", "the hotspot");
    dui_group(s, DUI_ZONE_MAIN, DUI_G_PACK);
    qr_url(s, b);
    dui_step(s, 2, "Open", "the setup page"); /* no "Point your phone camera at a code" row */
}

/* "59s", "14m", "2h 14m", "3d 5h". */
static void uptime(char *out, size_t n, uint32_t s)
{
    if (s < 60) snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
    else if (s < 86400) snprintf(out, n, "%uh %um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
    else snprintf(out, n, "%ud %uh", (unsigned)(s / 86400), (unsigned)(s % 86400 / 3600));
}

/* the Sync button, always there. "Sync now" while the other slot differs, is empty or invalid; in sync it says so
 * in its tag (the way Make static shows "saved", DUI_F_TAG) and a tap copies the running firmware again all the same
 * (Ota.Sync, slotsync_start(false)). Disabled (DUI_F_DIM, no action) with its reason in the tag while a copy runs
 * (its percentage), over a newer image ("newer": a forced copy stays on the web page), and while the firmware is
 * pending verify or while an upload holds the flash (flash_busy: "wait", not a "Not now" toast). Locked
 * under the settings PIN while it does something. */
static void sync_button(dui_screen_t *s, const dui_box_t *b)
{
    char pct[8];
    const char *tag = NULL;
    bool live = true;
    switch (b->box.slot) {
    case DUI_SLOT_SYNCING:
        snprintf(pct, sizeof pct, "%u%%", (unsigned)(b->box.pct > 100 ? 100 : b->box.pct));
        tag = pct;
        live = false;
        break;
    case DUI_SLOT_WAITING:
        tag = "wait";
        live = false;
        break;
    case DUI_SLOT_OTHER_NEWER:
        tag = "newer";
        live = false;
        break;
    case DUI_SLOT_IN_SYNC: tag = "In sync"; break;
    default: break; /* differs, empty, invalid: Sync now */
    }
    if ((b->box.pending || b->box.flash_busy) && b->box.slot != DUI_SLOT_SYNCING) { /* the running image not confirmed
                                                                                 * yet, or an upload holds the flash */
        tag = "wait";
        live = false;
    }
    dui_tile_t *t = dui_button(s, DUI_ICON_SYNC, tag ? "Sync" : "Sync now", live ? DUI_A_SYNC : DUI_A_NONE);
    if (tag) {
        t->caption = dui_str(s, tag);
        t->flags |= DUI_F_TAG;
    }
    if (live) lock(b, t);
    else t->flags |= DUI_F_DIM;
}

/* The other slot: its state in a word, and what it offers. Sync now (no force) when it differs,
 * is empty or invalid (another project's image too); Boot other only for a newer image,
 * and only one that is valid, ours, with the running image not pending verify. */
static void other_slot(dui_screen_t *s, const dui_box_t *b)
{
    const char *caption = b->box.running_b ? "Slot A" : "Slot B", *ver = b->box.other_version;
    char pct[8];
    switch (b->box.slot) {
    case DUI_SLOT_IN_SYNC: dui_cap(s, caption, "In sync", DUI_ROLE_OK); break;
    case DUI_SLOT_SYNCING:
        snprintf(pct, sizeof pct, "%u%%", (unsigned)(b->box.pct > 100 ? 100 : b->box.pct));
        dui_cap(s, caption, pct, DUI_ROLE_WAIT);
        break;
    case DUI_SLOT_DIFFERS: dui_cap(s, caption, ver[0] ? ver : "Older", DUI_ROLE_LABEL); break;
    case DUI_SLOT_OTHER_NEWER: dui_cap(s, caption, ver[0] ? ver : "Newer", DUI_ROLE_WAIT); break;
    case DUI_SLOT_EMPTY: dui_cap(s, caption, "Empty", DUI_ROLE_LABEL); break;
    case DUI_SLOT_INVALID: dui_cap(s, caption, "Invalid", DUI_ROLE_BAD); break;
    default: dui_cap(s, caption, "Waiting", DUI_ROLE_WAIT); break;
    }
    sync_button(s, b);
    if (b->box.slot == DUI_SLOT_OTHER_NEWER && b->box.other_valid && !b->box.other_foreign && !b->box.pending)
        hold_button(s, b, DUI_ICON_RESTART, "Boot other", DUI_A_BOOT_OTHER, DUI_HOLD_BOOT_MS, "restarts", true);
}

void dui_build_box(dui_screen_t *s, const dui_box_t *b)
{
    sub_screen(s, b, "Box");
    char up[16], bright[8];
    uptime(up, sizeof up, b->box.uptime_s);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_value(s, "Name", b->net.name, DUI_ROLE_VALUE);
    dui_cap(s, "Uptime", up, DUI_ROLE_VALUE);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_cap(s, "Version", b->box.version[0] ? b->box.version : "?", DUI_ROLE_VALUE);
    dui_cap(s, b->box.running_b ? "Slot B" : "Slot A", "Running", DUI_ROLE_VALUE);
    other_slot(s, b);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_button(s, DUI_ICON_SUN_LOW, "Dimmer", DUI_A_DIMMER);
    snprintf(bright, sizeof bright, "%u%%", (unsigned)b->brightness);
    dui_cap(s, "Bright", bright, DUI_ROLE_VALUE);
    dui_button(s, DUI_ICON_SUN_HIGH, "Brighter", DUI_A_BRIGHTER);
    dui_tile_t *t = dui_button(s, b->sound ? DUI_ICON_SOUND : DUI_ICON_MUTE, b->sound ? "Sound on" : "Sound off", DUI_A_SOUND);
    if (b->sound) t->flags |= DUI_F_SELECTED;
    /* ledFlash. One word, on one line on every key: the selected look says it is on */
    t = dui_button(s, DUI_ICON_FLASH, "Flashes", DUI_A_FLASH);
    if (b->flash) t->flags |= DUI_F_SELECTED;
    hold_button(s, b, DUI_ICON_RESTART, "Restart", DUI_A_RESTART, DUI_HOLD_RESTART_MS, "restarts", true);
}

/* Choose: the Companions found, the one it connects to selected; a tap pins one (DUI_A_PICK*, as
 * Companion.Choose). Opening it changes nothing. With a manual address there is nothing to pick. */
void dui_build_choose(dui_screen_t *s, const dui_box_t *b)
{
    sub_screen(s, b, "Choose");
    dui_group(s, DUI_ZONE_MAIN, 0);
    if (!b->sat.auto_disc) {
        dui_word(s, "Manual", DUI_ROLE_LABEL);
        dui_word(s, "address", DUI_ROLE_LABEL);
        return;
    }
    if (!b->sat.ncomps) {
        dui_spin(s);
        dui_word(s, "Searching", DUI_ROLE_LABEL);
        return;
    }
    dui_step(s, 1, "Tap", "your Companion");
    const dui_comp_t *target = &b->sat.target;
    for (int i = 0; i < b->sat.ncomps && i < DUI_COMPS_MAX; i++) {
        const dui_comp_t *c = &b->sat.comp[i];
        dui_tile_t *t = dui_choice(s, c->name[0] ? c->name : "Companion", c->ip, (uint16_t)(DUI_A_PICK0 + i));
        if (target->ip[0] && !strcmp(target->ip, c->ip)) t->flags |= DUI_F_SELECTED;
    }
}

void dui_build(dui_screen_t *s, int page, const dui_box_t *b)
{
    switch (page) {
    case DUI_PAGE_MENU: dui_build_menu(s, b); break;
    case DUI_PAGE_NETWORK: dui_build_network(s, b); break;
    case DUI_PAGE_COMPANION: dui_build_companion(s, b); break;
    case DUI_PAGE_HOTSPOT: dui_build_hotspot(s, b); break;
    case DUI_PAGE_QR: dui_build_qr(s, b); break;
    case DUI_PAGE_BOX: dui_build_box(s, b); break;
    case DUI_PAGE_CHOOSE: dui_build_choose(s, b); break;
    default: dui_build_home(s, b); break;
    }
}
