/* The maintenance UI's glue in surfaces (components/surfaces/maintmap.c): the snapshot the deck's screens
 * show from netmgr / slotsync / the Satellite client / discovery / the settings, who gets a deck's input
 * in each state (never Companion while the maintenance screen is on demand), and deck input as
 * the navigation's events. */
#include <string.h>
#include "unity.h"
#include "deckmodel/model.h"
#include "deckui/screens.h"
#include "maintmap.h"

void setUp(void) {}
void tearDown(void) {}

static netmgr_status_t NS;
static slotsync_status_t SS;
static companion_instance_t COMPS[3];

/* A typical box: dock mode, Ethernet routed, the main Wi-Fi joined too, the hotspot on. */
static surf_maint_in_t in_default(void)
{
    memset(&NS, 0, sizeof NS);
    NS.route = NET_ROUTE_ETH;
    strcpy(NS.ip, "192.168.8.225");
    strcpy(NS.eth_addr, "192.168.8.225"); /* each connection's own address */
    strcpy(NS.sta_addr, "192.168.8.226");
    NS.eth_link = NS.eth_ip = true;
    NS.wifi_connected = true;
    NS.wifi_enabled = true;
    NS.sta_net = NETMGR_STA_MAIN;
    strcpy(NS.sta_ssid, "Simbox-Home");
    NS.sta_rssi = -58;
    NS.ap_on = true;
    strcpy(NS.ap_ssid, "CompanionBox-8D1C");
    strcpy(NS.ap_ip, "192.168.4.1");
    memset(&SS, 0, sizeof SS);
    SS.state = SLOTSYNC_IN_SYNC;
    SS.pct = 100;
    strcpy(SS.running, "ota_0");
    strcpy(SS.other, "ota_1");
    strcpy(SS.other_version, "0.1.0");
    SS.other_valid = true;
    memset(COMPS, 0, sizeof COMPS);
    strcpy(COMPS[0].id, "id-a");
    strcpy(COMPS[0].host, "192.168.8.155");
    strcpy(COMPS[0].name, "Studio-Mac");
    strcpy(COMPS[0].version, "5.1.1");
    strcpy(COMPS[1].id, "id-b");
    strcpy(COMPS[1].host, "192.168.8.40");
    return (surf_maint_in_t){
        .dock = true, .ns = &NS, .ss = &SS, .box_name = "CompanionBox-8D1C", .wifi_main = "Simbox-Home",
        .wifi_backup = "Office-5G", .ap_shown = true, .ap_open = false, .ap_ssid = "", .ap_pass = "k7m2p9xq",
        .brightness = 70, .sound = true, .version = "0.1.0", .uptime_s = 123, .sel = SEL_NONE,
        .target_host = "", .companion_version = "", .comps = COMPS, .ncomps = 2, .dock_port = 5343,
        .dock_peer = "", .toast = "",
    };
}

static dui_box_t B;

static void map(const surf_maint_in_t *in) { surf_maint_box(&B, in); }

static void test_network_and_settings(void)
{
    surf_maint_in_t in = in_default();
    map(&in);
    TEST_ASSERT_EQUAL(DUI_MODE_DOCK, B.mode);
    TEST_ASSERT_TRUE(B.net.eth_link);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225", B.net.eth_ip);
    TEST_ASSERT_EQUAL_STRING("192.168.8.226", B.net.wifi_ip); /* the joined Wi-Fi's own, behind Ethernet */
    TEST_ASSERT_EQUAL(DUI_WIFI_MAIN, B.net.wifi_net);
    TEST_ASSERT_EQUAL(-58, B.net.rssi);
    TEST_ASSERT_EQUAL_STRING("Simbox-Home", B.net.wifi_main);
    TEST_ASSERT_EQUAL_STRING("Office-5G", B.net.wifi_backup);
    TEST_ASSERT_TRUE(B.net.ap_on);
    TEST_ASSERT_EQUAL_STRING("CompanionBox-8D1C", B.net.ap_ssid); /* netmgr's when surfaces has none */
    TEST_ASSERT_EQUAL_STRING("k7m2p9xq", B.net.ap_pass);
    TEST_ASSERT_TRUE(B.net.ap_secured);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", B.net.ap_ip);
    TEST_ASSERT_EQUAL_STRING("CompanionBox-8D1C", B.net.name);
    TEST_ASSERT_EQUAL(70, B.brightness);
    TEST_ASSERT_TRUE(B.sound);
    TEST_ASSERT_FALSE(B.locked);
    TEST_ASSERT_EQUAL(5343, B.dock.port);
    TEST_ASSERT_FALSE(B.dock.no_free);
    /* Ethernet's own only while linked (an address kept a moment after the cable is pulled: none) */
    NS.eth_link = false;
    map(&in);
    TEST_ASSERT_EQUAL_STRING("", B.net.eth_ip);
    /* Wi-Fi routed: its address is the station's */
    NS.route = NET_ROUTE_WIFI;
    NS.eth_link = NS.eth_ip = false;
    NS.eth_addr[0] = '\0';
    strcpy(NS.sta_addr, "192.168.8.225");
    NS.sta_net = NETMGR_STA_BACKUP;
    map(&in);
    TEST_ASSERT_EQUAL_STRING("", B.net.eth_ip);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225", B.net.wifi_ip);
    /* the cable linked, no address yet: none shown for it */
    NS.eth_link = true;
    map(&in);
    TEST_ASSERT_EQUAL_STRING("", B.net.eth_ip);
    NS.eth_link = false;
    TEST_ASSERT_EQUAL(DUI_WIFI_BACKUP, B.net.wifi_net);
    /* the network being tried, what the driver found, the radio */
    NS.sta_net = NETMGR_STA_NONE;
    NS.sta_trying = NETMGR_STA_BACKUP;
    NS.sta_not_found = 1u << (NETMGR_STA_MAIN - 1);
    NS.radio_down = true;
    map(&in);
    TEST_ASSERT_EQUAL(DUI_WIFI_BACKUP, B.net.wifi_trying);
    TEST_ASSERT_EQUAL(1u << (DUI_WIFI_MAIN - 1), B.net.wifi_not_found);
    TEST_ASSERT_TRUE(B.net.radio_down);
    NS.sta_net = NETMGR_STA_BACKUP;
    NS.sta_trying = NETMGR_STA_NONE;
    NS.sta_not_found = 0;
    NS.radio_down = false;
    /* auto-discovery as the settings say */
    TEST_ASSERT_FALSE(B.sat.auto_disc);
    in.companion_auto = true;
    map(&in);
    TEST_ASSERT_TRUE(B.sat.auto_disc);
    in.companion_auto = false;
    /* the hotspot off, or not given to surfaces: not shown; open */
    in.ap_open = true;
    in.ap_ssid = "My-Box";
    map(&in);
    TEST_ASSERT_FALSE(B.net.ap_secured);
    TEST_ASSERT_EQUAL_STRING("My-Box", B.net.ap_ssid);
    in.ap_shown = false;
    map(&in);
    TEST_ASSERT_FALSE(B.net.ap_on);
    in.ap_shown = true;
    NS.ap_on = false;
    map(&in);
    TEST_ASSERT_FALSE(B.net.ap_on);
    /* no free dock; the peer; on demand, locked, the toast */
    in.dock_port = 0;
    in.connected = in.ondemand = in.locked = true;
    in.dock_peer = "192.168.8.155";
    in.toast = "Restarting";
    in.deck_limit = 4; /* the box's deck limit, for "Max N decks" */
    map(&in);
    TEST_ASSERT_EQUAL(4, B.deck_limit);
    TEST_ASSERT_TRUE(B.dock.no_free);
    TEST_ASSERT_EQUAL_STRING("192.168.8.155", B.dock.peer);
    TEST_ASSERT_TRUE(B.connected && B.ondemand && B.locked);
    TEST_ASSERT_EQUAL_STRING("Restarting", B.toast);
    /* NULL strings are "" */
    in.box_name = in.wifi_main = in.wifi_backup = in.ap_pass = in.toast = NULL;
    map(&in);
    TEST_ASSERT_EQUAL_STRING("", B.net.name);
    TEST_ASSERT_EQUAL_STRING("", B.toast);
}

/* The recovery slot: slotsync's state as it is (same order), the second slot running. */
/* Ethernet's addressing as the settings say, and whether a static set is there to switch to. */
static void test_eth_addressing(void)
{
    surf_maint_in_t in = in_default();
    in.eth_dhcp = true;
    map(&in); /* nothing saved, no lease given: nothing to make static from */
    TEST_ASSERT_TRUE(B.net.eth_dhcp);
    TEST_ASSERT_FALSE(B.net.eth_static_ok);
    const dui_ipset_t lease = {"192.168.8.225", "255.255.255.0", "192.168.8.1", "192.168.8.1"};
    in.eth_lease = &lease;
    map(&in);
    TEST_ASSERT_TRUE(B.net.eth_static_ok); /* the lease */
    const dui_ipset_t saved = {"10.0.0.5", "255.0.0.0", "10.0.0.1", ""}, none = {"", "", "", ""};
    in.eth_lease = &none;
    in.eth_saved = &saved;
    map(&in);
    TEST_ASSERT_TRUE(B.net.eth_static_ok); /* the saved set */
    in.eth_dhcp = false;
    map(&in);
    TEST_ASSERT_FALSE(B.net.eth_dhcp);
}

/* the station off by the settings shows as Wi-Fi off in the snapshot. */
static void test_wifi_off(void)
{
    surf_maint_in_t in = in_default();
    map(&in);
    TEST_ASSERT_FALSE(B.net.wifi_off);
    NS.wifi_enabled = false;
    NS.wifi_connected = false;
    NS.sta_net = NETMGR_STA_NONE;
    map(&in);
    TEST_ASSERT_TRUE(B.net.wifi_off);
    TEST_ASSERT_EQUAL(DUI_WIFI_NONE, B.net.wifi_net);
}

static void test_slot(void)
{
    surf_maint_in_t in = in_default();
    static const int STATES[][2] = {
        {SLOTSYNC_IN_SYNC, DUI_SLOT_IN_SYNC},     {SLOTSYNC_SYNCING, DUI_SLOT_SYNCING},
        {SLOTSYNC_DIFFERS, DUI_SLOT_DIFFERS},     {SLOTSYNC_OTHER_NEWER, DUI_SLOT_OTHER_NEWER},
        {SLOTSYNC_EMPTY, DUI_SLOT_EMPTY},         {SLOTSYNC_INVALID, DUI_SLOT_INVALID},
        {SLOTSYNC_WAITING, DUI_SLOT_WAITING},
    };
    for (size_t i = 0; i < sizeof STATES / sizeof STATES[0]; i++) {
        SS.state = (slotsync_state_t)STATES[i][0];
        map(&in);
        TEST_ASSERT_EQUAL(STATES[i][1], B.box.slot);
    }
    SS.state = SLOTSYNC_SYNCING;
    SS.pct = 45;
    SS.other_foreign = true;
    SS.other_valid = false;
    strcpy(SS.running, "ota_1");
    in.pending = true;
    map(&in);
    TEST_ASSERT_EQUAL(45, B.box.pct);
    TEST_ASSERT_TRUE(B.box.other_foreign);
    TEST_ASSERT_FALSE(B.box.other_valid);
    TEST_ASSERT_TRUE(B.box.running_b);
    TEST_ASSERT_TRUE(B.box.pending);
    TEST_ASSERT_EQUAL_STRING("0.1.0", B.box.other_version);
    TEST_ASSERT_EQUAL_STRING("0.1.0", B.box.version);
    TEST_ASSERT_EQUAL(123, B.box.uptime_s);
    in.ss = NULL; /* not known yet: waiting */
    map(&in);
    TEST_ASSERT_EQUAL(DUI_SLOT_WAITING, B.box.slot);
}

/* Satellite mode: the state in today's order (no route, choose, too old, connected, connecting to a
 * target, finding); the Companions found with their names; the target's name from them. */
static void test_satellite(void)
{
    surf_maint_in_t in = in_default();
    in.dock = false;
    map(&in);
    TEST_ASSERT_EQUAL(DUI_MODE_SAT, B.mode);
    TEST_ASSERT_EQUAL(DUI_SAT_FINDING, B.sat.state);
    TEST_ASSERT_EQUAL(2, B.sat.ncomps);
    TEST_ASSERT_EQUAL_STRING("Studio-Mac", B.sat.comp[0].name);
    TEST_ASSERT_EQUAL_STRING("192.168.8.155", B.sat.comp[0].ip);
    TEST_ASSERT_EQUAL_STRING("", B.sat.comp[1].name); /* no instance name: the screen says "Companion" */
    TEST_ASSERT_EQUAL_STRING("192.168.8.40", B.sat.comp[1].ip);
    in.target_host = "192.168.8.155";
    map(&in);
    TEST_ASSERT_EQUAL(DUI_SAT_RECON, B.sat.state);
    TEST_ASSERT_EQUAL_STRING("Studio-Mac", B.sat.target.name);
    TEST_ASSERT_EQUAL_STRING("192.168.8.155", B.sat.target.ip);
    in.live = true;
    map(&in);
    TEST_ASSERT_EQUAL(DUI_SAT_CONNECTED, B.sat.state);
    in.too_old = true;
    in.companion_version = "4.2.0";
    map(&in);
    TEST_ASSERT_EQUAL(DUI_SAT_TOO_OLD, B.sat.state);
    TEST_ASSERT_EQUAL_STRING("4.2.0", B.sat.version);
    in.sel = SEL_AMBIGUOUS;
    map(&in);
    TEST_ASSERT_EQUAL(DUI_SAT_CHOOSE, B.sat.state);
    NS.route = NET_ROUTE_NONE;
    map(&in);
    TEST_ASSERT_EQUAL(DUI_SAT_NONET, B.sat.state);
    /* a manual address: its name is itself, no IPv4 to show in octets */
    NS.route = NET_ROUTE_ETH;
    in.sel = SEL_CONNECT;
    in.too_old = in.live = false;
    in.target_host = "studio.local";
    map(&in);
    TEST_ASSERT_EQUAL_STRING("studio.local", B.sat.target.name);
    TEST_ASSERT_EQUAL_STRING("", B.sat.target.ip);
    /* the saved companionHost (Set IP's prefill) only when it is an IPv4 address; companionPort as saved */
    TEST_ASSERT_EQUAL_STRING("", B.sat.saved_ip); /* none given */
    TEST_ASSERT_EQUAL(0, B.sat.port);
    in.companion_host = "studio.local";
    in.companion_port = 16622;
    map(&in);
    TEST_ASSERT_EQUAL_STRING("", B.sat.saved_ip);
    TEST_ASSERT_EQUAL(16622, B.sat.port);
    in.companion_host = "10.0.0.7";
    in.companion_port = 16000;
    map(&in);
    TEST_ASSERT_EQUAL_STRING("10.0.0.7", B.sat.saved_ip);
    TEST_ASSERT_EQUAL(16000, B.sat.port);
    in.companion_host = NULL;
    /* at most 6 */
    static companion_instance_t many[8];
    for (int i = 0; i < 8; i++) snprintf(many[i].host, sizeof many[i].host, "10.0.0.%d", i + 1);
    in.comps = many;
    in.ncomps = 8;
    map(&in);
    TEST_ASSERT_EQUAL(DUI_COMPS_MAX, B.sat.ncomps);
    TEST_ASSERT_EQUAL_STRING("10.0.0.6", B.sat.comp[5].ip);
}

/* The mapped snapshot drives the real screens: the dock home of that box. */
static void test_end_to_end_home(void)
{
    surf_maint_in_t in = in_default();
    map(&in);
    static dui_screen_t s;
    dui_build(&s, DUI_PAGE_HOME, &B);
    TEST_ASSERT_EQUAL_STRING("Waiting", s.status.word[0]);
    in.connected = in.ondemand = true;
    in.dock_peer = "192.168.8.155";
    map(&in);
    dui_build(&s, DUI_PAGE_HOME, &B);
    TEST_ASSERT_EQUAL_STRING("Connected", s.status.word[0]);
    TEST_ASSERT_TRUE(s.status.close);
}

/* who gets a press, in every state of the hand-over. */
static void test_input_route(void)
{
    /* nothing on demand: Companion when it drives the deck, else the UI when shown, else nobody */
    TEST_ASSERT_EQUAL(SURF_TO_COMPANION, surf_input_route(true, false, false, true, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, false, true, false, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(true, false, false, false, false, false)); /* a message */
    /* Companion gone a moment ago, its last images (dimmed, "Reconnecting") still shown: a press brings the box's
     * screens on that deck at once (and does nothing else); never for Companion, never under on demand */
    TEST_ASSERT_EQUAL(SURF_TO_WAKE, surf_input_route(true, false, false, false, false, true));
    TEST_ASSERT_EQUAL(SURF_TO_COMPANION, surf_input_route(true, false, false, true, false, true)); /* back meanwhile */
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, false, true, false, false, true));
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(true, true, false, false, false, true));
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(false, false, false, false, false, true)); /* the Pedal: no screen */
    /* opening: asked for but not drawn yet: nobody, never Companion; drawn: the UI */
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(true, true, false, true, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(true, true, false, false, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, true, true, true, false, false));
    /* closing: the UI while it is still drawn; then nobody until Companion's images are back; then
     * Companion */
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, false, true, true, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(true, false, false, true, true, false));
    TEST_ASSERT_EQUAL(SURF_TO_COMPANION, surf_input_route(true, false, false, true, false, false));
    /* reopened during the hand-back: the UI again once drawn, nobody before */
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(true, true, false, true, true, false));
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, true, true, true, true, false));
    /* the Pedal (no screen): Companion's always */
    TEST_ASSERT_EQUAL(SURF_TO_COMPANION, surf_input_route(false, true, false, true, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(false, true, false, false, false, false));
}

/* Closing: the hand-back is over once Companion's images are on the deck, not merely queued for it,
 * or at the cap: 3 s for every deck, so the slow full-speed BMP decks can rewrite every key. */
static void test_handback_step(void)
{
    TEST_ASSERT_EQUAL_INT64(3000000, SURF_HANDBACK_US);
    const int64_t until = SURF_HANDBACK_US; /* closed at 0 */
    /* a fast deck: back as soon as its images are written, long before the cap */
    TEST_ASSERT_EQUAL(SURF_HB_BACK, surf_handback_step(true, true, 30000, until));
    /* still writing at 1 s (round 2's cap) and 2.9 s: waits on */
    TEST_ASSERT_EQUAL(SURF_HB_WAIT, surf_handback_step(true, false, 1000000, until));
    TEST_ASSERT_EQUAL(SURF_HB_WAIT, surf_handback_step(true, false, 2900000, until));
    /* all handed to usbdeck, still being written (a full-speed or BMP deck): wait */
    TEST_ASSERT_EQUAL(SURF_HB_WAIT, surf_handback_step(true, false, 10000, until));
    /* usbdeck idle, but imgwork still at the last frame (or dockmode still writing its cache back): wait */
    TEST_ASSERT_EQUAL(SURF_HB_WAIT, surf_handback_step(false, true, 10000, until));
    TEST_ASSERT_EQUAL(SURF_HB_WAIT, surf_handback_step(false, false, 10000, until));
    /* handed and written: back */
    TEST_ASSERT_EQUAL(SURF_HB_BACK, surf_handback_step(true, true, 10000, until));
    /* the cap: over whatever they say (a deck that never drains, one that is stuck) */
    TEST_ASSERT_EQUAL(SURF_HB_LATE, surf_handback_step(true, false, until, until));
    TEST_ASSERT_EQUAL(SURF_HB_LATE, surf_handback_step(false, false, until + 1, until));
    TEST_ASSERT_EQUAL(SURF_HB_WAIT, surf_handback_step(true, false, until - 1, until));
    TEST_ASSERT_EQUAL(SURF_HB_BACK, surf_handback_step(true, true, until + 1, until)); /* back, not by time */
}

static const deck_model_t *M(uint16_t pid)
{
    const deck_model_t *m = deck_model_find(DECK_VID_ELGATO, pid);
    TEST_ASSERT_NOT_NULL(m);
    return m;
}

/* the screen on demand opens on one deck, the biggest with a screen: most keys, then the larger key,
 * then a strip or info bar, then the earliest attached; never the Pedal. */
static void test_od_pick(void)
{
    const deck_model_t *mini = M(0x0063), *plus = M(0x0084), *neo = M(0x009A), *mk2 = M(0x0080), *orig = M(0x0060),
                       *m32 = M(0x00BA), *xl = M(0x006C), *plus_xl = M(0x00C6), *pedal = M(0x0086);
    surf_od_cand_t c[8] = {
        {mini, "streamdeck:MINI", 1}, {plus, "streamdeck:PLUS", 2}, {neo, "streamdeck:NEO", 3},
        {mk2, "streamdeck:MK2", 4},   {m32, "streamdeck:M32", 5},   {plus_xl, "streamdeck:PXL", 6},
        {pedal, "streamdeck:PED", 0}, {NULL, "", 0},
    };
    TEST_ASSERT_EQUAL(5, surf_od_pick(c, 8));                        /* + XL, 36 keys */
    c[5].model = NULL;
    TEST_ASSERT_EQUAL(4, surf_od_pick(c, 8));                        /* Module 32 */
    c[4] = (surf_od_cand_t){xl, "streamdeck:XL", 9};
    TEST_ASSERT_EQUAL(4, surf_od_pick(c, 8));                        /* XL, 32 */
    c[4].model = NULL;
    TEST_ASSERT_EQUAL(3, surf_od_pick(c, 8));                        /* MK.2, 15 */
    c[3].model = NULL;
    TEST_ASSERT_EQUAL(1, surf_od_pick(c, 8));                        /* + over Neo: 8 keys each, 120 px over 96 */
    c[1].model = NULL;
    TEST_ASSERT_EQUAL(2, surf_od_pick(c, 8));                        /* Neo over Mini */
    c[2].model = NULL;
    TEST_ASSERT_EQUAL(0, surf_od_pick(c, 8));                        /* the Mini */
    c[0].model = NULL;
    TEST_ASSERT_EQUAL(-1, surf_od_pick(c, 8));                       /* the Pedal alone: none */
    TEST_ASSERT_EQUAL(-1, surf_od_pick(c, 0));
    /* ties: the same size: the earliest attached; a strip or info bar breaks a tie before that */
    surf_od_cand_t t[3] = {{mk2, "streamdeck:B", 7}, {orig, "streamdeck:A", 3}, {mk2, "streamdeck:C", 5}};
    TEST_ASSERT_EQUAL(1, surf_od_pick(t, 3));
    deck_model_t barred = *mk2;
    barred.info_w = 248;
    barred.info_h = 58;
    t[2].model = &barred;
    TEST_ASSERT_EQUAL(2, surf_od_pick(t, 3));
}

/* A set holding the given ids. */
static surf_od_set_t set_of(const char *a, const char *b)
{
    surf_od_set_t s = {0};
    if (a) surf_od_add(&s, a);
    if (b) surf_od_add(&s, b);
    return s;
}

/* the box menu's set: per deck, no duplicates, closes per deck; a deck found by its id (a replug: the same id). */
static void test_od_set(void)
{
    surf_od_set_t s = {0};
    TEST_ASSERT_FALSE(surf_od_here(&s, "streamdeck:A"));
    TEST_ASSERT_TRUE(surf_od_add(&s, "streamdeck:A"));
    TEST_ASSERT_FALSE(surf_od_add(&s, "streamdeck:A")); /* open there already */
    TEST_ASSERT_FALSE(surf_od_add(&s, ""));
    TEST_ASSERT_TRUE(surf_od_add(&s, "streamdeck:B"));
    TEST_ASSERT_EQUAL(2, s.n);
    TEST_ASSERT_TRUE(surf_od_here(&s, "streamdeck:A") && surf_od_here(&s, "streamdeck:B"));
    TEST_ASSERT_FALSE(surf_od_here(&s, "streamdeck:C"));
    TEST_ASSERT_FALSE(surf_od_here(&s, ""));
    TEST_ASSERT_FALSE(surf_od_here(NULL, "streamdeck:A"));
    TEST_ASSERT_TRUE(surf_od_del(&s, "streamdeck:A")); /* closed on A: B stays open */
    TEST_ASSERT_FALSE(surf_od_del(&s, "streamdeck:A"));
    TEST_ASSERT_FALSE(surf_od_here(&s, "streamdeck:A"));
    TEST_ASSERT_TRUE(surf_od_here(&s, "streamdeck:B"));
    for (int i = 0; i < SURF_MAX_DECKS + 2; i++) { /* never more than the decks there can be */
        char id[16];
        snprintf(id, sizeof id, "streamdeck:%d", i);
        surf_od_add(&s, id);
    }
    TEST_ASSERT_EQUAL(SURF_MAX_DECKS, s.n);
    const deck_model_t *mk2 = M(0x0080), *m32 = M(0x00BA), *pedal = M(0x0086);
    surf_od_cand_t c[3] = {{mk2, "streamdeck:MK2", 1}, {m32, "streamdeck:M32", 2}, {pedal, "streamdeck:PED", 3}};
    TEST_ASSERT_EQUAL(1, surf_od_find(c, 3, "streamdeck:M32"));
    TEST_ASSERT_EQUAL(-1, surf_od_find(c, 3, "streamdeck:PED")); /* the Pedal has no screen: never a menu */
    TEST_ASSERT_EQUAL(-1, surf_od_find(c, 3, ""));
    /* unplugged: pruned, so a replug comes back closed; the others stay open */
    s = set_of("streamdeck:MK2", "streamdeck:M32");
    char gone[SURF_MAX_DECKS][48];
    c[1] = (surf_od_cand_t){NULL, "", 0};
    TEST_ASSERT_EQUAL(1, surf_od_prune(&s, c, 3, gone, SURF_MAX_DECKS));
    TEST_ASSERT_EQUAL_STRING("streamdeck:M32", gone[0]);
    TEST_ASSERT_EQUAL(1, s.n);
    TEST_ASSERT_TRUE(surf_od_here(&s, "streamdeck:MK2"));
    c[1] = (surf_od_cand_t){m32, "streamdeck:M32", 4}; /* replugged: closed */
    TEST_ASSERT_FALSE(surf_od_here(&s, "streamdeck:M32"));
    TEST_ASSERT_EQUAL(0, surf_od_prune(&s, c, 3, gone, SURF_MAX_DECKS));
}

/* each deck's input on its own: the decks it is open on to the box (nobody until drawn); every other deck,
 * Companion's or one plugged in while it is open, as before. */
static void test_od_routing(void)
{
    const surf_od_set_t s = set_of("streamdeck:M32", "streamdeck:MINI");
    const bool m32 = surf_od_here(&s, "streamdeck:M32"), mini = surf_od_here(&s, "streamdeck:MINI"),
               other = surf_od_here(&s, "streamdeck:MK2");
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(true, m32, false, true, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, m32, true, true, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, mini, true, true, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_COMPANION, surf_input_route(true, other, false, true, false, false));
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, other, true, false, false, false)); /* its own screen, as before */
}

static bool held_has(const char **ids, int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(ids[i], id)) return true;
    return false;
}

/* a deck still handing back keeps its hold and its "neither side" gate until its own hand-back is over,
 * whatever opens or closes on another deck; every open deck is held from the moment it opens. */
static void test_od_holds_over_decks(void)
{
    surf_hold_cand_t c[3] = {{"streamdeck:A", false, false}, {"streamdeck:B", false, false}, {"", false, false}};
    const char *ids[SURF_MAX_DECKS + 1];
    surf_od_set_t s = set_of("streamdeck:A", NULL); /* open on A: held at once, before the pass covers it */
    TEST_ASSERT_EQUAL(1, surf_held_decks(&s, c, 3, ids, 4));
    TEST_ASSERT_EQUAL_STRING("streamdeck:A", ids[0]);
    c[0].covered = true;
    TEST_ASSERT_EQUAL(1, surf_held_decks(&s, c, 3, ids, 4)); /* once */
    surf_od_add(&s, "streamdeck:B"); /* opened on B too: both held */
    int n = surf_held_decks(&s, c, 3, ids, 4);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_TRUE(held_has(ids, n, "streamdeck:A") && held_has(ids, n, "streamdeck:B"));
    surf_od_del(&s, "streamdeck:A"); /* closed on A: still covered, then handing back; B still held */
    n = surf_held_decks(&s, c, 3, ids, 4);
    TEST_ASSERT_EQUAL(2, n);
    c[0].covered = false;
    c[0].handing_back = true;
    n = surf_held_decks(&s, c, 3, ids, 4);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_TRUE(held_has(ids, n, "streamdeck:A") && held_has(ids, n, "streamdeck:B"));
    /* A's presses go to neither side meanwhile; B's to the box once drawn */
    TEST_ASSERT_EQUAL(SURF_TO_DROP, surf_input_route(true, surf_od_here(&s, "streamdeck:A"), false, true, true, false));
    c[1].covered = true;
    TEST_ASSERT_EQUAL(SURF_TO_BOX, surf_input_route(true, surf_od_here(&s, "streamdeck:B"), true, true, false, false));
    /* A's own hand-back over: A to Companion again, B still held */
    c[0].handing_back = false;
    n = surf_held_decks(&s, c, 3, ids, 4);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("streamdeck:B", ids[0]);
    TEST_ASSERT_EQUAL(SURF_TO_COMPANION, surf_input_route(true, surf_od_here(&s, "streamdeck:A"), false, true, false, false));
    /* all closed (the service button with any open): every deck hands back on its own */
    s.n = 0;
    c[1].covered = false;
    c[1].handing_back = true;
    TEST_ASSERT_EQUAL(1, surf_held_decks(&s, c, 3, ids, 4));
    c[1].handing_back = false;
    TEST_ASSERT_EQUAL(0, surf_held_decks(&s, c, 3, ids, 4));
    s = set_of("streamdeck:A", "streamdeck:B");
    TEST_ASSERT_EQUAL(1, surf_held_decks(&s, c, 3, ids, 1)); /* never more than max */
}

/* with no deck named, the pick is still the biggest deck with a screen, whatever is open already. */
static void test_od_pick_no_deck_named(void)
{
    const deck_model_t *mini = M(0x0063), *m32 = M(0x00BA), *pedal = M(0x0086);
    surf_od_cand_t c[3] = {{mini, "streamdeck:MINI", 1}, {m32, "streamdeck:M32", 2}, {pedal, "streamdeck:PED", 3}};
    TEST_ASSERT_EQUAL(1, surf_od_pick(c, 3));
    const surf_od_set_t s = set_of("streamdeck:MINI", NULL); /* open on the Mini by its gesture: the pick unchanged */
    TEST_ASSERT_TRUE(surf_od_here(&s, "streamdeck:MINI"));
    TEST_ASSERT_EQUAL(1, surf_od_pick(c, 3));
}

/* which brightness applies when (-1: nothing to set). */
static void test_brightness_decision(void)
{
    /* the maintenance screen on the deck (the box drives it, or on demand over Companion): deckBrightness,
     * at once on every change, only when it differs from what the box set last (-1: nothing set yet) */
    TEST_ASSERT_EQUAL(70, surf_bright_apply(70, -1));
    TEST_ASSERT_EQUAL(-1, surf_bright_apply(70, 70));
    TEST_ASSERT_EQUAL(80, surf_bright_apply(80, 70)); /* Brighter */
    TEST_ASSERT_EQUAL(-1, surf_bright_apply(0, -1));  /* no setting yet */
    /* the box's goes through dockmode's cache exactly while Companion's dock connection owns
     * the deck, whatever an earlier look said; else straight to the deck (Satellite, or nobody's) */
    TEST_ASSERT_EQUAL(SURF_BRIGHT_DOCKCACHE, surf_bright_box_path(true, true));
    TEST_ASSERT_EQUAL(SURF_BRIGHT_DIRECT, surf_bright_box_path(true, false));
    TEST_ASSERT_EQUAL(SURF_BRIGHT_DIRECT, surf_bright_box_path(false, true));
    TEST_ASSERT_EQUAL(SURF_BRIGHT_DIRECT, surf_bright_box_path(false, false));
    /* on demand closed over Companion's deck: its last brightness back when the box changed it */
    TEST_ASSERT_EQUAL(65, surf_bright_handback(65, 70));
    TEST_ASSERT_EQUAL(0, surf_bright_handback(0, 70));   /* Companion's 0 is a brightness too */
    TEST_ASSERT_EQUAL(-1, surf_bright_handback(-1, 70)); /* none known: leave it */
    TEST_ASSERT_EQUAL(-1, surf_bright_handback(65, -1)); /* the box set none */
    /* Companion's brightness: on the deck now, or kept while on demand covers it */
    TEST_ASSERT_EQUAL(SURF_BRIGHT_DIRECT, surf_bright_comp_path(false));
    TEST_ASSERT_EQUAL(SURF_BRIGHT_KEEP, surf_bright_comp_path(true));
}

static void test_nav_events(void)
{
    deck_event_t e = {.kind = DECK_EV_KEY, .index = 9, .pressed = true};
    surf_nav_ev_t n = surf_nav_event(M(0x00BA), &e); /* Module 32: 8 a row */
    TEST_ASSERT_EQUAL(SURF_NAV_KEY, n.kind);
    TEST_ASSERT_EQUAL(1, n.col);
    TEST_ASSERT_EQUAL(1, n.row);
    TEST_ASSERT_TRUE(n.down);
    e.pressed = false;
    n = surf_nav_event(M(0x00BA), &e);
    TEST_ASSERT_FALSE(n.down);
    e = (deck_event_t){.kind = DECK_EV_KEY, .index = 0, .pressed = true};
    n = surf_nav_event(M(0x0060), &e); /* the Original v1: rows mirrored */
    TEST_ASSERT_EQUAL(SURF_NAV_KEY, n.kind);
    TEST_ASSERT_EQUAL(4, n.col);
    TEST_ASSERT_EQUAL(0, n.row);
    e.index = 99;
    TEST_ASSERT_EQUAL(SURF_NAV_NONE, surf_nav_event(M(0x00BA), &e).kind);
    /* the Neo's touch keys (indexes 8, 9): on the press */
    e = (deck_event_t){.kind = DECK_EV_KEY, .index = 8, .pressed = true};
    n = surf_nav_event(M(0x009A), &e);
    TEST_ASSERT_EQUAL(SURF_NAV_TOUCH, n.kind);
    TEST_ASSERT_EQUAL(-1, n.steps);
    e.index = 9;
    TEST_ASSERT_EQUAL(1, surf_nav_event(M(0x009A), &e).steps);
    e.pressed = false;
    TEST_ASSERT_EQUAL(SURF_NAV_NONE, surf_nav_event(M(0x009A), &e).kind);
    /* encoders */
    e = (deck_event_t){.kind = DECK_EV_ENC_ROTATE, .index = 1, .delta = -2};
    n = surf_nav_event(M(0x0084), &e);
    TEST_ASSERT_EQUAL(SURF_NAV_DIAL, n.kind);
    TEST_ASSERT_EQUAL(-2, n.steps);
    e = (deck_event_t){.kind = DECK_EV_ENC_PRESS, .index = 0, .pressed = true};
    TEST_ASSERT_EQUAL(SURF_NAV_BACK, surf_nav_event(M(0x0084), &e).kind);
    e.pressed = false;
    TEST_ASSERT_EQUAL(SURF_NAV_NONE, surf_nav_event(M(0x0084), &e).kind);
    /* the strip: a sideways swipe pages (left = next), a vertical swipe nothing, a tap its place */
    e = (deck_event_t){.kind = DECK_EV_TOUCH_SWIPE, .x = 600, .y = 50, .x2 = 100, .y2 = 55};
    n = surf_nav_event(M(0x0084), &e);
    TEST_ASSERT_EQUAL(SURF_NAV_DIAL, n.kind);
    TEST_ASSERT_EQUAL(1, n.steps);
    e.x = 100;
    e.x2 = 600;
    TEST_ASSERT_EQUAL(-1, surf_nav_event(M(0x0084), &e).steps);
    e = (deck_event_t){.kind = DECK_EV_TOUCH_SWIPE, .x = 100, .y = 10, .x2 = 105, .y2 = 90};
    TEST_ASSERT_EQUAL(SURF_NAV_NONE, surf_nav_event(M(0x0084), &e).kind);
    e = (deck_event_t){.kind = DECK_EV_TOUCH_TAP, .x = 760, .y = 50};
    n = surf_nav_event(M(0x0084), &e);
    TEST_ASSERT_EQUAL(SURF_NAV_BAR, n.kind);
    TEST_ASSERT_EQUAL(760, n.x);
    TEST_ASSERT_EQUAL(SURF_NAV_NONE, surf_nav_event(M(0x009A), &e).kind); /* the Neo's info bar isn't touch */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_network_and_settings);
    RUN_TEST(test_eth_addressing);
    RUN_TEST(test_wifi_off);
    RUN_TEST(test_slot);
    RUN_TEST(test_satellite);
    RUN_TEST(test_end_to_end_home);
    RUN_TEST(test_input_route);
    RUN_TEST(test_handback_step);
    RUN_TEST(test_od_pick);
    RUN_TEST(test_od_set);
    RUN_TEST(test_od_routing);
    RUN_TEST(test_od_holds_over_decks);
    RUN_TEST(test_od_pick_no_deck_named);
    RUN_TEST(test_brightness_decision);
    RUN_TEST(test_nav_events);
    return UNITY_END();
}
