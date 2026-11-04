/* The maintenance UI's screen builders (components/deckui/screens.c):
 * each home state's essentials, Network's Wi-Fi rows, Companion, Hotspot, the QR payloads, the Box's
 * slot buttons, the locks, the menu dots and the Close key. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fit_check.h"
#include "deckui/ipedit.h"
#include "deckui/pin.h"
#include "deckui/nav.h"
#include "unity.h"
#include "deckmodel/model.h"
#include "deckui/deckui.h"
#include "deckui/screens.h"
#include "rpc/refusals.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers ---- */

enum { PID_M32 = 0x00BA, PID_PLUS = 0x0084, PID_MINI = 0x0063 };

static dui_screen_t S;
static dui_layout_t L;

/* The default box: dock mode waiting, hotspot on, main Wi-Fi, the recovery slot in sync. */
static dui_box_t box_default(void)
{
    dui_box_t b;
    memset(&b, 0, sizeof b);
    b.mode = DUI_MODE_DOCK;
    b.sat.state = DUI_SAT_FINDING;
    static const dui_comp_t COMPS[] = {{"Studio-Mac", "192.168.8.155"}, {"FOH-PC", "192.168.8.40"}};
    b.sat.ncomps = 2;
    memcpy(b.sat.comp, COMPS, sizeof COMPS);
    b.sat.target = COMPS[0];
    strcpy(b.sat.version, "4.2.0");
    b.dock.port = 5343;
    strcpy(b.net.wifi_main, "Simbox-Home");
    strcpy(b.net.wifi_backup, "Office-5G");
    b.net.wifi_net = DUI_WIFI_MAIN;
    b.net.rssi = -58;
    strcpy(b.net.wifi_ip, "192.168.8.225");
    b.net.ap_on = true;
    strcpy(b.net.ap_ssid, "CompanionBox-8D1C");
    strcpy(b.net.ap_pass, "k7m2p9xq");
    b.net.ap_secured = true;
    strcpy(b.net.ap_ip, "192.168.4.1");
    strcpy(b.net.name, "CompanionBox-8D1C");
    strcpy(b.box.version, "0.1.0");
    b.box.uptime_s = 2 * 3600 + 14 * 60 + 5;
    b.box.slot = DUI_SLOT_IN_SYNC;
    b.box.pct = 100;
    strcpy(b.box.other_version, "0.1.0");
    b.box.other_valid = true;
    b.brightness = 70;
    b.sound = true;
    b.net.eth_dhcp = true; /* the default: DHCP, nothing to make static from */
    return b;
}

static dui_box_t sat(int state)
{
    dui_box_t b = box_default();
    b.mode = DUI_MODE_SAT;
    b.sat.state = (uint8_t)state;
    b.sat.auto_disc = true; /* companionAuto, on by default */
    if (state == DUI_SAT_NONET) b.net.wifi_net = DUI_WIFI_NONE, b.net.wifi_ip[0] = '\0';
    return b;
}

/* A tile as text: a word its text, "Caption:text", "#n verb" a step, "[Label]" a button ("[Name/IP]" a
 * choice), "(Label)" an icon tile, "*" the spinner, "QR", "." anything else. */
static void tile_text(const dui_tile_t *t, char *out, size_t n)
{
    char txt[160] = "";
    if (t->text) snprintf(txt, sizeof txt, "%.*s", t->len ? t->len : (int)strlen(t->text), t->text);
    switch (t->kind) {
    case DUI_TILE_WORD: snprintf(out, n, "%s", txt); break;
    case DUI_TILE_CAP: snprintf(out, n, "%s:%s", t->caption, txt); break;
    case DUI_TILE_STEP: snprintf(out, n, "#%d %s", t->num, txt); break;
    case DUI_TILE_BUTTON:
        if (t->caption && t->caption[0]) snprintf(out, n, "[%s/%s]", txt, t->caption);
        else snprintf(out, n, "[%s]", txt);
        break;
    case DUI_TILE_ICON: snprintf(out, n, "(%s)", txt); break;
    case DUI_TILE_SPIN: snprintf(out, n, "*"); break;
    case DUI_TILE_QR: snprintf(out, n, "QR"); break;
    default: snprintf(out, n, "."); break;
    }
}

/* The screen's content: tiles joined by '|', groups by " / ", a group prefixed "S " in the service zone
 * and "^" when anchored. */
static const char *content(const dui_screen_t *s)
{
    static char out[2048];
    size_t u = 0;
    out[0] = '\0';
    for (int g = 0; g < s->ngroups; g++) {
        const dui_group_t *gr = &s->group[g];
        u += (size_t)snprintf(out + u, sizeof out - u, "%s%s%s", g ? " / " : "", gr->flags & DUI_G_ANCHOR ? "^" : "",
                              gr->zone == DUI_ZONE_SVC ? "S " : "");
        for (int i = gr->first; i < gr->first + gr->n; i++) {
            char k[200];
            tile_text(&s->tile[i], k, sizeof k);
            u += (size_t)snprintf(out + u, sizeof out - u, "%s%s", i > gr->first ? "|" : "", k);
        }
    }
    return out;
}

/* The status words, space-separated (short: the short form). */
static const char *words(const dui_screen_t *s, bool short_form)
{
    static char out[128];
    size_t u = 0;
    out[0] = '\0';
    const char *const *w = short_form ? s->status.short_word : s->status.word;
    for (int i = 0; i < DUI_STATUS_WORDS && w[i]; i++)
        u += (size_t)snprintf(out + u, sizeof out - u, "%s%s", i ? " " : "", w[i]);
    return out;
}

/* The button labelled label on s; NULL when there is none. */
static const dui_tile_t *maybe_button(const dui_screen_t *s, const char *label)
{
    for (int i = 0; i < s->ntiles; i++)
        if (s->tile[i].kind == DUI_TILE_BUTTON && !strcmp(s->tile[i].text, label)) return &s->tile[i];
    return NULL;
}

/* The same, there. */
static const dui_tile_t *button(const dui_screen_t *s, const char *label)
{
    const dui_tile_t *t = maybe_button(s, label);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, label);
    return t;
}

static const dui_tile_t *menu_button(const dui_screen_t *s, const char *label)
{
    for (int i = 0; i < s->nmenu; i++)
        if (!strcmp(s->menu_tile[i].text, label)) return &s->menu_tile[i];
    TEST_FAIL_MESSAGE(label);
    return NULL;
}

/* The tile with this caption, or with this text when caption is NULL: there. */
static const dui_tile_t *find(const dui_screen_t *s, const char *caption, const char *text)
{
    for (int i = 0; i < s->ntiles; i++) {
        const dui_tile_t *t = &s->tile[i];
        if (caption ? t->caption && !strcmp(t->caption, caption)
                    : t->text && (int)strlen(text) == (t->len ? t->len : (int)strlen(t->text)) &&
                          !strncmp(t->text, text, strlen(text)))
            return t;
    }
    TEST_FAIL_MESSAGE(caption ? caption : text);
    return NULL;
}

static void build(int page, const dui_box_t *b) { dui_build(&S, page, b); }

static void plan(uint16_t pid)
{
    dui_deck_t d;
    TEST_ASSERT_TRUE(dui_deck_of(deck_model_find(DECK_VID_ELGATO, pid), &d));
    dui_plan(&d, &S, &L);
    TEST_ASSERT_FALSE(L.overflow);
}

static dui_deck_t deck_of(uint16_t pid)
{
    dui_deck_t d;
    TEST_ASSERT_TRUE(dui_deck_of(deck_model_find(DECK_VID_ELGATO, pid), &d));
    return d;
}

/* Where tile ti is placed: its row (and *page, *col); -1: nowhere. */
static int tile_row(int ti, int *page, int *col)
{
    for (int p = 0; p < L.pages; p++)
        for (int k = 0; k < L.deck.cols * L.deck.rows; k++)
            if (L.key[p][k] == ti) {
                *page = p;
                *col = k % L.deck.cols;
                return k / L.deck.cols;
            }
    return -1;
}

/* The tile showing exactly text (n characters), its index; -1: none. */
static int tile_with(const char *text)
{
    const int n = (int)strlen(text);
    for (int j = 0; j < L.ntiles; j++) {
        const dui_tile_t *t = &L.tile[j];
        if (t->text && (t->len ? t->len : (int)strlen(t->text)) == n && !strncmp(t->text, text, (size_t)n)) return j;
    }
    return -1;
}

/* ---- home ---- */

#define HOTSPOT "^S Hotspot:CompanionBox-8D1C|Password:k7m2p9xq|QR" /* its join code after it */

static void test_home_dock_waiting(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:192|168|8|225|Port:5343 / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL_STRING("remote surface", find(&S, NULL, "Add")->caption);
    TEST_ASSERT_EQUAL_STRING("Waiting for Companion", words(&S, false));
    TEST_ASSERT_EQUAL_STRING("Waiting", words(&S, true));
    TEST_ASSERT_EQUAL(DUI_STATE_WAIT, S.status.state);
    TEST_ASSERT_TRUE(S.menu);
    TEST_ASSERT_FALSE(S.sub);
    TEST_ASSERT_FALSE(S.overflow);
    /* the box's address is Ethernet's when it has one */
    strcpy(b.net.eth_ip, "10.0.0.7");
    b.net.eth_link = true;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:10|0|0|7|Port:5343 / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL(DUI_NET_ETH, S.status.net);
    /* an open hotspot: "Open" for the password; off: no service zone */
    b = box_default();
    b.net.ap_secured = false;
    b.net.ap_pass[0] = '\0';
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:192|168|8|225|Port:5343 / ^S Hotspot:CompanionBox-8D1C|Open|QR", content(&S));
    TEST_ASSERT_EQUAL(DUI_ROLE_LABEL, find(&S, NULL, "Open")->role);
    b.net.ap_on = false;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:192|168|8|225|Port:5343", content(&S));
    TEST_ASSERT_FALSE(S.status.hotspot);
}

/* the status row's signs as the box's state gives them, row 0's sign keys left to right: "hot" / "hotoff" (the
 * hotspot, always), "wifi" (its bars) / "wifioff" (crossed grey: off by choice, or none set up) / "nonet" (crossed red:
 * on and not joined), "eth" (while its cable is linked); the route's sign kept the longest where the row is short. */
static const char *signs(uint16_t pid)
{
    static char out[64];
    out[0] = '\0';
    plan(pid);
    for (int c = 0; c < L.deck.cols; c++) {
        dui_tile_t t;
        if (!dui_layout_key(&L, 0, c, 0, &t) || t.kind != DUI_TILE_IND) continue;
        const char *n = t.icon == DUI_ICON_HOTSPOT ? "hot" : t.icon == DUI_ICON_HOTSPOT_OFF ? "hotoff" : t.icon == DUI_ICON_WIFI ? "wifi"
                        : t.icon == DUI_ICON_WIFI_OFF ? "wifioff" : t.icon == DUI_ICON_NONET ? "nonet" : t.icon == DUI_ICON_ETH ? "eth" : "?";
        const size_t u = strlen(out);
        snprintf(out + u, sizeof out - u, "%s%s", u ? "|" : "", n);
        TEST_ASSERT_EQUAL(t.icon == DUI_ICON_HOTSPOT || t.icon == DUI_ICON_HOTSPOT_OFF ? DUI_ACT_HOTSPOT : DUI_ACT_NET, t.action);
    }
    return out;
}

static void test_status_signs(void)
{
    enum { M32 = 0x00BA, MK2 = 0x0080 };
    dui_box_t b = box_default(); /* Wi-Fi joined and routing, the hotspot on, no cable */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(DUI_SIGN_UP, S.status.wifi);
    TEST_ASSERT_FALSE(S.status.eth_link);
    TEST_ASSERT_EQUAL_STRING("hot|wifi", signs(M32));
    b.net.ap_on = false; /* off: crossed in grey, still there */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("hotoff|wifi", signs(M32));
    b.net.eth_link = true; /* the cable linked, Ethernet routing: all three; Wi-Fi joined on standby keeps its bars */
    strcpy(b.net.eth_ip, "10.0.0.7");
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(DUI_NET_ETH, S.status.net);
    TEST_ASSERT_EQUAL(-58, S.status.rssi);
    TEST_ASSERT_EQUAL_STRING("hotoff|wifi|eth", signs(M32));
    TEST_ASSERT_EQUAL_STRING("eth", signs(MK2)); /* room for one: the route's */
    b.net.wifi_off = true; /* Wi-Fi off by choice: crossed in grey */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(DUI_SIGN_OFF, S.status.wifi);
    TEST_ASSERT_EQUAL_STRING("hotoff|wifioff|eth", signs(M32));
    b.net.wifi_off = false; /* on, not joined: crossed in red */
    b.net.wifi_net = DUI_WIFI_NONE;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(DUI_SIGN_DOWN, S.status.wifi);
    TEST_ASSERT_EQUAL_STRING("hotoff|nonet|eth", signs(M32));
    b.net.wifi_main[0] = b.net.wifi_backup[0] = '\0'; /* no network set up: nothing wrong, grey */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("hotoff|wifioff|eth", signs(M32));
    b.net.eth_ip[0] = '\0'; /* the cable linked, no address, no route: Wi-Fi's sign is the one kept */
    strcpy(b.net.wifi_main, "Simbox-Home");
    b.net.wifi_ip[0] = '\0';
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(DUI_NET_NONE, S.status.net);
    TEST_ASSERT_EQUAL_STRING("hotoff|nonet|eth", signs(M32));
    TEST_ASSERT_EQUAL_STRING("nonet|eth", signs(MK2)); /* "No network": two words, room for two signs */
    b.net.wifi_main[0] = '\0'; /* no network set up and no route at all: a problem, red (not grey) */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(DUI_SIGN_DOWN, S.status.wifi);
    TEST_ASSERT_EQUAL_STRING("hotoff|nonet|eth", signs(M32));
    b.net.wifi_off = true; /* turned off by choice: grey, route or not */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(DUI_SIGN_OFF, S.status.wifi);
    b.net.wifi_off = false;
    strcpy(b.net.wifi_main, "Simbox-Home");
    b = box_default(); /* Wi-Fi routing, the cable linked too (no address on it): Wi-Fi kept, Ethernet shown */
    b.net.eth_link = true;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("hot|wifi|eth", signs(M32));
    TEST_ASSERT_EQUAL_STRING("wifi", signs(MK2));
    /* the 32-key Network screen: its Wi-Fi key takes the row's sixth column; the signs right of it, the route's and the
     * other network's kept, the hotspot gone */
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("wifi|eth", signs(M32));
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 5, 0, &t));
    TEST_ASSERT_EQUAL(DUI_TILE_BUTTON, t.kind);
    b.net.eth_link = false;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("hot|wifi", signs(M32));
    /* drawn: the crossed hotspot and Wi-Fi off all grey (no white, no red); the red-slashed one red */
    static const struct {
        int icon;
        bool red;
    } D[] = {{DUI_ICON_HOTSPOT_OFF, false}, {DUI_ICON_WIFI_OFF, false}, {DUI_ICON_NONET, true}};
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) {
        dui_tile_t k;
        memset(&k, 0, sizeof k);
        k.kind = DUI_TILE_IND;
        k.icon = (uint8_t)D[i].icon;
        static uint8_t px[96 * 96 * 3];
        memset(px, 0, sizeof px);
        dui_render_key(&k, 96, px);
        int white = 0, red = 0, ink = 0;
        for (int j = 0; j < 96 * 96; j++) {
            const uint8_t *q = px + 3 * j;
            white += q[0] > 200 && q[1] > 200 && q[2] > 200;
            red += q[0] > 200 && q[1] < 120;
            ink += q[0] > 60 || q[1] > 60 || q[2] > 60;
        }
        TEST_ASSERT_EQUAL(0, white);
        TEST_ASSERT_TRUE(ink > 150);
        TEST_ASSERT_EQUAL(D[i].red, red > 30);
    }
}

static void test_home_no_network(void)
{
    dui_box_t b = box_default();
    b.net.wifi_net = DUI_WIFI_NONE;
    b.net.wifi_ip[0] = '\0';
    build(DUI_PAGE_HOME, &b); /* dock mode: no address */
    /* each step's QR code after its words (the join code in the service zone, the URL code in the main one) */
    TEST_ASSERT_EQUAL_STRING("S #1 Join|Hotspot:CompanionBox-8D1C|Password:k7m2p9xq / S QR / #2 Open|Setup page:192|168|4|1 / QR",
                             content(&S));
    TEST_ASSERT_EQUAL_STRING("No network", words(&S, false));
    TEST_ASSERT_EQUAL(DUI_STATE_BAD, S.status.state);
    TEST_ASSERT_EQUAL(DUI_NET_NONE, S.status.net);
    b = sat(DUI_SAT_NONET); /* satellite: the same */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("S #1 Join|Hotspot:CompanionBox-8D1C|Password:k7m2p9xq / S QR / #2 Open|Setup page:192|168|4|1 / QR",
                             content(&S));
    b.net.ap_secured = false; /* open */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("S #1 Join|Hotspot:CompanionBox-8D1C|Open / S QR / #2 Open|Setup page:192|168|4|1 / QR", content(&S));
    b.net.ap_on = false; /* hotspot off */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("Plug in cable", content(&S)); /* one key */
}

/* The screen's QR tiles' payloads, in order. */
static int qr_payloads(const char *out[4])
{
    int n = 0;
    for (int i = 0; i < S.ntiles && n < 4; i++)
        if (S.tile[i].kind == DUI_TILE_QR) out[n++] = S.tile[i].text;
    return n;
}

/* the setup screen (no network, the hotspot on) carries the QR page's two codes, the join code (an open
 * hotspot's without a password) and the setup page's URL, each right after its step's words. Every other home with
 * the hotspot on shows its join code after its name and password (wherever the hotspot shows, its code to join it);
 * the hotspot off, none. On every model each code is placed, the URL code on its step's page, the join code too where the row has
 * room (the 32-key decks, the + XL, the 15-key decks, the + and the Neo; the Mini: the next page). */
static void test_setup_screen_qr(void)
{
    dui_box_t b = sat(DUI_SAT_NONET);
    build(DUI_PAGE_HOME, &b);
    const char *q[4];
    TEST_ASSERT_EQUAL(2, qr_payloads(q));
    char join[256];
    dui_wifi_join(join, sizeof join, "CompanionBox-8D1C", "k7m2p9xq");
    TEST_ASSERT_EQUAL_STRING(join, q[0]);
    TEST_ASSERT_EQUAL_STRING("http://192.168.4.1/", q[1]);
    b.net.ap_secured = false; /* open: no password in the join code */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(2, qr_payloads(q));
    dui_wifi_join(join, sizeof join, "CompanionBox-8D1C", NULL);
    TEST_ASSERT_EQUAL_STRING(join, q[0]);
    TEST_ASSERT_NULL(strstr(q[0], "k7m2p9xq"));
    TEST_ASSERT_NULL(strstr(q[0], "P:"));
    /* the hotspot off: no code */
    b.net.ap_on = false;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(0, qr_payloads(q));
    /* every other home with the hotspot on: its join code, once (each Satellite state, dock mode, open or not) */
    dui_wifi_join(join, sizeof join, "CompanionBox-8D1C", "k7m2p9xq");
    static const int SAT[] = {DUI_SAT_FINDING, DUI_SAT_CHOOSE, DUI_SAT_TOO_OLD, DUI_SAT_RECON, DUI_SAT_CONNECTED};
    for (size_t i = 0; i < sizeof SAT / sizeof SAT[0]; i++) {
        b = sat(SAT[i]);
        build(DUI_PAGE_HOME, &b);
        TEST_ASSERT_EQUAL(1, qr_payloads(q));
        TEST_ASSERT_EQUAL_STRING(join, q[0]);
        b.net.ap_on = false;
        build(DUI_PAGE_HOME, &b);
        TEST_ASSERT_EQUAL(0, qr_payloads(q));
    }
    b = box_default();
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(1, qr_payloads(q));
    TEST_ASSERT_EQUAL_STRING(join, q[0]);
    b.net.ap_secured = false;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(1, qr_payloads(q));
    TEST_ASSERT_NULL(strstr(q[0], "P:")); /* open: no password in it */
    /* positions per model */
    static const struct {
        uint16_t pid;
        bool join_with_step; /* the join code on the "1 Join" page */
    } M[] = {{0x00BA, true}, {0x006C, true}, {0x00C6, true}, {0x0080, true}, {0x0084, true}, {0x009A, true}, {0x0063, false}};
    b = sat(DUI_SAT_NONET);
    build(DUI_PAGE_HOME, &b);
    for (size_t m = 0; m < sizeof M / sizeof M[0]; m++) {
        plan(M[m].pid); /* asserts no overflow */
        int pq[2] = {-1, -1}, nq = 0, pj = -1, po = -1, p, c;
        for (int j = 0; j < L.ntiles; j++) {
            const dui_tile_t *t = &L.tile[j];
            if (tile_row(j, &p, &c) < 0) continue;
            if (t->kind == DUI_TILE_QR && nq < 2) pq[nq++] = p;
            if (t->kind == DUI_TILE_STEP && t->num == 1) pj = p;
            if (t->kind == DUI_TILE_STEP && t->num == 2) po = p;
        }
        char msg[32];
        snprintf(msg, sizeof msg, "0x%04X", M[m].pid);
        TEST_ASSERT_EQUAL_MESSAGE(2, nq, msg);
        TEST_ASSERT_TRUE_MESSAGE(pj >= 0 && po >= 0, msg); /* the words stay */
        TEST_ASSERT_EQUAL_MESSAGE(po, pq[1], msg);
        if (M[m].join_with_step) TEST_ASSERT_EQUAL_MESSAGE(pj, pq[0], msg);
        else TEST_ASSERT_EQUAL_MESSAGE(pj + 1, pq[0], msg);
        if (M[m].pid == 0x00BA || M[m].pid == 0x006C || M[m].pid == 0x00C6) TEST_ASSERT_EQUAL_MESSAGE(1, L.pages, msg);
        fit_layout(&L.deck, &L, "setup screen QR");
    }
}

static void test_home_satellite_states(void)
{
    dui_box_t b = sat(DUI_SAT_FINDING);
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("Box IP:192|168|8|225|*|Searching / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL_STRING("Finding Companion", words(&S, false));
    TEST_ASSERT_EQUAL(DUI_STATE_WAIT, S.status.state);

    b = sat(DUI_SAT_CHOOSE);
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Tap|[Studio-Mac/192.168.8.155]|[FOH-PC/192.168.8.40] / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL_STRING("Choose Companion", words(&S, false));
    TEST_ASSERT_EQUAL_STRING("your Companion", find(&S, NULL, "Tap")->caption);
    TEST_ASSERT_EQUAL(DUI_A_PICK0, button(&S, "Studio-Mac")->action);
    TEST_ASSERT_EQUAL(DUI_A_PICK0 + 1, button(&S, "FOH-PC")->action);
    /* one choice per Companion found, up to 6 */
    b.sat.ncomps = DUI_COMPS_MAX;
    for (int i = 0; i < DUI_COMPS_MAX; i++) {
        snprintf(b.sat.comp[i].name, sizeof b.sat.comp[i].name, "PC-%d", i);
        snprintf(b.sat.comp[i].ip, sizeof b.sat.comp[i].ip, "10.0.0.%d", i + 1);
    }
    build(DUI_PAGE_HOME, &b);
    int choices = 0;
    for (int i = 0; i < S.ntiles; i++)
        if (S.tile[i].kind == DUI_TILE_BUTTON) {
            TEST_ASSERT_EQUAL(DUI_A_PICK0 + choices, S.tile[i].action);
            choices++;
        }
    TEST_ASSERT_EQUAL(DUI_COMPS_MAX, choices);
    TEST_ASSERT_EQUAL_STRING("10.0.0.6", button(&S, "PC-5")->caption);
    b.sat.comp[2].name[0] = '\0'; /* no name: "Companion" over its IP, not the IP twice */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("10.0.0.3", button(&S, "Companion")->caption);
    TEST_ASSERT_EQUAL(DUI_A_PICK0 + 2, button(&S, "Companion")->action);

    b = sat(DUI_SAT_TOO_OLD);
    strcpy(b.sat.target.name, "FOH-PC");
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("Needs 5.0+ has 4.2.0|FOH-PC / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL_STRING("Companion too old", words(&S, false));
    TEST_ASSERT_EQUAL_STRING("Too old", words(&S, true));
    TEST_ASSERT_EQUAL(DUI_STATE_BAD, S.status.state);

    b = sat(DUI_SAT_RECON);
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("*|to|Studio-Mac / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL_STRING("Reconnecting", words(&S, false)); /* never split over keys */
    TEST_ASSERT_EQUAL_STRING("Reconnecting", S.status.text);
}

static void test_home_no_free_dock(void)
{
    dui_box_t b = box_default();
    b.dock.no_free = true;
    b.dock.port = 0;
    build(DUI_PAGE_HOME, &b); /* the deck limit, 2 when the snapshot has none */
    TEST_ASSERT_EQUAL_STRING("Max 2 decks|Unplug one / " HOTSPOT, content(&S));
    b.deck_limit = 4; /* unlocked */
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("Max 4 decks|Unplug one / " HOTSPOT, content(&S));
    b.deck_limit = 2;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("Max 2 decks|Unplug one / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL_STRING("No free dock", words(&S, false));
    TEST_ASSERT_EQUAL_STRING("No dock", words(&S, true));
    TEST_ASSERT_EQUAL(DUI_STATE_BAD, S.status.state);
}

/* On every model, both modes: the connected home on demand reads "Connected Companion" (a grey label, 2 lines
 * in its own key, at 16 px or more) and then the Companion's address with its IP caption (octets, or the key or
 * pairs on the small decks), the port after it in dock mode; everything fits its key. Where the label can't share a
 * page with the address (the Mini in dock mode), there is no label and the address's first key says COMPANION IP:
 * the address and its port on one page. the hotspot's name under HOTSPOT. */
static void test_home_connected_label(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063};
    for (int dock = 0; dock < 2; dock++)
        for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
            dui_box_t b = dock ? box_default() : sat(DUI_SAT_CONNECTED);
            if (dock) strcpy(b.dock.peer, "192.168.8.155");
            b.connected = b.ondemand = true;
            build(DUI_PAGE_HOME, &b);
            plan(PIDS[m]);
            char msg[32];
            snprintf(msg, sizeof msg, "%s 0x%04X", dock ? "dock" : "sat", PIDS[m]);
            const bool alone = dock && PIDS[m] == PID_MINI; /* label + 2 pairs + port: 4 keys, a row of 3 */
            const int li = tile_with("Connected Companion");
            /* the address, accent; captioned IP after the label, or COMPANION IP without it; the port in dock mode */
            int ip = -1, port = -1;
            for (int j = 0; j < L.ntiles; j++) {
                const dui_tile_t *t = &L.tile[j];
                if (ip < 0 && t->caption && !strcmp(t->caption, alone ? "Companion IP" : "IP")) ip = j;
                if (port < 0 && t->caption && !strcmp(t->caption, "Port")) port = j;
            }
            TEST_ASSERT_TRUE_MESSAGE(ip >= 0, msg);
            TEST_ASSERT_EQUAL_MESSAGE(DUI_ROLE_ACCENT, L.tile[ip].role, msg);
            TEST_ASSERT_TRUE_MESSAGE(L.tile[ip].flags & (DUI_F_OCTET | DUI_F_IP), msg); /* octets, or the key / pairs */
            TEST_ASSERT_TRUE_MESSAGE(dock ? port > ip : port < 0, msg);
            int ap, ac;
            const int ar = tile_row(ip, &ap, &ac);
            TEST_ASSERT_TRUE_MESSAGE(ar >= 0, msg);
            if (dock) { /* the port on the address's page */
                int pp, pc;
                TEST_ASSERT_TRUE_MESSAGE(tile_row(port, &pp, &pc) >= 0 && pp == ap, msg);
            }
            if (alone) {
                TEST_ASSERT_EQUAL_MESSAGE(-1, li, msg);
                TEST_ASSERT_EQUAL_MESSAGE(0, ap, msg); /* on the first page */
                TEST_ASSERT_TRUE_MESSAGE(L.tile[ip].fit.cap_px >= DUI_CAP_MIN_PX, msg);
            } else {
                TEST_ASSERT_TRUE_MESSAGE(li >= 0 && ip > li, msg);
                const dui_tile_t *l = &L.tile[li];
                TEST_ASSERT_EQUAL_MESSAGE(DUI_TILE_WORD, l->kind, msg);
                TEST_ASSERT_EQUAL_MESSAGE(DUI_ROLE_LABEL, l->role, msg);
                TEST_ASSERT_EQUAL_MESSAGE(2, l->fit.nl, msg);
                TEST_ASSERT_EQUAL_MESSAGE(0, l->fit.hyph, msg);
                TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_TEXT_MIN_PX, l->fit.px, msg);
                int lp, lc;
                const int lr = tile_row(li, &lp, &lc);
                TEST_ASSERT_TRUE_MESSAGE(lr >= 0 && lp == ap, msg); /* label and address share a page */
                TEST_ASSERT_TRUE_MESSAGE(ar > lr || (ar == lr && ac > lc), msg); /* the address after it */
            }
            for (int j = 0; j < S.ntiles; j++) /* the old caption is gone */
                TEST_ASSERT_FALSE_MESSAGE(S.tile[j].caption && !strcmp(S.tile[j].caption, "Target"), msg);
            const int hs = tile_with("CompanionBox-8D1C");
            TEST_ASSERT_TRUE_MESSAGE(hs >= 0 && L.tile[hs].caption && !strcmp(L.tile[hs].caption, "Hotspot"), msg);
            fit_layout(&L.deck, &L, msg);
        }
}

/* dui_lead in general: a label kept where the tile after it shares its page, dropped (its caption passed on) where
 * it doesn't. A Mini's home: the label, then a run of 3 (an address in pairs and a port) that a row of 3 can't take
 * after it. */
static void test_lead_label(void)
{
    static dui_screen_t s;
    dui_screen_init(&s);
    dui_group(&s, DUI_ZONE_MAIN, 0);
    dui_lead(&s, "Lead words", "Alt cap");
    dui_ipv4(&s, "Cap", "192.168.100.200"); /* the Mini: two keys */
    dui_port(&s, "Port", "5343");
    dui_deck_t d = deck_of(PID_MINI);
    dui_plan(&d, &s, &L);
    TEST_ASSERT_FALSE(L.overflow);
    TEST_ASSERT_EQUAL(-1, tile_with("Lead words"));
    bool alt = false, cap = false;
    for (int j = 0; j < L.ntiles; j++) {
        alt |= L.tile[j].caption && !strcmp(L.tile[j].caption, "Alt cap");
        cap |= L.tile[j].caption && !strcmp(L.tile[j].caption, "Cap");
    }
    TEST_ASSERT_TRUE(alt && !cap);
    d = deck_of(PID_M32); /* room for all: kept, the address's own caption */
    dui_plan(&d, &s, &L);
    TEST_ASSERT_TRUE(tile_with("Lead words") >= 0);
    alt = cap = false;
    for (int j = 0; j < L.ntiles; j++) {
        alt |= L.tile[j].caption && !strcmp(L.tile[j].caption, "Alt cap");
        cap |= L.tile[j].caption && !strcmp(L.tile[j].caption, "Cap");
    }
    TEST_ASSERT_TRUE(cap && !alt);
    /* Before anything but an address the label stays, even apart from it (a run of 3 words that a
     * Mini's row of 3 can't take after the label), and its caption names nothing */
    dui_screen_init(&s);
    dui_group(&s, DUI_ZONE_MAIN, 0);
    dui_lead(&s, "Lead words", "Alt cap");
    dui_words(&s, "Name", "Alpha Beta Gamma", DUI_ROLE_VALUE);
    d = deck_of(PID_MINI);
    dui_plan(&d, &s, &L);
    TEST_ASSERT_FALSE(L.overflow);
    int lp, lc, wp, wc;
    TEST_ASSERT_TRUE(tile_row(tile_with("Lead words"), &lp, &lc) >= 0);
    TEST_ASSERT_TRUE(tile_row(tile_with("Alpha"), &wp, &wc) >= 0);
    TEST_ASSERT_NOT_EQUAL(lp, wp); /* apart: the case a drop would be asked for */
    for (int j = 0; j < L.ntiles; j++) TEST_ASSERT_FALSE(L.tile[j].caption && !strcmp(L.tile[j].caption, "Alt cap"));
    /* a dock with no peer yet: the label, then only PORT; never COMPANION IP over the port */
    dui_box_t b = box_default();
    b.connected = b.ondemand = true;
    b.dock.peer[0] = '\0';
    static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063};
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        build(DUI_PAGE_HOME, &b);
        plan(PIDS[m]);
        for (int j = 0; j < L.ntiles; j++)
            TEST_ASSERT_FALSE(L.tile[j].caption && !strcmp(L.tile[j].caption, "Companion IP"));
    }
}

/* a toast always has a place, on every model and screen (homes, sub-screens, the Menu screen): the key status
 * row taken over (its outcome's tint and colour, the toast's words alone, never after the status or title words), the
 * Back key on a screen with no status row on the keys (the Mini's sub-screens, the Menu screen), the strip or the info
 * bar on the + / + XL / Neo (dui_render_bar). Coloured by outcome: green done, amber under way, red refused. */
static void test_toast_has_a_place(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063};
    static const int PAGES[] = {DUI_PAGE_HOME, DUI_PAGE_NETWORK, DUI_PAGE_MENU, DUI_PAGE_BOX};
    static const struct {
        const char *text;
        int state;
    } T[] = {{"Scan running", DUI_STATE_BAD}, {"Switching", DUI_STATE_WAIT}, {"Static 192.168.8.50", DUI_STATE_OK}};
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++)
        for (size_t pg = 0; pg < sizeof PAGES / sizeof PAGES[0]; pg++)
            for (size_t k = 0; k < sizeof T / sizeof T[0]; k++) {
                dui_box_t b = box_default();
                strcpy(b.toast, T[k].text);
                b.toast_state = (uint8_t)T[k].state;
                build(PAGES[pg], &b);
                plan(PIDS[m]);
                char msg[48];
                snprintf(msg, sizeof msg, "0x%04X page %d toast %d", PIDS[m], PAGES[pg], (int)k);
                TEST_ASSERT_EQUAL_MESSAGE(T[k].state, S.status.toast_state, msg);
                const int role = T[k].state == DUI_STATE_BAD ? DUI_ROLE_BAD : T[k].state == DUI_STATE_WAIT ? DUI_ROLE_WAIT : DUI_ROLE_OK;
                if (L.status_at != DUI_STATUS_KEYS) { /* the bar: in place of its text, in the outcome's colour */
                    const int w = 800, h = 100;
                    uint8_t *px = calloc((size_t)w * h, 3);
                    dui_render_bar(&S.status, S.sub, 0, 1, w, h, px);
                    const uint32_t want = dui__role_rgb(role);
                    int hits = 0;
                    for (int i = 0; i < w * h; i++)
                        hits += abs(px[3 * i] - (int)(want >> 16 & 255)) < 12 && abs(px[3 * i + 1] - (int)(want >> 8 & 255)) < 12 &&
                                abs(px[3 * i + 2] - (int)(want & 255)) < 12;
                    TEST_ASSERT_GREATER_THAN_MESSAGE(50, hits, msg);
                    free(px);
                    continue;
                }
                if (L.status_rows == 0) { /* the Back (Close) key carries it */
                    dui_tile_t t;
                    TEST_ASSERT_TRUE_MESSAGE(dui_layout_key(&L, 0, 0, 0, &t), msg);
                    TEST_ASSERT_TRUE_MESSAGE(t.flags & DUI_F_NAV, msg);
                    TEST_ASSERT_EQUAL_STRING_MESSAGE(T[k].text, t.text, msg);
                    TEST_ASSERT_EQUAL_MESSAGE(T[k].state, t.state, msg);
                    continue;
                }
                /* the status row: the toast's words in its keys (one or two), the row in the outcome's tint; no status or
                 * title word left in it */
                char got[48] = "";
                for (int c = 0; c < L.deck.cols; c++) {
                    dui_tile_t t;
                    if (!dui_layout_key(&L, 0, c, 0, &t)) continue;
                    if (t.kind == DUI_TILE_WORD) {
                        TEST_ASSERT_EQUAL_MESSAGE(role, t.role, msg);
                        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_TEXT_MIN_PX, t.fit.px, msg);
                        TEST_ASSERT_EQUAL_MESSAGE(T[k].state == DUI_STATE_BAD ? DUI_TINT_BAD : T[k].state == DUI_STATE_WAIT ? DUI_TINT_WAIT : DUI_TINT_OK,
                                                  t.tint, msg);
                        const size_t u = strlen(got);
                        snprintf(got + u, sizeof got - u, "%s%.*s", u ? " " : "", t.len ? t.len : (int)strlen(t.text), t.text);
                    }
                }
                TEST_ASSERT_EQUAL_STRING_MESSAGE(T[k].text, got, msg);
                fit_layout(&L.deck, &L, msg);
            }
}

/* the PIN pad names the action it unlocks ("PIN to restart", "PIN: Wi-Fi off"): on the status row (the 32-
 * key decks), over the dots (the 15-key decks), on the bar (+, + XL, Neo), over the roller's digit (the Mini: the
 * whole title, or the action alone, as fits); ⌫ shows "HOLD" and a ✕ where there is no ✕ key (the + / Neo pages, the
 * roller); the roller's add key reads "Next digit"; every key fits on every model and for every action. */
static void test_pin_titles(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063};
    static const struct {
        uint16_t action;
        const char *title, *shortt; /* the title, and its narrow keys' form */
    } A[] = {
        {DUI_A_RESTART, "PIN to restart", "PIN to restart"},           {DUI_A_SYNC, "PIN to sync", "PIN to sync"},
        {DUI_A_BOOT_OTHER, "PIN to boot other", "PIN to boot other"},  {DUI_A_MODE_SAT, "PIN to use Satellite", "PIN to Satellite"},
        {DUI_A_MODE_DOCK, "PIN to use Dock", "PIN to Dock"},           {DUI_A_USE_MAIN, "PIN to use Primary", "PIN to Primary"},
        {DUI_A_USE_BACKUP, "PIN to use Backup", "PIN to Backup"},      {DUI_A_ETH_DHCP, "PIN to use DHCP", "PIN to DHCP"},
        {DUI_A_ETH_STATIC, "PIN to make static", "PIN to static"},     {DUI_A_ETH_EDIT, "PIN to edit address", "PIN to edit IP"},
        {DUI_A_ETH_SAVE, "PIN to save address", "PIN to save IP"},     {DUI_A_AP_OFF, "PIN: hotspot off", "PIN: hotspot"},
        {DUI_A_WIFI_OFF, "PIN: Wi-Fi off", "PIN: Wi-Fi off"},          {DUI_A_WIFI_ON, "PIN: Wi-Fi on", "PIN: Wi-Fi on"},
        /* Set IP, its Save, Auto find */
        {DUI_A_COMP_EDIT, "PIN to set Companion IP", "PIN to set IP"}, {DUI_A_COMP_SAVE, "PIN to save Companion IP", "PIN to save IP"},
        {DUI_A_COMP_AUTO, "PIN to auto find", "PIN to auto find"},     {DUI_A_AP_ON, "PIN: hotspot on", "PIN: hotspot"},
        {0x0042, "Enter PIN", "Enter PIN"}};
    static dui_pin_t P;
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        const dui_deck_t d = deck_of(PIDS[m]);
        for (size_t a = 0; a < sizeof A / sizeof A[0]; a++) {
            dui_pin_out_t o;
            dui_pin_open(&P, &d, A[a].action, true, 0, 1000, &o);
            dui_pin_layout(&d, &P, 1000, &L);
            char msg[48], m2[48];
            snprintf(msg, sizeof msg, "0x%04X action 0x%x", PIDS[m], A[a].action);
            dui_pin_message(&P, 1000, m2, sizeof m2);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(A[a].title, m2, msg);
            fit_layout(&d, &L, msg);
            char shown[64] = "";
            bool del_tag = false, next_digit = false;
            for (int r = 0; r < d.rows; r++)
                for (int c = 0; c < d.cols; c++) {
                    dui_tile_t t;
                    if (!dui_layout_key(&L, 0, c, r, &t)) continue;
                    if (t.action == DUI_PIN_A_DEL && (t.flags & DUI_F_TAG)) del_tag = true;
                    if (t.action == DUI_PIN_A_ADD && t.text && !strcmp(t.text, "Next digit")) next_digit = true;
                    const char *txt = t.kind == DUI_TILE_WORD ? t.text : (t.kind == DUI_TILE_DOTS || t.kind == DUI_TILE_KEY) ? t.caption : NULL;
                    if (txt && r == 0) {
                        const size_t u = strlen(shown);
                        snprintf(shown + u, sizeof shown - u, "%s%s", u ? " " : "", txt);
                    }
                }
            switch (P.style) {
            case DUI_PIN_PHONE: TEST_ASSERT_EQUAL_STRING_MESSAGE(A[a].title, shown, msg); break; /* row 0's words */
            case DUI_PIN_ROWS: /* over the dots: the title, or its short form where the key is narrow */
                TEST_ASSERT_TRUE_MESSAGE(!strcmp(A[a].title, shown) || !strcmp(A[a].shortt, shown), msg);
                break;
            case DUI_PIN_ROLLER: /* the digit's caption: part of the title, never empty for a named action */
                TEST_ASSERT_TRUE_MESSAGE(next_digit && del_tag, msg);
                if (A[a].action != 0x0042)
                    TEST_ASSERT_TRUE_MESSAGE(shown[0] && (strstr(A[a].title, shown) || strstr(A[a].shortt, shown)), msg);
                break;
            case DUI_PIN_PAGES: TEST_ASSERT_TRUE_MESSAGE(del_tag, msg); break; /* the title on the bar (the message) */
            default: break;
            }
        }
    }
}

/* after the right PIN the unlocked key pulses cyan for 3 s (on, off, ... then as it was), and the notice
 * names it and how it goes: "Unlocked · hold Restart", "Unlocked · press Sync"; "Unlocked" when it isn't shown. */
static void test_unlock_pulse_and_note(void)
{
    dui_box_t b = box_default();
    b.box.slot = DUI_SLOT_DIFFERS;
    build(DUI_PAGE_BOX, &b);
    plan(0x00BA);
    char note[48];
    dui_unlock_note(&L, DUI_A_RESTART, note, sizeof note);
    TEST_ASSERT_EQUAL_STRING("Unlocked \xC2\xB7 hold Restart", note);
    dui_unlock_note(&L, DUI_A_SYNC, note, sizeof note);
    TEST_ASSERT_EQUAL_STRING("Unlocked \xC2\xB7 press Sync now", note);
    dui_unlock_note(&L, DUI_A_USE_MAIN, note, sizeof note);
    TEST_ASSERT_EQUAL_STRING("Unlocked", note);
    /* the pulse */
    dui_nav_t n;
    const dui_deck_t d = deck_of(0x00BA);
    dui_nav_init(&n, &d, 0);
    int rc = -1, rr = -1;
    for (int r = 0; r < d.rows && rc < 0; r++)
        for (int c = 0; c < d.cols; c++) {
            dui_tile_t t;
            if (dui_layout_key(&L, 0, c, r, &t) && t.action == DUI_A_RESTART) rc = c, rr = r;
        }
    TEST_ASSERT_TRUE(rc >= 0);
    dui_nav_pulse(&n, DUI_A_RESTART, 10000);
    int on = 0, off = 0;
    for (int64_t t = 10000; t < 10000 + DUI_UNLOCK_PULSE_MS; t += 50) {
        dui_tile_t k;
        dui_layout_key(&L, 0, rc, rr, &k);
        dui_nav_decorate(&n, rc, rr, t, &k);
        if (k.phase) on++;
        else off++;
    }
    TEST_ASSERT_TRUE(on > 10 && off > 10); /* it pulses */
    dui_tile_t k;
    dui_layout_key(&L, 0, rc, rr, &k);
    dui_nav_decorate(&n, rc, rr, 10000 + DUI_UNLOCK_PULSE_MS + 1, &k);
    TEST_ASSERT_EQUAL(0, k.phase); /* and stops */
    /* glowing, it is drawn cyan (its outline and label), its HOLD tag kept */
    dui_layout_key(&L, 0, rc, rr, &k);
    k.phase = 1;
    static uint8_t px[96 * 96 * 3];
    dui_render_key(&k, 96, px);
    int cyan = 0;
    for (int i = 0; i < 96 * 96; i++)
        cyan += abs(px[3 * i] - 0x5C) < 16 && abs(px[3 * i + 1] - 0xD6) < 16 && abs(px[3 * i + 2] - 0xF0) < 16;
    TEST_ASSERT_GREATER_THAN(150, cyan);
    /* the notice in the status row: "Unlocked" | "hold Restart", green */
    strcpy(b.toast, "Unlocked \xC2\xB7 hold Restart");
    b.toast_state = DUI_STATE_OK;
    build(DUI_PAGE_BOX, &b);
    plan(0x0080);
    dui_tile_t w1, w2;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 1, 0, &w1) && dui_layout_key(&L, 0, 2, 0, &w2));
    TEST_ASSERT_EQUAL(8, w1.len);
    TEST_ASSERT_EQUAL(0, strncmp(w1.text, "Unlocked", 8));
    TEST_ASSERT_EQUAL(12, w2.len);
    TEST_ASSERT_EQUAL(0, strncmp(w2.text, "hold Restart", 12));
    TEST_ASSERT_EQUAL(DUI_ROLE_OK, w1.role);
}

/* every refusal says why, in two words, from the RPC's error */
static void test_refusal_reasons(void)
{
    static const struct {
        int code;
        const char *msg, *want;
    } R[] = { /* the refusals' own words (rpc/refusals.h), as the box's methods and slotsync_err_text send them */
        {RPC_E_BAD_STATE, RPC_MSG_UPLOAD, "Upload running"},
        {RPC_E_BAD_STATE, RPC_MSG_SYNCING, "Sync running"},
        {RPC_E_BAD_STATE, RPC_MSG_CHECKING, "Still checking"},
        {RPC_E_BAD_STATE, RPC_MSG_SELF_CHECK, "Still checking"},
        {RPC_E_BAD_STATE, RPC_MSG_NEWER, "Other newer"},
        {RPC_E_BAD_STATE, RPC_MSG_EMPTY, "Slot empty"},
        {RPC_E_BAD_STATE, RPC_MSG_INVALID, "Not valid"},
        {RPC_E_BAD_STATE, RPC_MSG_FOREIGN, "Not ours"},
        {RPC_E_UNAVAILABLE, RPC_MSG_NO_BACKUP, "No backup"},
        {RPC_E_UNAVAILABLE, RPC_MSG_WIFI_OFF, "Wi-Fi off"},
        {RPC_E_UNAVAILABLE, RPC_MSG_NO_DECK, "No deck"},
        {RPC_E_UNAVAILABLE, RPC_MSG_NO_MEMORY, "No memory"},
        {RPC_E_BAD_STATE, RPC_MSG_RESTARTING, "Restarting already"},
        {RPC_E_INVALID_ARG, "ethIp: enter four numbers 0-255", "Not valid"},
        {RPC_E_UNAUTHORIZED, RPC_MSG_UPLOAD, "PIN needed"}, /* the code first: it says it all */
        {RPC_E_NOT_FOUND, "no Companion with id x is visible", "Not found"},
        {RPC_E_BAD_STATE, "something new", "Busy now"},
        {-1, "", "Not done"},
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        const char *r = dui_refusal(R[i].code, R[i].msg);
        TEST_ASSERT_EQUAL_STRING(R[i].want, r);
        int spaces = 0;
        for (const char *p = r; *p; p++) spaces += *p == ' ';
        TEST_ASSERT_EQUAL_MESSAGE(1, spaces, r); /* two words */
    }
}

/* "Companion connected": satellite, the target and its address; dock, the peer and the port. Close
 * only on demand. */
static void test_home_connected_and_close(void)
{
    dui_box_t b = sat(DUI_SAT_CONNECTED);
    b.connected = b.ondemand = true;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("Connected Companion|IP:192|168|8|155 / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL_STRING("Connected", words(&S, false));
    TEST_ASSERT_EQUAL_STRING("Companion connected", S.status.text);
    TEST_ASSERT_EQUAL(DUI_STATE_OK, S.status.state);
    TEST_ASSERT_TRUE(S.status.close);
    plan(PID_M32); /* the Close key, top-left */
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 0, 0, &t));
    TEST_ASSERT_EQUAL(DUI_ACT_CLOSE, t.action);
    plan(PID_PLUS);
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 0, 0, &t));
    TEST_ASSERT_EQUAL(DUI_ACT_CLOSE, t.action);

    b = box_default(); /* dock */
    strcpy(b.dock.peer, "192.168.8.155");
    b.connected = b.ondemand = true;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL_STRING("Connected Companion|IP:192|168|8|155|Port:5343 / " HOTSPOT, content(&S));
    TEST_ASSERT_EQUAL(DUI_STATE_OK, S.status.state);

    /* not on demand: no Close, on any home */
    b.ondemand = false;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_FALSE(S.status.close);
    plan(PID_M32);
    TEST_ASSERT_EQUAL(-1, L.back_col);
    for (int st = DUI_SAT_NONET; st <= DUI_SAT_CONNECTED; st++) {
        b = sat(st);
        build(DUI_PAGE_HOME, &b);
        TEST_ASSERT_FALSE(S.status.close);
        b.ondemand = true;
        build(DUI_PAGE_HOME, &b);
        TEST_ASSERT_TRUE(S.status.close);
    }
    /* sub-screens: Back, never Close */
    b.ondemand = true;
    build(DUI_PAGE_BOX, &b);
    plan(PID_M32);
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 0, 0, &t));
    TEST_ASSERT_EQUAL(DUI_ACT_BACK, t.action);
}

/* ---- Menu ---- */

static void test_menu_buttons_and_dots(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(4, S.nmenu);
    TEST_ASSERT_EQUAL(DUI_A_GO_NETWORK, menu_button(&S, "Network")->action);
    TEST_ASSERT_EQUAL(DUI_A_GO_COMPANION, menu_button(&S, "Companion")->action);
    TEST_ASSERT_EQUAL(DUI_A_GO_HOTSPOT, menu_button(&S, "Hotspot")->action);
    TEST_ASSERT_EQUAL(DUI_A_GO_BOX, menu_button(&S, "Box")->action);
    TEST_ASSERT_EQUAL(DUI_ICON_CIRCUITRY, menu_button(&S, "Box")->icon); /* Phosphor's Circuitry */
    TEST_ASSERT_EQUAL(DUI_STATE_NONE, menu_button(&S, "Network")->state);
    TEST_ASSERT_EQUAL(DUI_STATE_WAIT, menu_button(&S, "Companion")->state);
    TEST_ASSERT_EQUAL(DUI_STATE_OK, menu_button(&S, "Hotspot")->state);
    TEST_ASSERT_EQUAL(DUI_STATE_NONE, menu_button(&S, "Box")->state);
    b.net.ap_on = false;
    b.box.slot = DUI_SLOT_DIFFERS;
    b.dock.no_free = true;
    build(DUI_PAGE_HOME, &b);
    TEST_ASSERT_EQUAL(DUI_STATE_BAD, menu_button(&S, "Companion")->state);
    TEST_ASSERT_EQUAL(DUI_STATE_NONE, menu_button(&S, "Hotspot")->state);
    TEST_ASSERT_EQUAL(DUI_STATE_WAIT, menu_button(&S, "Box")->state);
    /* the Menu screen: Back and the same four, the whole grid */
    build(DUI_PAGE_MENU, &b);
    TEST_ASSERT_TRUE(S.sub);
    TEST_ASSERT_TRUE(S.full);
    TEST_ASSERT_EQUAL_STRING("Menu", S.status.title[0]);
    TEST_ASSERT_EQUAL_STRING("[Network]|[Companion]|[Hotspot]|[Box]", content(&S));
    TEST_ASSERT_EQUAL(DUI_STATE_WAIT, button(&S, "Box")->state);
    TEST_ASSERT_EQUAL(DUI_A_GO_BOX, button(&S, "Box")->action);
}

/* ---- Network ---- */

#define NAME_ROW " / Name:CompanionBox-8D1C|.local"

static void test_network_wifi_rows(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Simbox-Home|In use|Signal:-58|IP:192|168|8|225 / "
                             "(Backup)|Office-5G|Standby|[Use] / [Wi-Fi]" NAME_ROW,
                             content(&S));
    TEST_ASSERT_EQUAL(DUI_ROLE_OK, find(&S, NULL, "In use")->role);
    TEST_ASSERT_TRUE(S.sub);
    TEST_ASSERT_EQUAL_STRING("Network", S.status.title[0]);
    TEST_ASSERT_TRUE(find(&S, NULL, "Ethernet")->flags & DUI_F_DIM);
    TEST_ASSERT_FALSE(find(&S, NULL, "Primary")->flags & DUI_F_DIM);
    TEST_ASSERT_EQUAL(-58, find(&S, NULL, "Primary")->rssi);
    TEST_ASSERT_TRUE(find(&S, NULL, "Backup")->flags & DUI_F_DIM);
    TEST_ASSERT_EQUAL(DUI_ROLE_LABEL, find(&S, NULL, "Standby")->role);

    b.net.wifi_net = DUI_WIFI_BACKUP; /* on the backup */
    b.net.rssi = -66;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Simbox-Home|Standby|[Use] / "
                             "(Backup)|Office-5G|In use|Signal:-66|IP:192|168|8|225 / [Wi-Fi]" NAME_ROW,
                             content(&S));
    TEST_ASSERT_TRUE(find(&S, NULL, "Primary")->flags & DUI_F_DIM);
    TEST_ASSERT_FALSE(find(&S, NULL, "Backup")->flags & DUI_F_DIM);

    b.net.wifi_net = DUI_WIFI_NONE; /* neither joined: the main network being tried */
    b.net.wifi_ip[0] = '\0';
    b.net.wifi_trying = DUI_WIFI_MAIN;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Simbox-Home|Joining|[Use] / "
                             "(Backup)|Office-5G|Standby|[Use] / [Wi-Fi]" NAME_ROW,
                             content(&S));
    TEST_ASSERT_EQUAL(DUI_ROLE_WAIT, find(&S, NULL, "Joining")->role);
    /* "Not found" only once the driver has said so; then the backup is tried */
    b.net.wifi_not_found = 1u << (DUI_WIFI_MAIN - 1);
    b.net.wifi_trying = DUI_WIFI_BACKUP;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Simbox-Home|Not found|[Use] / "
                             "(Backup)|Office-5G|Joining|[Use] / [Wi-Fi]" NAME_ROW,
                             content(&S));
    TEST_ASSERT_EQUAL(DUI_ROLE_BAD, find(&S, NULL, "Not found")->role);
    b.net.wifi_not_found |= 1u << (DUI_WIFI_BACKUP - 1);
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Simbox-Home|Not found|[Use] / "
                             "(Backup)|Office-5G|Not found|[Use] / [Wi-Fi]" NAME_ROW,
                             content(&S));
    /* the radio down (the co-processor not answering): said so, on both */
    b.net.radio_down = true;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Simbox-Home|Radio down / "
                             "(Backup)|Office-5G|Radio down / [Wi-Fi]" NAME_ROW, /* no radio: nothing to switch */
                             content(&S));
    TEST_ASSERT_EQUAL(DUI_ROLE_BAD, find(&S, NULL, "Radio down")->role);
    b.net.radio_down = false;
    b.net.wifi_not_found = 0;
    b.net.wifi_trying = DUI_WIFI_NONE;

    b.net.eth_link = true; /* Ethernet up, Wi-Fi joined without an address yet */
    strcpy(b.net.eth_ip, "10.0.0.7");
    b.net.wifi_net = DUI_WIFI_MAIN;
    b.net.rssi = -58;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|Up|[Set on web page]|[Edit]|IP:10|0|0|7 / (Primary)|Simbox-Home|In use|Signal:-58 / "
                             "(Backup)|Office-5G|Standby|[Use] / [Wi-Fi]" NAME_ROW,
                             content(&S));
    TEST_ASSERT_FALSE(find(&S, NULL, "Ethernet")->flags & DUI_F_DIM);
    TEST_ASSERT_EQUAL(DUI_ROLE_OK, find(&S, NULL, "Up")->role);

    b = box_default(); /* no backup network; no Wi-Fi at all */
    b.net.wifi_backup[0] = '\0';
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Simbox-Home|In use|Signal:-58|IP:192|168|8|225 / "
                             "(Backup)|Not set / [Wi-Fi]" NAME_ROW, /* Not set, and no Use */
                             content(&S));
    b.net.wifi_main[0] = '\0';
    b.net.wifi_net = DUI_WIFI_NONE;
    b.net.wifi_ip[0] = '\0';
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Not set / (Backup)|Not set" NAME_ROW, content(&S));
}

/* the Network screen's key map. Where map row `row`'s role `role` must be on deck pid (page, row, column;
 * the k-th key of a two-key name or address one column on), the same in every state. */
static bool map_want(uint16_t pid, int row, int role, int *p, int *r, int *c)
{
    if (role == DUI_MC_TOGGLE) {
        switch (pid) {
        case 0x00BA: *p = 0, *r = 0, *c = 5; return true; /* the status row */
        case 0x00C6: *p = 0, *r = 0, *c = 8; return true;
        case 0x0080: *p = 1, *r = 2, *c = 3; return true; /* Main's page */
        case 0x0084:
        case 0x009A: *p = 1, *r = 1, *c = 1; return true;
        default: *p = 3, *r = 0, *c = 1; return true;    /* the Mini: Main's detail page */
        }
    }
    const int n = row - DUI_MAP_ETH; /* 0, 1, 2 */
    switch (pid) {
    case 0x00BA:
    case 0x00C6: {
        static const int col[] = {0, 0, 1, 3, 4, 5, 6};
        *p = 0, *r = row, *c = col[role];
        return true;
    }
    case 0x0080: {
        static const int rr[] = {0, 1, 1, 1, 1, 2, 2}, cc[] = {0, 0, 1, 3, 4, 0, 1};
        *p = n, *r = rr[role], *c = cc[role];
        return true;
    }
    case 0x0084:
    case 0x009A: {
        static const int rr[] = {0, 0, 0, 0, 1, 1, 1}, cc[] = {0, 1, 2, 3, 0, 1, 2};
        if (role == DUI_MC_AUX && row != DUI_MAP_ETH) return false; /* the signal: in the icon's bars */
        *p = n, *r = rr[role], *c = cc[role];
        return true;
    }
    default: { /* the Mini: a network's page (icon, name; state, action), then its details (Edit / signal; address) */
        const int main = 2 * n, det = 2 * n + 1;
        switch (role) {
        case DUI_MC_ICON: *p = main, *r = 0, *c = 1; return true;
        case DUI_MC_NAME: *p = main, *r = 0, *c = 2; return true;
        case DUI_MC_STATE: *p = main, *r = 1, *c = 0; return true;
        case DUI_MC_ACTION: *p = main, *r = 1, *c = 1; return true;
        case DUI_MC_AUX: *p = det, *r = 0, *c = row == DUI_MAP_ETH ? 1 : 2; return true;
        default: *p = det, *r = 1, *c = 0; return true; /* the address */
        }
    }
    }
}

/* on every model and in every state, each network's keys stay where its map puts them: Ethernet, Main and
 * Backup in that order, the icon, the name, the state, the action, Edit / the signal, the address; the Wi-Fi key in
 * one place; the ">" key in one place. A Wi-Fi name of two words takes two keys where the map gives it two. */
static void test_network_keymap(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063};
    dui_box_t st[10];
    int ns = 0;
    st[ns++] = box_default(); /* Main in use, Backup on standby, no cable */
    st[ns] = box_default(), st[ns].net.wifi_net = DUI_WIFI_BACKUP, ns++;
    st[ns] = box_default(), st[ns].net.wifi_net = DUI_WIFI_NONE, st[ns].net.wifi_ip[0] = '\0',
    st[ns].net.wifi_trying = DUI_WIFI_MAIN, ns++;
    st[ns] = st[ns - 1], st[ns].net.wifi_not_found = 3, st[ns].net.radio_down = true, ns++;
    st[ns] = box_default(), st[ns].net.eth_link = true, strcpy(st[ns].net.eth_ip, "10.0.0.7"), st[ns].net.eth_dhcp = false, ns++;
    st[ns] = box_default(), st[ns].net.eth_static_ok = true, ns++; /* Make static */
    st[ns] = box_default(), st[ns].net.wifi_off = true, ns++;
    st[ns] = box_default(), st[ns].locked = true, ns++;
    st[ns] = box_default(), st[ns].net.wifi_backup[0] = '\0', ns++;
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++)
        for (int k = 0; k < ns; k++) {
            build(DUI_PAGE_NETWORK, &st[k]);
            plan(PIDS[m]);
            int checked = 0;
            for (int row = DUI_MAP_ETH; row <= DUI_MAP_BACKUP; row++)
                for (int role = DUI_MC_ICON; role <= DUI_MC_TOGGLE; role++) {
                    if (role == DUI_MC_TOGGLE && row != DUI_MAP_MAIN) continue;
                    int ti = -1;
                    for (int j = 0; j < L.ntiles && ti < 0; j++)
                        if (L.tile[j].map == DUI_MAP(row, role)) ti = j;
                    int wp, wr, wc;
                    const bool shown = map_want(PIDS[m], row, role, &wp, &wr, &wc);
                    char msg[48];
                    snprintf(msg, sizeof msg, "0x%04X state %d row %d role %d", PIDS[m], k, row, role);
                    if (ti < 0) continue; /* not in this state (an address, Use, the signal) */
                    int p, c;
                    const int r = tile_row(ti, &p, &c);
                    if (!shown) {
                        TEST_ASSERT_EQUAL_MESSAGE(-1, r, msg);
                        continue;
                    }
                    TEST_ASSERT_EQUAL_MESSAGE(wp, p, msg);
                    TEST_ASSERT_EQUAL_MESSAGE(wr, r, msg);
                    TEST_ASSERT_EQUAL_MESSAGE(wc, c, msg);
                    checked++;
                }
            TEST_ASSERT_GREATER_OR_EQUAL(9, checked); /* icons, states, the toggle, ... */
            fit_layout(&L.deck, &L, "network keymap");
        }
    /* Ethernet linked with its address and Main joined with its own: both rows' addresses at once, each at its
     * own keys (the map gives every row its address keys: on its row on the wide decks, on its page or detail page on
     * the others), every key of each address placed, no two on one key */
    dui_box_t both = box_default();
    both.net.eth_link = true;
    strcpy(both.net.eth_ip, "192.168.100.200");
    strcpy(both.net.wifi_ip, "192.168.8.225");
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        build(DUI_PAGE_NETWORK, &both);
        plan(PIDS[m]);
        int keys[2][4], nk[2] = {0, 0};
        for (int w = 0; w < 2; w++) {
            const int row = w ? DUI_MAP_MAIN : DUI_MAP_ETH;
            int wp, wr, wc;
            TEST_ASSERT_TRUE(map_want(PIDS[m], row, DUI_MC_IP, &wp, &wr, &wc));
            for (int j = 0; j < L.ntiles; j++) {
                if (L.tile[j].map != DUI_MAP(row, DUI_MC_IP)) continue;
                int p, c;
                const int r = tile_row(j, &p, &c);
                char msg[48];
                snprintf(msg, sizeof msg, "0x%04X both addresses: row %d key %d", PIDS[m], row, nk[w]);
                TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(0, r, msg);
                if (nk[w] == 0) { /* the first key where the map puts it */
                    TEST_ASSERT_EQUAL_MESSAGE(wp, p, msg);
                    TEST_ASSERT_EQUAL_MESSAGE(wr, r, msg);
                    TEST_ASSERT_EQUAL_MESSAGE(wc, c, msg);
                }
                TEST_ASSERT_LESS_THAN(4, nk[w]);
                keys[w][nk[w]++] = (p * L.deck.rows + r) * L.deck.cols + c;
            }
            TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(1, nk[w], "an address not shown");
        }
        for (int a = 0; a < nk[0]; a++)
            for (int b2 = 0; b2 < nk[1]; b2++) TEST_ASSERT_NOT_EQUAL(keys[0][a], keys[1][b2]);
        fit_layout(&L.deck, &L, "network both addresses");
    }
    /* ... and with a long Wi-Fi name over its keys: both addresses, no overflow */
    strcpy(both.net.wifi_main, "Studio-Upstairs-Wireless-5G");
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        build(DUI_PAGE_NETWORK, &both);
        plan(PIDS[m]); /* asserts no overflow */
        int shown = 0;
        for (int j = 0; j < L.ntiles; j++) {
            int p, c;
            if ((L.tile[j].map == DUI_MAP(DUI_MAP_ETH, DUI_MC_IP) || L.tile[j].map == DUI_MAP(DUI_MAP_MAIN, DUI_MC_IP)) &&
                tile_row(j, &p, &c) >= 0)
                shown |= L.tile[j].map == DUI_MAP(DUI_MAP_ETH, DUI_MC_IP) ? 1 : 2;
        }
        TEST_ASSERT_EQUAL_MESSAGE(3, shown, "a long name: both addresses");
        fit_layout(&L.deck, &L, "network both addresses, a long name");
    }
    /* the two-word name on two keys, side by side, where the map gives two; Ethernet's Edit on Ethernet's row */
    dui_box_t b = box_default();
    strcpy(b.net.wifi_main, "Simbox Home");
    build(DUI_PAGE_NETWORK, &b);
    plan(0x00BA);
    int p0, c0, p1, c1;
    TEST_ASSERT_EQUAL(2, tile_row(tile_with("Simbox"), &p0, &c0));
    TEST_ASSERT_EQUAL(2, tile_row(tile_with("Home"), &p1, &c1));
    TEST_ASSERT_TRUE(c0 == 1 && c1 == 2);
    TEST_ASSERT_EQUAL(1, tile_row(tile_with("Edit"), &p0, &c0));
    TEST_ASSERT_EQUAL(5, c0);
    TEST_ASSERT_EQUAL(4, L.pager_col); /* the ">" key in the status row */
    TEST_ASSERT_EQUAL(0, L.pager_row);
    plan(0x0080);
    TEST_ASSERT_TRUE(L.pager_col == 4 && L.pager_row == 2 && L.pages == 4);
    plan(0x0063);
    TEST_ASSERT_TRUE(L.pager_col == 2 && L.pager_row == 1);
}

/* the Wi-Fi station on / off: a power toggle on the Wi-Fi rows, selected while on; off is a 1.5 s hold
 * ("Hold 1.5 s to turn off"), on a tap; both locked under the PIN. While off both rows say "Off" (grey) and no Use. */
static void test_wifi_toggle(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_NETWORK, &b);
    const dui_tile_t *t = button(&S, "Wi-Fi");
    TEST_ASSERT_EQUAL(DUI_A_WIFI_OFF, t->action);
    TEST_ASSERT_EQUAL(DUI_ICON_POWER, t->icon);
    TEST_ASSERT_TRUE(t->flags & DUI_F_ON);
    TEST_ASSERT_TRUE(t->flags & DUI_F_SELECTED);
    TEST_ASSERT_EQUAL(DUI_HOLD_WIFI_MS, t->hold_ms);
    TEST_ASSERT_EQUAL(1500, DUI_HOLD_WIFI_MS);
    TEST_ASSERT_EQUAL_STRING("Wi-Fi off", t->hint);
    TEST_ASSERT_FALSE(t->flags & DUI_F_LOCKED);

    b.net.wifi_off = true; /* off: both rows "Off", no Use, no signal or address; the toggle turns it on with a tap */
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("(Ethernet)|Address:DHCP|No cable|[Set on web page]|[Edit] / (Primary)|Simbox-Home|Off / "
                             "(Backup)|Office-5G|Off / [Wi-Fi]" NAME_ROW,
                             content(&S));
    TEST_ASSERT_EQUAL(DUI_ROLE_LABEL, find(&S, NULL, "Off")->role);
    TEST_ASSERT_NULL(maybe_button(&S, "Use"));
    TEST_ASSERT_TRUE(find(&S, NULL, "Primary")->flags & DUI_F_DIM);
    TEST_ASSERT_EQUAL(0, find(&S, NULL, "Primary")->rssi);
    t = button(&S, "Wi-Fi");
    TEST_ASSERT_EQUAL(DUI_A_WIFI_ON, t->action);
    TEST_ASSERT_EQUAL(0, t->hold_ms); /* a tap */
    TEST_ASSERT_FALSE(t->flags & (DUI_F_ON | DUI_F_SELECTED));

    b.locked = true; /* under the PIN: both directions locked */
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_TRUE(button(&S, "Wi-Fi")->flags & DUI_F_LOCKED);
    b.net.wifi_off = false;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_TRUE(button(&S, "Wi-Fi")->flags & DUI_F_LOCKED);
    TEST_ASSERT_FALSE(dui_action_allowed(DUI_A_WIFI_OFF, true, false));
    TEST_ASSERT_TRUE(dui_action_allowed(DUI_A_WIFI_ON, true, true)); /* the PIN pad unlocks it */

    b = box_default(); /* no Wi-Fi set up: nothing to turn on or off */
    b.net.wifi_main[0] = '\0';
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_NULL(maybe_button(&S, "Wi-Fi"));
}

/* A Wi-Fi name's words are separated by whitespace only, one a key, on every deck; a word
 * is never split over keys (2 lines inside its key at its natural breaks); a password stays whole. */
static void test_wifi_words(void)
{
    int st[8], ln[8];
    char w[8][24];
    const char *cases[][5] = {
        {"Simbox Home", "Simbox", "Home", NULL},
        {"CompanionBox-8D1C", "CompanionBox-8D1C", NULL},
        {"My Wifi 5G", "My", "Wifi", "5G", NULL},
        {"  a_b.c  Home\tX", "a_b.c", "Home", "X", NULL},
        {"Simbox-Home", "Simbox-Home", NULL},
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const int n = dui_split_words(cases[k][0], (int)strlen(cases[k][0]), st, ln, 8);
        int want = 0;
        while (want < 4 && cases[k][want + 1]) want++;
        TEST_ASSERT_EQUAL_MESSAGE(want, n, cases[k][0]);
        for (int i = 0; i < n; i++) {
            snprintf(w[i], sizeof w[i], "%.*s", ln[i], cases[k][0] + st[i]);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[k][i + 1], w[i], cases[k][0]);
        }
    }
    TEST_ASSERT_EQUAL(0, dui_split_words("", 0, st, ln, 8));
    TEST_ASSERT_EQUAL(0, dui_split_words("   ", 3, st, ln, 8));
    const char *many = "a b c d"; /* more words than places: the last takes the rest */
    TEST_ASSERT_EQUAL(2, dui_split_words(many, 7, st, ln, 2));
    TEST_ASSERT_EQUAL(5, ln[1]);
    /* the screens, on every model: the hotspot name "CompanionBox-8D1C" one key ("Companion" / "Box-8D1C"),
     * never pieces over keys, the big decks too; "Simbox Home" two keys; a password whole */
    static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063};
    dui_box_t b = box_default();
    strcpy(b.net.wifi_backup, "Simbox Home");
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        static const int PAGES[] = {DUI_PAGE_HOTSPOT, DUI_PAGE_NETWORK};
        for (size_t p = 0; p < 2; p++) {
            build(PAGES[p], &b);
            plan(PIDS[m]);
            char msg[32];
            snprintf(msg, sizeof msg, "0x%04X page %d", PIDS[m], PAGES[p]);
            bool ssid_whole = false, word1 = false, word2 = false;
            for (int j = 0; j < L.ntiles; j++) {
                const dui_tile_t *t = &L.tile[j];
                const int n = t->text ? (t->len ? t->len : (int)strlen(t->text)) : 0;
                if (n == 17 && !strncmp(t->text, "CompanionBox-8D1C", 17)) {
                    ssid_whole = true;
                    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(2, t->fit.nl, msg);
                }
                if (n == 6 && !strncmp(t->text, "Simbox", 6)) word1 = true;
                if (n == 4 && !strncmp(t->text, "Home", 4)) word2 = true;
                if (PAGES[p] == DUI_PAGE_HOTSPOT) /* no piece of the hotspot's name (Network has the box's name) */
                    TEST_ASSERT_FALSE_MESSAGE(n > 0 && n < 17 && !strncmp(t->text, "CompanionBox", 12), msg);
            }
            if (PAGES[p] == DUI_PAGE_HOTSPOT) TEST_ASSERT_TRUE_MESSAGE(ssid_whole, msg);
            if (PAGES[p] == DUI_PAGE_NETWORK) {
                /* The key map: the name's keys (two on the wide and 15-key decks, a word a key, side by
                 * side; one on the 4- and 6-key decks, the two words on two lines in it) */
                const bool two = PIDS[m] == 0x00BA || PIDS[m] == 0x00C6 || PIDS[m] == 0x0080;
                if (two) {
                    TEST_ASSERT_TRUE_MESSAGE(word1 && word2, msg);
                    int p0, c0, p1, c1;
                    const int r0 = tile_row(tile_with("Simbox"), &p0, &c0), r1 = tile_row(tile_with("Home"), &p1, &c1);
                    TEST_ASSERT_TRUE_MESSAGE(r0 >= 0 && r1 == r0 && p1 == p0 && c1 == c0 + 1, msg);
                } else {
                    const int w = tile_with("Simbox Home");
                    TEST_ASSERT_TRUE_MESSAGE(w >= 0 && L.tile[w].fit.nl == 2 && L.tile[w].fit.px >= DUI_TEXT_MIN_PX, msg);
                }
            }
        }
    }
    /* a word too long for its key in a multi-word SSID stays one key at 16 px or more, in two
     * lines of its own characters (no '-'), the next word beside it on the same row, on every model; the 4- and 6-key
     * decks' key map has one name key, where the whole name reads on two lines at 16 px */
    strcpy(b.net.wifi_backup, "Supercalifragilistic Home");
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        build(DUI_PAGE_NETWORK, &b);
        plan(PIDS[m]);
        char msg[24];
        snprintf(msg, sizeof msg, "long 0x%04X", PIDS[m]);
        const int one = tile_with("Supercalifragilistic Home"); /* a 4- or 6-key deck's one name key */
        if (one >= 0) {
            TEST_ASSERT_FALSE_MESSAGE(PIDS[m] == 0x00BA || PIDS[m] == 0x00C6 || PIDS[m] == 0x0080, msg);
            TEST_ASSERT_TRUE_MESSAGE(L.tile[one].fit.px >= 16 && L.tile[one].fit.nl == 2 && !L.tile[one].fit.hyph, msg);
            continue;
        }
        const int wi = tile_with("Supercalifragilistic"), hi = tile_with("Home");
        TEST_ASSERT_TRUE_MESSAGE(wi >= 0 && hi >= 0, msg);
        const dui_tile_t *w = &L.tile[wi];
        TEST_ASSERT_TRUE_MESSAGE(dui__value_fits(w->text, 20, deck_of(PIDS[m]).key_px), msg);
        TEST_ASSERT_TRUE_MESSAGE(w->fit.px >= 16, msg);
        TEST_ASSERT_EQUAL_MESSAGE(0, w->fit.hyph, msg);
        int p0, c0, p1, c1;
        const int r0 = tile_row(wi, &p0, &c0), r1 = tile_row(hi, &p1, &c1);
        TEST_ASSERT_TRUE_MESSAGE(r0 >= 0 && r1 == r0 && p1 == p0 && c1 == c0 + 1, msg);
    }
    strcpy(b.net.wifi_backup, "Simbox Home");
    build(DUI_PAGE_HOTSPOT, &b);
    TEST_ASSERT_NOT_NULL(find(&S, "Password", "k7m2p9xq"));
    TEST_ASSERT_TRUE(find(&S, "Password", "k7m2p9xq")->flags & DUI_F_WHOLE); /* on one key, as a Wi-Fi name's word */
    /* nothing is ever added to a value: an SSID word with no natural break wraps inside its key with
     * no '-', its two lines together the word itself; the plan's fit (dui__value_fits) and the render agree */
    strcpy(b.net.ap_ssid, "Supercalifragilistic");
    static const uint16_t SMALL[] = {0x0080, 0x0063, 0x009A, 0x00BA};
    for (size_t m = 0; m < sizeof SMALL / sizeof SMALL[0]; m++) {
        build(DUI_PAGE_HOTSPOT, &b);
        plan(SMALL[m]);
        const dui_deck_t d = deck_of(SMALL[m]);
        const dui_tile_t *w = NULL;
        for (int j = 0; j < L.ntiles && !w; j++)
            if (L.tile[j].text && L.tile[j].len == 20 && !strncmp(L.tile[j].text, "Supercalifragilistic", 20)) w = &L.tile[j];
        char msg[24];
        snprintf(msg, sizeof msg, "0x%04X", SMALL[m]);
        TEST_ASSERT_NOT_NULL_MESSAGE(w, msg); /* one key: it reads at 16 px on 2 lines */
        TEST_ASSERT_TRUE_MESSAGE(dui__value_fits(w->text, 20, d.key_px), msg);
        TEST_ASSERT_EQUAL_MESSAGE(0, w->fit.hyph, msg);
        if (w->fit.nl == 2) { /* the two lines: the word's own characters, in order, nothing added or lost */
            char joined[32];
            snprintf(joined, sizeof joined, "%.*s%.*s", w->fit.cut, w->text, 20 - w->fit.cut, w->text + w->fit.cut);
            TEST_ASSERT_EQUAL_STRING_MESSAGE("Supercalifragilistic", joined, msg);
            TEST_ASSERT_TRUE_MESSAGE(w->fit.cut >= 6 && w->fit.cut <= 14, msg); /* about even */
        }
    }
}

/* With the string pool full, an SSID's words and an address's octets add no tile (none pointing past
 * the "" dui_str then gives) and the screen says it overflowed. */
static void test_pieces_pool_full(void)
{
    static dui_screen_t s;
    dui_screen_init(&s);
    dui_group(&s, DUI_ZONE_MAIN, 0);
    s.pool_used = (uint16_t)(sizeof s.pool - 4); /* room for 3 characters and the NUL */
    dui_words(&s, NULL, "Simbox Home", DUI_ROLE_VALUE);
    dui_ipv4(&s, NULL, "192.168.1.20");
    TEST_ASSERT_TRUE(s.overflow);
    TEST_ASSERT_EQUAL(0, s.ntiles);
}

/* ---- Companion ---- */

static void test_companion(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_EQUAL_STRING("Mode|[Satellite]|[Dock] / Port:5343|Waiting", content(&S));
    TEST_ASSERT_EQUAL_STRING("Companion", S.status.title[0]);
    const dui_tile_t *t = button(&S, "Dock");
    TEST_ASSERT_TRUE(t->flags & DUI_F_SELECTED);
    TEST_ASSERT_EQUAL(0, t->hold_ms);
    TEST_ASSERT_EQUAL(DUI_A_NONE, t->action);
    t = button(&S, "Satellite");
    TEST_ASSERT_FALSE(t->flags & (DUI_F_SELECTED | DUI_F_DANGER));
    TEST_ASSERT_EQUAL(DUI_HOLD_MODE_MS, t->hold_ms);
    TEST_ASSERT_EQUAL_STRING("restarts", t->hint); /* "Hold 1.5 s" / "restarts" */
    TEST_ASSERT_EQUAL(DUI_A_MODE_SAT, t->action);
    TEST_ASSERT_EQUAL(DUI_ICON_SAT, t->icon); /* the satellite glyph; Dock keeps its icon */
    TEST_ASSERT_EQUAL(DUI_ICON_DOCK, button(&S, "Dock")->icon);
    TEST_ASSERT_EQUAL(DUI_ROLE_WAIT, find(&S, NULL, "Waiting")->role);
    strcpy(b.dock.peer, "192.168.8.155");
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_EQUAL_STRING("Mode|[Satellite]|[Dock] / Port:5343|Connected", content(&S));
    b.dock.no_free = true;
    b.dock.port = 0;
    b.dock.peer[0] = '\0';
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_EQUAL_STRING("Mode|[Satellite]|[Dock] / No dock", content(&S));
    /* dock mode has neither Set IP nor Auto find, a manual Companion or not, locked or not */
    for (int manual = 0; manual < 2; manual++) {
        b.sat.auto_disc = !manual;
        b.locked = manual;
        build(DUI_PAGE_COMPANION, &b);
        TEST_ASSERT_NULL(maybe_button(&S, "Set IP"));
        TEST_ASSERT_NULL(maybe_button(&S, "Auto find"));
    }
    b.locked = false;

    b = sat(DUI_SAT_FINDING);
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_EQUAL_STRING("Mode|[Satellite]|[Dock] / Companion:Studio-Mac|IP:192|168|8|155|Finding / "
                             "[Retry now]|[Choose]|[Set IP]",
                             content(&S));
    /* Set IP, always there in Satellite mode, a tap that opens the editor (DUI_A_COMP_EDIT) */
    t = button(&S, "Set IP");
    TEST_ASSERT_EQUAL(DUI_A_COMP_EDIT, t->action);
    TEST_ASSERT_EQUAL(DUI_ICON_EDIT, t->icon);
    TEST_ASSERT_EQUAL(0, t->hold_ms);
    TEST_ASSERT_FALSE(t->flags & DUI_F_LOCKED);
    TEST_ASSERT_NULL(maybe_button(&S, "Auto find")); /* auto-discovery on: Choose, no Auto find */
    TEST_ASSERT_TRUE(button(&S, "Satellite")->flags & DUI_F_SELECTED);
    t = button(&S, "Dock");
    TEST_ASSERT_EQUAL(DUI_HOLD_MODE_MS, t->hold_ms);
    TEST_ASSERT_EQUAL(DUI_A_MODE_DOCK, t->action);
    TEST_ASSERT_EQUAL(DUI_A_RETRY, button(&S, "Retry now")->action);
    TEST_ASSERT_EQUAL(0, button(&S, "Retry now")->hold_ms);
    TEST_ASSERT_EQUAL(DUI_A_GO_CHOOSE, button(&S, "Choose")->action); /* the pick list, nothing saved */
    TEST_ASSERT_EQUAL(DUI_ROLE_WAIT, find(&S, NULL, "Finding")->role);
    /* a manual address (no auto-discovery): no Choose */
    b.sat.auto_disc = false;
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_EQUAL_STRING("Mode|[Satellite]|[Dock] / Companion:Studio-Mac|IP:192|168|8|155|Finding / "
                             "[Retry now]|[Auto find]|[Set IP]",
                             content(&S));
    TEST_ASSERT_NULL(maybe_button(&S, "Choose"));
    t = button(&S, "Auto find"); /* a manual Companion: auto-discovery on again, a tap */
    TEST_ASSERT_EQUAL(DUI_A_COMP_AUTO, t->action);
    TEST_ASSERT_EQUAL(DUI_ICON_SEARCH, t->icon); /* its own, the magnifying glass (Choose keeps the list) */
    TEST_ASSERT_EQUAL(0, t->hold_ms);
    TEST_ASSERT_FALSE(t->flags & DUI_F_LOCKED);
    b.locked = true; /* both locked under the PIN (the pad first); Retry is not */
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_TRUE(button(&S, "Set IP")->flags & DUI_F_LOCKED);
    TEST_ASSERT_TRUE(button(&S, "Auto find")->flags & DUI_F_LOCKED);
    TEST_ASSERT_FALSE(button(&S, "Retry now")->flags & DUI_F_LOCKED);
    b.locked = false;
    b.sat.auto_disc = true;
    b.sat.state = DUI_SAT_CONNECTED;
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_EQUAL(DUI_ROLE_OK, find(&S, NULL, "Connected")->role);
    /* connected, nothing to retry */
    TEST_ASSERT_NULL(maybe_button(&S, "Retry now"));
    TEST_ASSERT_EQUAL_STRING("Mode|[Satellite]|[Dock] / Companion:Studio-Mac|IP:192|168|8|155|Connected / [Choose]|[Set IP]",
                             content(&S));
    b.sat.auto_disc = false;
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_EQUAL_STRING("Mode|[Satellite]|[Dock] / Companion:Studio-Mac|IP:192|168|8|155|Connected / "
                             "[Auto find]|[Set IP]",
                             content(&S));
    b.sat.auto_disc = true;
    b.sat.target.name[0] = b.sat.target.ip[0] = '\0'; /* none picked yet */
    b.sat.state = DUI_SAT_CHOOSE;
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_EQUAL_STRING("Mode|[Satellite]|[Dock] / Companion:None|Choose / [Retry now]|[Choose]|[Set IP]", content(&S));
}

/* ---- Hotspot and Scan to join ---- */

/* Choose lists the Companions found; only a pick (Companion.Choose) changes anything. */
static void test_choose_page(void)
{
    dui_box_t b = sat(DUI_SAT_CONNECTED);
    build(DUI_PAGE_CHOOSE, &b);
    TEST_ASSERT_TRUE(S.sub);
    TEST_ASSERT_EQUAL_STRING("Choose", S.status.title[0]);
    TEST_ASSERT_EQUAL_STRING("#1 Tap|[Studio-Mac/192.168.8.155]|[FOH-PC/192.168.8.40]", content(&S));
    TEST_ASSERT_EQUAL(DUI_A_PICK0, button(&S, "Studio-Mac")->action);
    TEST_ASSERT_EQUAL(DUI_A_PICK0 + 1, button(&S, "FOH-PC")->action);
    TEST_ASSERT_TRUE(button(&S, "Studio-Mac")->flags & DUI_F_SELECTED); /* the one it connects to */
    TEST_ASSERT_FALSE(button(&S, "FOH-PC")->flags & DUI_F_SELECTED);
    b.locked = true; /* choosing still works under the password */
    build(DUI_PAGE_CHOOSE, &b);
    TEST_ASSERT_FALSE(button(&S, "FOH-PC")->flags & DUI_F_LOCKED);
    b.sat.ncomps = 0; /* none seen */
    build(DUI_PAGE_CHOOSE, &b);
    TEST_ASSERT_EQUAL_STRING("*|Searching", content(&S));
    b = sat(DUI_SAT_FINDING); /* a manual address meanwhile: nothing to pick */
    b.sat.auto_disc = false;
    build(DUI_PAGE_CHOOSE, &b);
    TEST_ASSERT_EQUAL_STRING("Manual|address", content(&S));
    TEST_ASSERT_NULL(maybe_button(&S, "FOH-PC"));
}

static void test_hotspot(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_HOTSPOT, &b);
    TEST_ASSERT_EQUAL_STRING("S Hotspot:CompanionBox-8D1C / S Password:k7m2p9xq|QR / [Turn off]|[Show QR]", content(&S));
    const dui_tile_t *t = button(&S, "Turn off");
    TEST_ASSERT_TRUE(t->flags & DUI_F_DANGER);
    TEST_ASSERT_TRUE(t->flags & DUI_F_ON);
    TEST_ASSERT_EQUAL(DUI_HOLD_AP_OFF_MS, t->hold_ms);
    TEST_ASSERT_EQUAL_STRING("cuts users", t->hint);
    TEST_ASSERT_EQUAL(DUI_A_AP_OFF, t->action);
    TEST_ASSERT_EQUAL(DUI_A_GO_QR, button(&S, "Show QR")->action);
    b.net.ap_secured = false;
    build(DUI_PAGE_HOTSPOT, &b);
    TEST_ASSERT_EQUAL_STRING("S Hotspot:CompanionBox-8D1C / S No|password|QR / [Turn off]|[Show QR]", content(&S));
    b.net.ap_on = false;
    build(DUI_PAGE_HOTSPOT, &b);
    TEST_ASSERT_EQUAL_STRING("Hotspot|is off / [Always on]", content(&S));
    t = button(&S, "Always on");
    TEST_ASSERT_EQUAL(0, t->hold_ms);
    TEST_ASSERT_EQUAL(DUI_A_AP_ON, t->action);
    TEST_ASSERT_EQUAL(DUI_ICON_POWER, t->icon);
    TEST_ASSERT_FALSE(t->flags & (DUI_F_DANGER | DUI_F_ON | DUI_F_LOCKED));
    b.locked = true; /* a saved setting: locked under the PIN, as Turn off */
    build(DUI_PAGE_HOTSPOT, &b);
    TEST_ASSERT_TRUE(button(&S, "Always on")->flags & DUI_F_LOCKED);
    TEST_ASSERT_NULL(maybe_button(&S, "On 15 min"));
}

static const char *qr_payload(int k)
{
    int n = 0;
    for (int i = 0; i < S.ntiles; i++)
        if (S.tile[i].kind == DUI_TILE_QR && n++ == k) return S.tile[i].text;
    return NULL;
}

static void test_qr_payloads(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_QR, &b);
    TEST_ASSERT_EQUAL_STRING("QR|#1 Join / QR|#2 Open", content(&S)); /* no "Point your ..." row */
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:CompanionBox-8D1C;P:k7m2p9xq;;", qr_payload(0));
    TEST_ASSERT_EQUAL_STRING("http://192.168.4.1/", qr_payload(1));
    TEST_ASSERT_EQUAL_STRING("Scan", S.status.title[0]);
    TEST_ASSERT_EQUAL_STRING("join", S.status.title[2]);
    TEST_ASSERT_EQUAL(DUI_G_PACK, S.group[1].flags);
    TEST_ASSERT_EQUAL(2, S.ngroups); /* the two codes and their steps, nothing after them */
    /* escaped; open: T:nopass */
    strcpy(b.net.ap_ssid, "My;Box,\"1\"");
    strcpy(b.net.ap_pass, "p:w\\");
    build(DUI_PAGE_QR, &b);
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:My\\;Box\\,\\\"1\\\";P:p\\:w\\\\;;", qr_payload(0));
    b.net.ap_secured = false;
    build(DUI_PAGE_QR, &b);
    TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:My\\;Box\\,\\\"1\\\";;", qr_payload(0));
    strcpy(b.net.ap_ip, "10.1.2.3");
    build(DUI_PAGE_QR, &b);
    TEST_ASSERT_EQUAL_STRING("http://10.1.2.3/", qr_payload(1));
    /* the hotspot off meanwhile: no codes */
    b.net.ap_on = false;
    build(DUI_PAGE_QR, &b);
    TEST_ASSERT_NULL(qr_payload(0));
    TEST_ASSERT_EQUAL_STRING("Hotspot|is off", content(&S));
}

/* ---- Box ---- */

static void test_box_screen(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_BOX, &b);
    TEST_ASSERT_EQUAL_STRING("Name:CompanionBox-8D1C|Uptime:2h 14m / Version:0.1.0|Slot A:Running|Slot B:In sync|[Sync/In sync] / "
                             "[Dimmer]|Bright:70%|[Brighter]|[Sound on]|[Flashes]|[Restart]",
                             content(&S));
    TEST_ASSERT_EQUAL(DUI_ROLE_OK, find(&S, "Slot B", NULL)->role);
    TEST_ASSERT_EQUAL(DUI_A_DIMMER, button(&S, "Dimmer")->action);
    TEST_ASSERT_EQUAL(DUI_A_BRIGHTER, button(&S, "Brighter")->action);
    TEST_ASSERT_EQUAL(DUI_ICON_SUN_LOW, button(&S, "Dimmer")->icon);
    const dui_tile_t *t = button(&S, "Sound on");
    TEST_ASSERT_EQUAL(DUI_A_SOUND, t->action);
    TEST_ASSERT_TRUE(t->flags & DUI_F_SELECTED);
    TEST_ASSERT_EQUAL(DUI_ICON_SOUND, t->icon);
    t = button(&S, "Restart");
    TEST_ASSERT_EQUAL(DUI_A_RESTART, t->action);
    TEST_ASSERT_EQUAL(DUI_HOLD_RESTART_MS, t->hold_ms);
    TEST_ASSERT_EQUAL_STRING("restarts", t->hint); /* "Hold 2 s" / "restarts" */
    TEST_ASSERT_TRUE(t->flags & DUI_F_DANGER);
    /* sound off, the dimmest; running from the second slot */
    b.sound = false;
    b.brightness = 10;
    b.box.running_b = true;
    b.box.uptime_s = 59;
    build(DUI_PAGE_BOX, &b);
    TEST_ASSERT_EQUAL_STRING("Name:CompanionBox-8D1C|Uptime:59s / Version:0.1.0|Slot B:Running|Slot A:In sync|[Sync/In sync] / "
                             "[Dimmer]|Bright:10%|[Brighter]|[Sound off]|[Flashes]|[Restart]",
                             content(&S));
    t = button(&S, "Sound off");
    TEST_ASSERT_FALSE(t->flags & DUI_F_SELECTED);
    TEST_ASSERT_EQUAL(DUI_ICON_MUTE, t->icon);
}

static void test_box_uptime(void)
{
    static const struct { uint32_t s; const char *text; } U[] = {
        {0, "0s"}, {59, "59s"}, {60, "1m"}, {3599, "59m"}, {3600, "1h 0m"}, {2 * 3600 + 14 * 60, "2h 14m"},
        {86399, "23h 59m"}, {86400, "1d 0h"}, {3 * 86400 + 5 * 3600 + 59, "3d 5h"},
    };
    dui_box_t b = box_default();
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++) {
        b.box.uptime_s = U[i].s;
        build(DUI_PAGE_BOX, &b);
        TEST_ASSERT_EQUAL_STRING(U[i].text, find(&S, "Uptime", NULL)->text);
    }
}

/* the Wi-Fi row not in use has a Use button: a 1.5 s hold, "Hold 1.5 s to switch", locked under the PIN;
 * none for a network that isn't set up. */
static void test_wifi_use_button(void)
{
    dui_box_t b = box_default();
    build(DUI_PAGE_NETWORK, &b);
    const dui_tile_t *t = button(&S, "Use");
    TEST_ASSERT_EQUAL(DUI_A_USE_BACKUP, t->action);
    TEST_ASSERT_EQUAL(DUI_HOLD_USE_MS, t->hold_ms);
    TEST_ASSERT_EQUAL(1500, DUI_HOLD_USE_MS);
    TEST_ASSERT_EQUAL_STRING("Wi-Fi blips", t->hint);
    TEST_ASSERT_FALSE(t->flags & DUI_F_LOCKED);
    b.net.wifi_net = DUI_WIFI_BACKUP;
    b.locked = true;
    build(DUI_PAGE_NETWORK, &b);
    t = button(&S, "Use");
    TEST_ASSERT_EQUAL(DUI_A_USE_MAIN, t->action);
    TEST_ASSERT_TRUE(t->flags & DUI_F_LOCKED); /* locked; the PIN pad unlocks it */
    b = box_default();
    b.net.wifi_backup[0] = '\0';
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_NULL(maybe_button(&S, "Use"));
}

/* the Box screen's flashes toggle, beside Sound: selected while ledFlash is on; never locked. */
static void test_box_flashes_toggle(void)
{
    dui_box_t b = box_default();
    b.flash = true;
    build(DUI_PAGE_BOX, &b);
    const dui_tile_t *t = button(&S, "Flashes"); /* one word: one line on every key; selected = on */
    TEST_ASSERT_EQUAL(DUI_A_FLASH, t->action);
    TEST_ASSERT_EQUAL(DUI_ICON_FLASH, t->icon);
    TEST_ASSERT_TRUE(t->flags & DUI_F_SELECTED);
    TEST_ASSERT_EQUAL(0, t->hold_ms);
    /* beside Sound */
    int is = -1, it = -1;
    for (int i = 0; i < S.ntiles; i++) {
        if (S.tile[i].text && !strcmp(S.tile[i].text, "Sound on")) is = i;
        if (S.tile[i].text && !strcmp(S.tile[i].text, "Flashes")) it = i;
    }
    TEST_ASSERT_EQUAL(is + 1, it);
    b.flash = false;
    b.locked = true;
    build(DUI_PAGE_BOX, &b);
    t = button(&S, "Flashes");
    TEST_ASSERT_FALSE(t->flags & (DUI_F_SELECTED | DUI_F_LOCKED));
    TEST_ASSERT_FALSE(dui_action_locked(DUI_A_FLASH));
}

/* The other slot's word and the button per state: Sync now (no force) for differs, invalid and empty;
 * Boot other only for other_newer, and only when that image is valid, ours and the running one isn't
 * pending verify. */
static void test_box_slot_buttons(void)
{
    static const struct {
        int slot;
        bool valid, foreign, pending;
        const char *row;
        int role;
        bool dot;
    } C[] = {
        {DUI_SLOT_IN_SYNC, true, false, false, "Version:0.1.0|Slot A:Running|Slot B:In sync|[Sync/In sync]", DUI_ROLE_OK, false},
        {DUI_SLOT_SYNCING, false, false, false, "Version:0.1.0|Slot A:Running|Slot B:45%|[Sync/45%]", DUI_ROLE_WAIT, false},
        {DUI_SLOT_DIFFERS, true, false, false, "Version:0.1.0|Slot A:Running|Slot B:0.0.9|[Sync now]", DUI_ROLE_LABEL, true},
        {DUI_SLOT_EMPTY, false, false, false, "Version:0.1.0|Slot A:Running|Slot B:Empty|[Sync now]", DUI_ROLE_LABEL, true},
        {DUI_SLOT_INVALID, false, false, false, "Version:0.1.0|Slot A:Running|Slot B:Invalid|[Sync now]", DUI_ROLE_BAD, true},
        {DUI_SLOT_INVALID, true, true, false, "Version:0.1.0|Slot A:Running|Slot B:Invalid|[Sync now]", DUI_ROLE_BAD, true},
        {DUI_SLOT_OTHER_NEWER, true, false, false, "Version:0.1.0|Slot A:Running|Slot B:0.0.9|[Sync/newer]|[Boot other]", DUI_ROLE_WAIT, true},
        {DUI_SLOT_OTHER_NEWER, true, false, true, "Version:0.1.0|Slot A:Running|Slot B:0.0.9|[Sync/wait]", DUI_ROLE_WAIT, true},
        {DUI_SLOT_OTHER_NEWER, false, false, false, "Version:0.1.0|Slot A:Running|Slot B:0.0.9|[Sync/newer]", DUI_ROLE_WAIT, true},
        {DUI_SLOT_OTHER_NEWER, true, true, false, "Version:0.1.0|Slot A:Running|Slot B:0.0.9|[Sync/newer]", DUI_ROLE_WAIT, true},
        {DUI_SLOT_WAITING, true, false, false, "Version:0.1.0|Slot A:Running|Slot B:Waiting|[Sync/wait]", DUI_ROLE_WAIT, false},
        {DUI_SLOT_DIFFERS, true, false, true, "Version:0.1.0|Slot A:Running|Slot B:0.0.9|[Sync/wait]", DUI_ROLE_LABEL, true},
        {DUI_SLOT_IN_SYNC, true, false, true, "Version:0.1.0|Slot A:Running|Slot B:In sync|[Sync/wait]", DUI_ROLE_OK, false},
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        dui_box_t b = box_default();
        b.box.slot = (uint8_t)C[i].slot;
        b.box.pct = 45;
        strcpy(b.box.other_version, "0.0.9");
        b.box.other_valid = C[i].valid;
        b.box.other_foreign = C[i].foreign;
        b.box.pending = C[i].pending;
        build(DUI_PAGE_BOX, &b);
        char want[512], msg[16];
        snprintf(want, sizeof want, "Name:CompanionBox-8D1C|Uptime:2h 14m / %s / [Dimmer]|Bright:70%%|[Brighter]|[Sound on]|[Flashes]|[Restart]",
                 C[i].row);
        snprintf(msg, sizeof msg, "case %d", (int)i);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want, content(&S), msg);
        TEST_ASSERT_EQUAL_MESSAGE(C[i].role, find(&S, "Slot B", NULL)->role, msg);
        /* the Sync button in every state: a tap, the sync icon; live ("Sync now", or "In sync" in its tag) or
         * disabled (no action, dimmed) with its reason in the tag; the PIN locks it only while live */
        const dui_tile_t *t = maybe_button(&S, "Sync now");
        if (!t) t = maybe_button(&S, "Sync");
        TEST_ASSERT_NOT_NULL_MESSAGE(t, msg);
        TEST_ASSERT_EQUAL_MESSAGE(0, t->hold_ms, msg);
        TEST_ASSERT_EQUAL(DUI_ICON_SYNC, t->icon);
        const bool live = !C[i].pending && (C[i].slot == DUI_SLOT_IN_SYNC || C[i].slot == DUI_SLOT_DIFFERS ||
                                            C[i].slot == DUI_SLOT_EMPTY || C[i].slot == DUI_SLOT_INVALID);
        TEST_ASSERT_EQUAL_MESSAGE(live ? DUI_A_SYNC : DUI_A_NONE, t->action, msg);
        TEST_ASSERT_EQUAL_MESSAGE(!live, !!(t->flags & DUI_F_DIM), msg);
        TEST_ASSERT_EQUAL_MESSAGE(strcmp(t->text, "Sync now") != 0, !!(t->flags & DUI_F_TAG), msg);
        b.locked = true;
        build(DUI_PAGE_BOX, &b);
        const dui_tile_t *lt = maybe_button(&S, t->text);
        TEST_ASSERT_EQUAL_MESSAGE(live, !!(lt->flags & DUI_F_LOCKED), msg);
        b.locked = false;
        build(DUI_PAGE_BOX, &b);
        static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063};
        for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) { /* the tag and the label fit, the icon clear */
            plan(PIDS[m]);
            char where[32];
            snprintf(where, sizeof where, "box case %d 0x%04X", (int)i, PIDS[m]);
            fit_layout(&L.deck, &L, where);
        }
        t = maybe_button(&S, "Boot other");
        if (t) {
            TEST_ASSERT_EQUAL_MESSAGE(DUI_A_BOOT_OTHER, t->action, msg);
            TEST_ASSERT_EQUAL_MESSAGE(DUI_HOLD_BOOT_MS, t->hold_ms, msg);
            TEST_ASSERT_EQUAL_STRING("restarts", t->hint);
            TEST_ASSERT_TRUE(t->flags & DUI_F_DANGER);
        }
        build(DUI_PAGE_HOME, &b); /* the Box menu button's amber dot: the slot needs attention */
        TEST_ASSERT_EQUAL_MESSAGE(C[i].dot ? DUI_STATE_WAIT : DUI_STATE_NONE, menu_button(&S, "Box")->state, msg);
    }
    /* no version read: the state in words */
    dui_box_t b = box_default();
    b.box.other_version[0] = '\0';
    b.box.slot = DUI_SLOT_DIFFERS;
    build(DUI_PAGE_BOX, &b);
    TEST_ASSERT_EQUAL_STRING("Older", find(&S, "Slot B", NULL)->text);
    b.box.slot = DUI_SLOT_OTHER_NEWER;
    build(DUI_PAGE_BOX, &b);
    TEST_ASSERT_EQUAL_STRING("Newer", find(&S, "Slot B", NULL)->text);
}

/* ---- The settings password: exactly mode switch, Turn off, Restart, Sync now, Boot other ---- */

static int count_locked(const dui_screen_t *s, const char **labels, int n)
{
    int locked = 0;
    for (int i = 0; i < s->ntiles; i++) {
        const dui_tile_t *t = &s->tile[i];
        if (!(t->flags & DUI_F_LOCKED)) continue;
        locked++;
        bool expected = false;
        for (int k = 0; k < n; k++) expected |= t->text && !strcmp(t->text, labels[k]);
        TEST_ASSERT_TRUE_MESSAGE(expected, t->text);
    }
    for (int i = 0; i < s->nmenu; i++) TEST_ASSERT_FALSE(s->menu_tile[i].flags & DUI_F_LOCKED);
    return locked;
}

/* the list, in one place: exactly the mode switch, Turn off, Restart, Sync now and Boot other. */
static void test_action_locked(void)
{
    int n = 0;
    for (int a = 0; a < DUI_A_COUNT; a++) n += dui_action_locked(a);
    TEST_ASSERT_EQUAL(18, n); /* and the two Wi-Fi Use buttons; Ethernet's four; Wi-Fi on / off;
                               * the Companion IP's three; Always on */
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_COMP_EDIT) && dui_action_locked(DUI_A_COMP_SAVE));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_COMP_AUTO));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_WIFI_OFF) && dui_action_locked(DUI_A_WIFI_ON));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_ETH_DHCP) && dui_action_locked(DUI_A_ETH_STATIC));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_ETH_EDIT) && dui_action_locked(DUI_A_ETH_SAVE));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_USE_MAIN) && dui_action_locked(DUI_A_USE_BACKUP));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_MODE_SAT) && dui_action_locked(DUI_A_MODE_DOCK));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_AP_OFF) && dui_action_locked(DUI_A_RESTART));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_SYNC) && dui_action_locked(DUI_A_BOOT_OTHER));
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_AP_ON)); /* Always on */
    TEST_ASSERT_FALSE(dui_action_locked(DUI_A_RETRY) || dui_action_locked(DUI_A_PICK0));
    TEST_ASSERT_FALSE(dui_action_locked(DUI_ACT_BACK));
}

/* The per-deck rule as pure helpers: a deck shows its locks while a PIN is set and it isn't
 * unlocked; a locked action runs when no secret is set, or the deck it came from was unlocked. */
static void test_lock_rules(void)
{
    TEST_ASSERT_TRUE(dui_deck_locked(true, false));
    TEST_ASSERT_FALSE(dui_deck_locked(true, true));
    TEST_ASSERT_FALSE(dui_deck_locked(false, false));
    TEST_ASSERT_TRUE(dui_action_allowed(DUI_A_BRIGHTER, true, false)); /* not a locked action */
    TEST_ASSERT_FALSE(dui_action_allowed(DUI_A_RESTART, true, false));
    TEST_ASSERT_TRUE(dui_action_allowed(DUI_A_RESTART, true, true));   /* this deck unlocked */
    TEST_ASSERT_TRUE(dui_action_allowed(DUI_A_RESTART, false, false)); /* no secret set */
    TEST_ASSERT_FALSE(dui_action_allowed(DUI_A_USE_BACKUP, true, false));
}

static void test_locks(void)
{
    static const char *LOCKABLE[] = {"Satellite", "Dock",     "Turn off",    "Restart", "Sync now", "Boot other", "Use",
                                     "Use DHCP",  "Make static", "Edit", "Wi-Fi", "Sync", "Set IP",   "Auto find",
                                     "Always on"};
    for (int pw = 0; pw < 2; pw++)
        for (int mode = 0; mode < 2; mode++)
            for (int slot = DUI_SLOT_IN_SYNC; slot <= DUI_SLOT_WAITING; slot++)
                for (int ap = 0; ap < 2; ap++) {
                    dui_box_t b = mode ? box_default() : sat(DUI_SAT_CHOOSE);
                    b.locked = pw;
                    b.box.slot = (uint8_t)slot;
                    b.net.ap_on = ap;
                    int locked = 0, want = 0;
                    for (int page = 0; page < DUI_PAGES_N; page++) {
                        build(page, &b);
                        locked += count_locked(&S, LOCKABLE, 15);
                    }
                    if (pw) /* the other mode, Turn off or Always on, Restart, Sync while it does something (in sync too; not
                             * while disabled), Boot other when shown, the backup's Use, Ethernet's Edit ("Set on web
                             * page" does nothing, so it has no lock) */
                        want = 1 + 1 + 1 + (slot == DUI_SLOT_IN_SYNC || slot == DUI_SLOT_DIFFERS || slot == DUI_SLOT_EMPTY ||
                                             slot == DUI_SLOT_INVALID) +
                               (slot == DUI_SLOT_OTHER_NEWER) + 1 + 1 + 1 + /* and the Wi-Fi toggle */
                               !mode; /* Satellite's Set IP (auto-discovery on: no Auto find) */
                    TEST_ASSERT_EQUAL(want, locked);
                }
    /* the rest still works: no lock on Retry, Choose, Show QR, brightness, sound, the picks */
    dui_box_t b = sat(DUI_SAT_CHOOSE);
    b.locked = true;
    build(DUI_PAGE_COMPANION, &b);
    TEST_ASSERT_TRUE(button(&S, "Dock")->flags & DUI_F_LOCKED);
    TEST_ASSERT_FALSE(button(&S, "Satellite")->flags & DUI_F_LOCKED); /* the current mode: nothing to lock */
    TEST_ASSERT_FALSE(button(&S, "Retry now")->flags & DUI_F_LOCKED);
    build(DUI_PAGE_HOTSPOT, &b);
    TEST_ASSERT_FALSE(button(&S, "Show QR")->flags & DUI_F_LOCKED);
}

/* the Ethernet row: its addressing, and the switch per case (static: Use DHCP; DHCP with a set to use:
 * Make static; with none: a disabled "Set on web page"), each a 1.5 s hold locked under the PIN; Edit. */
static void test_eth_row(void)
{
    dui_box_t b = box_default();
    b.net.eth_link = true;
    strcpy(b.net.eth_ip, "192.168.8.50");
    b.net.eth_dhcp = false;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_NOT_NULL(find(&S, "Address", "Static"));
    const dui_tile_t *t = button(&S, "Use DHCP");
    TEST_ASSERT_EQUAL(DUI_A_ETH_DHCP, t->action);
    TEST_ASSERT_EQUAL(DUI_HOLD_ETH_MS, t->hold_ms);
    TEST_ASSERT_EQUAL_STRING("net resets", t->hint);
    TEST_ASSERT_FALSE(t->flags & (DUI_F_LOCKED | DUI_F_DIM));
    TEST_ASSERT_NULL(maybe_button(&S, "Make static"));
    TEST_ASSERT_EQUAL(DUI_A_ETH_EDIT, button(&S, "Edit")->action);
    TEST_ASSERT_EQUAL(0, button(&S, "Edit")->hold_ms);

    b.net.eth_dhcp = true; /* DHCP with a set to use */
    b.net.eth_static_ok = true;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_NOT_NULL(find(&S, "Address", "DHCP"));
    t = button(&S, "Make static");
    TEST_ASSERT_EQUAL(DUI_A_ETH_STATIC, t->action);
    TEST_ASSERT_EQUAL(DUI_HOLD_ETH_MS, t->hold_ms);
    TEST_ASSERT_NULL(maybe_button(&S, "Use DHCP"));

    b.net.eth_static_ok = false; /* nothing to use: disabled, says where */
    b.net.eth_link = false;
    b.net.eth_ip[0] = '\0';
    build(DUI_PAGE_NETWORK, &b);
    t = button(&S, "Set on web page");
    TEST_ASSERT_EQUAL(DUI_A_NONE, t->action);
    TEST_ASSERT_TRUE(t->flags & DUI_F_DIM);
    TEST_ASSERT_EQUAL(0, t->hold_ms);
    TEST_ASSERT_NULL(maybe_button(&S, "Make static"));

    b.locked = true; /* under the PIN: the switches and Edit locked; the disabled one stays as it is */
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_FALSE(button(&S, "Set on web page")->flags & DUI_F_LOCKED);
    TEST_ASSERT_TRUE(button(&S, "Edit")->flags & DUI_F_LOCKED);
    b.net.eth_static_ok = true;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_TRUE(button(&S, "Make static")->flags & DUI_F_LOCKED);
    b.net.eth_dhcp = false;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_TRUE(button(&S, "Use DHCP")->flags & DUI_F_LOCKED);
}

/* ---- drawn results: what a button shows, from its pixels ---- */

/* Pixels in [x0, x1) x [y0, y1) (in 96-unit key coordinates) within tol of rgb. */
static int count_rgb(const uint8_t *img, int px, float x0, float y0, float x1, float y1, uint32_t rgb, int tol)
{
    const int r = (int)(rgb >> 16 & 255), g = (int)(rgb >> 8 & 255), b = (int)(rgb & 255);
    int n = 0;
    for (int y = (int)(y0 * px / 96); y < (int)(y1 * px / 96); y++)
        for (int x = (int)(x0 * px / 96); x < (int)(x1 * px / 96); x++) {
            const uint8_t *p = img + 3 * (y * px + x);
            if (abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol) n++;
        }
    return n;
}

static int nbox;
static int box[8][4];
static void box_probe(int px, int x0, int y0, int x1, int y1)
{
    (void)px;
    if (nbox < 8) box[nbox][0] = x0, box[nbox][1] = y0, box[nbox][2] = x1, box[nbox][3] = y1, nbox++;
}

/* t drawn on a key of key_px into img, its texts' ink boxes into box[] (nbox of them). */
static void draw_boxes(const dui_tile_t *t, int key_px, uint8_t *img)
{
    nbox = 0;
    dui__text_probe = box_probe;
    dui_render_key(t, key_px, img);
    dui__text_probe = NULL;
}

/* A text box in the key's top-left quarter (the HOLD tag's place). */
static bool text_top_left(int key_px)
{
    for (int i = 0; i < nbox; i++)
        if (box[i][2] <= key_px / 2 + 2 && box[i][3] <= key_px * 30 / 96) return true;
    return false;
}

/* The lowest row of the icon's ink: where the tile drawn as shown differs from it drawn without its icon; -1
 * none. */
static int icon_bottom(const dui_tile_t *t0, int px, uint8_t *img, uint32_t rgb)
{
    (void)rgb;
    static uint8_t bare[128 * 128 * 3];
    dui_tile_t t = *t0;
    dui_render_key(&t, px, img);
    t.icon = DUI_ICON_NONE;
    dui_render_key(&t, px, bare);
    int bottom = -1;
    for (int y = 0; y < px; y++)
        for (int x = 0; x < px && bottom < y; x++)
            if (memcmp(img + 3 * (y * px + x), bare + 3 * (y * px + x), 3)) bottom = y;
    return bottom;
}

/* The label's (or hint's) text boxes all start under the icon's ink. */
static void label_clear_of_icon(int bottom, int px, const char *msg)
{
    for (int i = 0; i < nbox; i++)
        if (box[i][1] > px * 34 / 96) TEST_ASSERT_GREATER_THAN_MESSAGE(bottom, box[i][1], msg);
}

/* Make static with its "saved" / "lease" tag is a normal hold button on every key size: its
 * icon, the HOLD tag and the tag (apart), the hold fill when held, "Hold 1.5 s" / "to apply" in amber when let go
 * early, and under the PIN the lock, its label and icon dimmed, no HOLD. */
static void test_make_static_drawn(void)
{
    dui_box_t b = box_default();
    b.net.eth_dhcp = true;
    b.net.eth_static_ok = true;
    b.net.eth_static_saved = false;
    build(DUI_PAGE_NETWORK, &b);
    const dui_tile_t t0 = *button(&S, "Make static");
    TEST_ASSERT_EQUAL_STRING("lease", t0.caption);
    TEST_ASSERT_TRUE(t0.flags & DUI_F_TAG);
    static uint8_t img[128 * 128 * 3];
    const int sizes[] = {72, 80, 96, 120};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        const int px = sizes[k];
        char msg[24];
        snprintf(msg, sizeof msg, "%d px", px);
        dui_tile_t t = t0;
        draw_boxes(&t, px, img); /* normal */
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 4, count_rgb(img, px, 30, 20, 66, 56, SUI_RGB_VALUE, 40), msg); /* icon */
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 8, count_rgb(img, px, 48, 6, 90, 28, SUI_RGB_ACCENT, 50), msg); /* tag */
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 4, count_rgb(img, px, 8, 62, 88, 90, SUI_RGB_VALUE, 40), msg); /* label */
        TEST_ASSERT_EQUAL_MESSAGE(0, count_rgb(img, px, 8, 62, 88, 90, SUI_RGB_WAIT, 40), msg); /* no hint */
        TEST_ASSERT_TRUE_MESSAGE(text_top_left(px), msg); /* HOLD */
        const int ib = icon_bottom(&t0, px, img, SUI_RGB_VALUE);
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, ib, msg);
        draw_boxes(&t, px, img);
        label_clear_of_icon(ib, px, msg); /* the label under the icon, never over it */
        for (int i = 0; i < nbox; i++) /* no two texts overlap (HOLD and the tag) */
            for (int j = i + 1; j < nbox; j++)
                TEST_ASSERT_TRUE_MESSAGE(box[i][2] <= box[j][0] || box[j][2] <= box[i][0] || box[i][3] <= box[j][1] ||
                                             box[j][3] <= box[i][1],
                                         msg);
        t = t0; /* held half way: the fill from the bottom, the icon on it */
        t.flags |= DUI_F_DOWN;
        t.hold = 128;
        dui_render_key(&t, px, img);
        TEST_ASSERT_GREATER_THAN_MESSAGE(px, count_rgb(img, px, 12, 70, 84, 88, DUI_RGB_HOLD, 6), msg);
        TEST_ASSERT_EQUAL_MESSAGE(0, count_rgb(img, px, 12, 10, 84, 30, DUI_RGB_HOLD, 6), msg);
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 4, count_rgb(img, px, 30, 20, 66, 56, SUI_RGB_VALUE, 40), msg);
        t = t0; /* let go early: "Hold 1.5 s" / "to apply" in amber, the icon still there */
        t.flags |= DUI_F_HINT;
        draw_boxes(&t, px, img);
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 4, count_rgb(img, px, 6, 50, 90, 90, SUI_RGB_WAIT, 40), msg);
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 4, count_rgb(img, px, 30, 20, 66, 56, SUI_RGB_VALUE, 40), msg);
        const int hb = icon_bottom(&t, px, img, SUI_RGB_VALUE); /* the icon as drawn beside the hint */
        draw_boxes(&t, px, img);
        label_clear_of_icon(hb, px, msg);
        t = t0; /* under the PIN: drawn disabled: the padlock (no HOLD text), the label, the icon and the tag
                 * dimmed */
        t.flags |= DUI_F_LOCKED;
        draw_boxes(&t, px, img);
        TEST_ASSERT_FALSE_MESSAGE(text_top_left(px), msg);
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 10, count_rgb(img, px, 8, 8, 34, 32, DUI_RGB_LOCK, 30), msg); /* lock */
        TEST_ASSERT_EQUAL_MESSAGE(0, count_rgb(img, px, 8, 50, 88, 90, SUI_RGB_VALUE, 40), msg);
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 4, count_rgb(img, px, 8, 20, 88, 90, DUI_RGB_DIM, 30), msg);
        TEST_ASSERT_EQUAL_MESSAGE(0, count_rgb(img, px, 48, 6, 90, 28, SUI_RGB_ACCENT, 50), msg);
    }
    b.net.eth_static_saved = true;
    build(DUI_PAGE_NETWORK, &b);
    TEST_ASSERT_EQUAL_STRING("saved", button(&S, "Make static")->caption);
}

/* which static set Make static uses: the saved one when valid, else the lease, else none. */
static void test_eth_static_pick(void)
{
    uint8_t q[4];
    TEST_ASSERT_TRUE(dui_ipv4_parse("192.168.8.50", q));
    TEST_ASSERT_EQUAL(50, q[3]);
    TEST_ASSERT_FALSE(dui_ipv4_parse("192.168.8.256", q));
    TEST_ASSERT_FALSE(dui_ipv4_parse("192.168.8", q));
    TEST_ASSERT_FALSE(dui_ipv4_parse("192.168.8.1.2", q));
    TEST_ASSERT_FALSE(dui_ipv4_parse("1922.168.8.1", q));
    TEST_ASSERT_FALSE(dui_ipv4_parse("", q));
    TEST_ASSERT_FALSE(dui_ipv4_parse(" 1.2.3.4", q));

    const dui_ipset_t saved = {"192.168.8.50", "255.255.255.0", "192.168.8.1", "8.8.8.8"};
    const dui_ipset_t lease = {"192.168.8.77", "255.255.255.0", "192.168.8.1", "192.168.8.1"};
    dui_ipset_t out;
    TEST_ASSERT_EQUAL(DUI_ETH_FROM_SAVED, dui_eth_static_pick(&saved, &lease, &out));
    TEST_ASSERT_EQUAL_STRING("192.168.8.50", out.ip);
    TEST_ASSERT_EQUAL_STRING("8.8.8.8", out.dns);

    dui_ipset_t bad = saved; /* a saved set that isn't valid: the lease */
    strcpy(bad.gw, "");
    TEST_ASSERT_EQUAL(DUI_ETH_FROM_LEASE, dui_eth_static_pick(&bad, &lease, &out));
    TEST_ASSERT_EQUAL_STRING("192.168.8.77", out.ip);
    TEST_ASSERT_EQUAL_STRING("192.168.8.1", out.gw);
    strcpy(bad.gw, "192.168.8.1");
    strcpy(bad.dns, "8.8.8");
    TEST_ASSERT_FALSE(dui_ipset_valid(&bad));
    strcpy(bad.dns, ""); /* no DNS is fine */
    TEST_ASSERT_TRUE(dui_ipset_valid(&bad));
    dui_ipset_t zero = {"0.0.0.0", "255.255.255.0", "0.0.0.0", ""}; /* never saved: empty or zeros */
    TEST_ASSERT_FALSE(dui_ipset_valid(&zero));
    const dui_ipset_t empty = {"", "", "", ""};
    TEST_ASSERT_EQUAL(DUI_ETH_FROM_LEASE, dui_eth_static_pick(&empty, &lease, &out));

    dui_ipset_t lease2 = {"10.0.0.9", "255.0.0.0", "", "0.0.0.0"}; /* no router, no DNS from the server */
    TEST_ASSERT_EQUAL(DUI_ETH_FROM_LEASE, dui_eth_static_pick(&empty, &lease2, &out));
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", out.gw);
    TEST_ASSERT_EQUAL_STRING("", out.dns);
    TEST_ASSERT_TRUE(dui_ipset_valid(&out)); /* what Eth.SetConfig takes */

    const dui_ipset_t none = {"", "", "", ""}; /* no cable, no lease, nothing saved */
    out.ip[0] = 'x';
    TEST_ASSERT_EQUAL(DUI_ETH_FROM_NONE, dui_eth_static_pick(&empty, &none, &out));
    TEST_ASSERT_EQUAL_STRING("", out.ip);
    TEST_ASSERT_EQUAL(DUI_ETH_FROM_NONE, dui_eth_static_pick(NULL, NULL, NULL));
    zero.ip[0] = '0'; /* a lease of 0.0.0.0 (DHCP still asking) is none */
    TEST_ASSERT_EQUAL(DUI_ETH_FROM_NONE, dui_eth_static_pick(NULL, &zero, &out));
}

/* ---- Every screen plans on every model without overflow ---- */

/* On every model, every real screen's text (descenders too) stays inside the key's margins and
 * every button's label inside its outline; the PIN pad's and the static-address editor's too. */
static void test_every_screen_fits(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x006C, 0x00C6, 0x0080, 0x00B9, 0x0060, 0x006D, 0x0084, 0x009A, 0x0063,
                                    0x00B3, 0x00B8};
    dui_box_t boxes[7];
    boxes[0] = box_default();
    boxes[1] = sat(DUI_SAT_CHOOSE);
    boxes[2] = sat(DUI_SAT_NONET);
    boxes[3] = sat(DUI_SAT_CONNECTED);
    boxes[3].ondemand = boxes[3].connected = true;
    boxes[4] = box_default();
    boxes[4].locked = true;
    boxes[4].box.slot = DUI_SLOT_DIFFERS;
    boxes[4].flash = true;
    boxes[5] = sat(DUI_SAT_TOO_OLD);
    strcpy(boxes[5].toast, "Restarting");
    boxes[6] = sat(DUI_SAT_RECON);
    boxes[6].net.eth_link = true;
    strcpy(boxes[6].net.eth_ip, "192.168.100.200");
    boxes[6].net.eth_static_ok = true;
    boxes[6].box.slot = DUI_SLOT_OTHER_NEWER;
    boxes[6].sound = false;
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        const dui_deck_t d = deck_of(PIDS[m]);
        for (int k = 0; k < 7; k++)
            for (int page = 0; page < DUI_PAGES_N; page++) {
                build(page, &boxes[k]);
                plan(PIDS[m]);
                char name[24];
                snprintf(name, sizeof name, "page %d box %d", page, k);
                fit_layout(&d, &L, name);
            }
        static dui_pin_t P; /* the PIN pad in its states */
        for (int st = 0; st < 4; st++) {
            dui_pin_out_t o;
            dui_pin_open(&P, &d, DUI_A_RESTART, st != 3, st == 2 ? 3599 : 0, 1000, &o);
            if (st == 1) P.phase = DUI_PIN_WRONG, P.until = 99999;
            dui_pin_layout(&d, &P, 1000, &L);
            fit_layout(&d, &L, "pin");
            if (d.bar != DUI_BAR_NONE) { /* the PIN bar's words at 16 px or more */
                static uint8_t bar[1200 * 100 * 3];
                fit_min_px = 0;
                dui__text_probe = min_probe;
                dui_render_pin_bar(&P, 1000, d.bar_w, d.bar_h, bar);
                dui__text_probe = NULL;
                if (fit_min_px) TEST_ASSERT_GREATER_OR_EQUAL(DUI_TEXT_MIN_PX, fit_min_px);
            }
        }
        static dui_ipe_t E; /* the editor, every field, an error, the summary */
        const dui_ipset_t pre = {"192.168.100.200", "255.255.255.0", "192.168.100.1", "255.255.255.255"};
        for (int f = DUI_IPE_IP; f <= DUI_IPE_SUM + 1; f++) {
            dui_ipe_out_t o;
            dui_ipe_open(&E, &d, &pre, 1000, &o);
            E.field = (uint8_t)(f > DUI_IPE_SUM ? DUI_IPE_GW : f);
            if (f > DUI_IPE_SUM) E.err = DUI_IPE_E_BROADCAST, E.err_until = 99999;
            dui_ipe_layout(&d, &E, 1000, &L);
            fit_layout(&d, &L, "ipedit");
            if (d.bar != DUI_BAR_NONE) { /* its bar too */
                static uint8_t bar[1200 * 100 * 3];
                fit_min_px = 0;
                dui__text_probe = min_probe;
                dui_render_ipe_bar(&E, 1000, d.bar_w, d.bar_h, bar);
                dui__text_probe = NULL;
                if (fit_min_px) TEST_ASSERT_GREATER_OR_EQUAL(DUI_TEXT_MIN_PX, fit_min_px);
            }
        }
    }
}

static void test_every_screen_plans(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063};
    dui_box_t boxes[6];
    boxes[0] = box_default();
    boxes[1] = sat(DUI_SAT_CHOOSE);
    boxes[1].sat.ncomps = DUI_COMPS_MAX;
    for (int i = 0; i < DUI_COMPS_MAX; i++) {
        snprintf(boxes[1].sat.comp[i].name, sizeof boxes[1].sat.comp[i].name, "Companion-%d", i);
        snprintf(boxes[1].sat.comp[i].ip, sizeof boxes[1].sat.comp[i].ip, "192.168.100.%d", 100 + i);
    }
    boxes[2] = sat(DUI_SAT_NONET);
    boxes[3] = sat(DUI_SAT_CONNECTED);
    boxes[3].ondemand = boxes[3].connected = true;
    boxes[4] = box_default();
    boxes[4].locked = true;
    boxes[4].box.slot = DUI_SLOT_OTHER_NEWER;
    boxes[5] = sat(DUI_SAT_TOO_OLD);
    strcpy(boxes[5].toast, "Restarting");
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++)
        for (int k = 0; k < 6; k++)
            for (int page = 0; page < DUI_PAGES_N; page++) {
                build(page, &boxes[k]);
                TEST_ASSERT_FALSE(S.overflow);
                plan(PIDS[m]);
                TEST_ASSERT_TRUE(L.pages >= 1);
                /* the real screens read at the minimum sizes on every model; no hyphen */
                const dui_deck_t d = deck_of(PIDS[m]);
                for (int t = 0; t < L.ntiles; t++) {
                    const dui_tile_t *x = &L.tile[t];
                    const int n = x->text ? (x->len ? x->len : (int)strlen(x->text)) : 0;
                    char msg[64];
                    snprintf(msg, sizeof msg, "0x%04X page %d box %d: %.24s", PIDS[m], page, k, x->text ? x->text : "");
                    if ((x->kind == DUI_TILE_WORD || x->kind == DUI_TILE_CAP || x->kind == DUI_TILE_STEP) && n && x->fit.nl)
                        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_TEXT_MIN_PX, x->fit.px, msg);
                    if (x->kind == DUI_TILE_CAP && x->caption && x->caption[0] && x->fit.nl)
                        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_CAP_MIN_PX, x->fit.cap_px, msg);
                    if (x->kind == DUI_TILE_BUTTON && n && !(x->flags & DUI_F_NAV))
                        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_LABEL_MIN_PX, dui_label_px(x->text, n, d.key_px), msg);
                    if (x->kind == DUI_TILE_BUTTON && x->hold_ms && x->hint) /* the hint as let go early */
                        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_LABEL_MIN_PX, dui_hint_px(x, d.key_px), msg);
                    if (x->fit.hyph) { /* only a word with no natural break, at a syllable-like point */
                        TEST_ASSERT_EQUAL_MESSAGE(DUI_TILE_WORD, x->kind, msg);
                        TEST_ASSERT_TRUE_MESSAGE(x->role != DUI_ROLE_VALUE && x->role != DUI_ROLE_ACCENT, msg);
                        TEST_ASSERT_TRUE_MESSAGE(x->fit.cut >= 3 && n - x->fit.cut >= 3, msg);
                        TEST_ASSERT_NULL_MESSAGE(memchr(x->text, ' ', (size_t)n), msg);
                    }
                }
            }
    build(DUI_PAGE_HOME, &boxes[5]);
    TEST_ASSERT_EQUAL_STRING("Restarting", S.status.toast);
    build(DUI_PAGE_HOME, &boxes[0]);
    TEST_ASSERT_NULL(S.status.toast);
}

/* ---- Deck text sizes ---- */

/* Every home state, the sub-screens and the toasts, on a deck with the status on its keys. */
static int nstates(void) { return 13; }
static void state_box(int k, dui_box_t *b, int *page)
{
    static const int SAT[] = {DUI_SAT_FINDING, DUI_SAT_CHOOSE, DUI_SAT_TOO_OLD, DUI_SAT_RECON, DUI_SAT_CONNECTED, DUI_SAT_NONET};
    *page = DUI_PAGE_HOME;
    if (k < 6) {
        *b = sat(SAT[k]);
        if (SAT[k] == DUI_SAT_CONNECTED) b->connected = true;
        return;
    }
    *b = box_default(); /* dock: waiting, no free dock */
    if (k == 7) b->dock.no_free = true;
    if (k == 8) *page = DUI_PAGE_QR;      /* "Scan to join": a title of three words */
    if (k == 9) *page = DUI_PAGE_COMPANION;
    if (k == 10) *page = DUI_PAGE_BOX;
    if (k == 11) strcpy(b->toast, "Static 192.168.8.50"), b->toast_state = DUI_STATE_OK;
    if (k == 12) strcpy(b->toast, "Restarting"), b->toast_state = DUI_STATE_WAIT, *page = DUI_PAGE_NETWORK;
}

/* One status size per model: on the decks with the status on their keys, every word of the status row (in every
 * state, a toast's, a title's) is set at dui__status_px; a word too long for one line wraps inside its key at that
 * size ("Recon-" / "necting"), never smaller. A sub-screen's title takes one key. */
static void test_status_one_size(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x0080, 0x0063, 0x006D, 0x00B9};
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        const dui_deck_t d = deck_of(PIDS[m]);
        const int px = dui__status_px(d.key_px);
        for (int k = 0; k < nstates(); k++) {
            dui_box_t b;
            int page;
            state_box(k, &b, &page);
            build(page, &b);
            plan(PIDS[m]);
            if (!L.status_rows) continue; /* the Mini's sub-screens: the title on the Back key */
            int words = 0;
            for (int c = 0; c < d.cols; c++) {
                dui_tile_t t;
                if (!dui_layout_key(&L, 0, c, 0, &t) || t.kind != DUI_TILE_WORD) continue;
                char msg[64];
                snprintf(msg, sizeof msg, "0x%04X state %d col %d: %.20s", PIDS[m], k, c, t.text);
                TEST_ASSERT_EQUAL_MESSAGE(px, t.fit.px, msg);
                words++;
            }
            if (S.sub && !S.status.toast) TEST_ASSERT_EQUAL_MESSAGE(1, words, "a title in one key");
        }
    }
    TEST_ASSERT_EQUAL(21, dui__status_px(96)); /* "Companion" whole on one line */
    TEST_ASSERT_EQUAL(16, dui__status_px(72));
    /* "Reconnecting" on the Module 32: in its key on two lines at the row's size, not one smaller line */
    dui_box_t b = sat(DUI_SAT_RECON);
    build(DUI_PAGE_HOME, &b);
    plan(PID_M32);
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 1, 0, &t));
    TEST_ASSERT_EQUAL_STRING("Reconnecting", t.text);
    TEST_ASSERT_EQUAL(2, t.fit.nl);
    TEST_ASSERT_EQUAL(21, t.fit.px);
    TEST_ASSERT_EQUAL(5, t.fit.cut); /* "Recon-" / "necting" */
    TEST_ASSERT_TRUE(t.fit.hyph);
}

static int probe_px, probe_x0, probe_x1;
static void bar_probe(int px, int x0, int y0, int x1, int y1)
{
    (void)y0, (void)y1;
    if (!probe_px) probe_px = px, probe_x0 = x0;
    probe_x1 = x1;
}

/* One status size per bar: the Neo's info bar at DUI_BAR_INFO_PX and the strip at DUI_BAR_STRIP_PX in every state
 * and on every sub-screen; a status too long for the info bar takes its short form ("Waiting"), never a smaller
 * size. */
static void test_bar_one_size(void)
{
    static const struct { int w, h, px; } BAR[] = {{248, 58, DUI_BAR_INFO_PX}, {800, 100, DUI_BAR_STRIP_PX},
                                                  {1200, 100, DUI_BAR_STRIP_PX}};
    uint8_t *img = malloc(1200 * 100 * 3);
    TEST_ASSERT_NOT_NULL(img);
    for (size_t i = 0; i < sizeof BAR / sizeof BAR[0]; i++)
        for (int k = 0; k < 11; k++) { /* the toasts aside: a toast's own text, smaller when it must */
            dui_box_t b;
            int page;
            state_box(k, &b, &page);
            build(page, &b);
            probe_px = 0;
            dui__text_probe = bar_probe;
            dui_render_bar(&S.status, S.sub, 0, 1, BAR[i].w, BAR[i].h, img);
            dui__text_probe = NULL;
            char msg[48];
            snprintf(msg, sizeof msg, "bar %dx%d state %d", BAR[i].w, BAR[i].h, k);
            TEST_ASSERT_EQUAL_MESSAGE(BAR[i].px, probe_px, msg);
        }
    dui_box_t b = box_default(); /* "Waiting for Companion" with the hotspot's icon: "Waiting" on the info bar */
    build(DUI_PAGE_HOME, &b);
    probe_px = 0;
    dui__text_probe = bar_probe;
    dui_render_bar(&S.status, false, 0, 1, 248, 58, img);
    dui__text_probe = NULL;
    TEST_ASSERT_EQUAL(DUI_BAR_INFO_PX, probe_px);
    TEST_ASSERT_INT_WITHIN(3, dui__text_w(DUI_BAR_INFO_PX, "Waiting", 7, 0, NULL), probe_x1 - probe_x0);
    free(img);
}

/* Short sentences in one key, on two lines where they read as a sentence; no "Point your phone camera" row. */
static void test_sentence_keys(void)
{
    static const struct { const char *text, *first; } K[] = {
        {"Needs 5.0+ has 4.2.0", "Needs 5.0+"}, {"Plug in cable", "Plug in"}, {"Max 2 decks", "Max 2"}};
    for (int i = 0; i < 3; i++) {
        dui_box_t b = i == 0 ? sat(DUI_SAT_TOO_OLD) : box_default();
        if (i == 1) b = sat(DUI_SAT_NONET), b.net.ap_on = false;
        if (i == 2) b.dock.no_free = true;
        build(DUI_PAGE_HOME, &b);
        static const uint16_t PIDS[] = {0x00BA, 0x0080, 0x0063, 0x009A};
        for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
            plan(PIDS[m]);
            bool found = false;
            for (int t = 0; t < L.ntiles; t++) {
                const dui_tile_t *x = &L.tile[t];
                if (x->kind != DUI_TILE_WORD || !x->text || strcmp(x->text, K[i].text)) continue;
                found = true;
                char msg[48];
                snprintf(msg, sizeof msg, "0x%04X %s", PIDS[m], K[i].text);
                TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_TEXT_MIN_PX, x->fit.px, msg);
                if (x->fit.nl == 2) /* split where the sentence breaks */
                    TEST_ASSERT_EQUAL_MESSAGE((int)strlen(K[i].first), x->fit.cut, msg);
                if (i == 0) TEST_ASSERT_EQUAL_MESSAGE(2, x->fit.nl, msg);
            }
            TEST_ASSERT_TRUE_MESSAGE(found, K[i].text);
        }
    }
    dui_box_t b = box_default();
    build(DUI_PAGE_QR, &b);
    for (int t = 0; t < S.ntiles; t++) TEST_ASSERT_FALSE(S.tile[t].kind == DUI_TILE_WORD);
}

/* A hold let go early says how long and what it does: "Hold 2 s" / "restarts", on two lines split at
 * its " · ", at DUI_LABEL_MIN_PX or more on every key size, clear of the outline. */
static void test_hint_instruction(void)
{
    static const struct { int page; const char *label, *want; } H[] = {
        {DUI_PAGE_BOX, "Restart", "Hold 2 s \xC2\xB7 restarts"},         {DUI_PAGE_COMPANION, "Satellite", "Hold 1.5 s \xC2\xB7 restarts"},
        {DUI_PAGE_HOTSPOT, "Turn off", "Hold 1.5 s \xC2\xB7 cuts users"}, {DUI_PAGE_NETWORK, "Use DHCP", "Hold 1.5 s \xC2\xB7 net resets"},
        {DUI_PAGE_NETWORK, "Use", "Hold 1.5 s \xC2\xB7 Wi-Fi blips"},     {DUI_PAGE_NETWORK, "Wi-Fi", "Hold 1.5 s \xC2\xB7 Wi-Fi off"},
    };
    static const int KEYS[] = {72, 80, 96, 112, 120};
    uint8_t *img = malloc(120 * 120 * 3);
    TEST_ASSERT_NOT_NULL(img);
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) {
        dui_box_t b = box_default();
        if (H[i].page == DUI_PAGE_NETWORK) b.net.eth_dhcp = false, b.net.wifi_net = DUI_WIFI_BACKUP; /* Use DHCP, Use */
        build(H[i].page, &b);
        const dui_tile_t *t0 = button(&S, H[i].label);
        TEST_ASSERT_NOT_NULL_MESSAGE(t0, H[i].label);
        char h[48];
        TEST_ASSERT_EQUAL((int)strlen(H[i].want), dui_hint_text(t0, h, sizeof h));
        TEST_ASSERT_EQUAL_STRING(H[i].want, h);
        for (int k = 0; k < 5; k++) {
            const int px = KEYS[k];
            char msg[48];
            snprintf(msg, sizeof msg, "%s on %d px", H[i].want, px);
            TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_LABEL_MIN_PX, dui_hint_px(t0, px), msg);
            dui_tile_t t = *t0;
            t.flags |= DUI_F_HINT;
            t.flags &= (uint16_t)~DUI_F_LOCKED;
            draw_boxes(&t, px, img);
            int lines = 0, low = 0;
            for (int j = 0; j < nbox; j++)
                if (box[j][1] > px * 40 / 96) { /* under the icon: the hint's lines */
                    lines++;
                    if (box[j][3] > low) low = box[j][3];
                }
            TEST_ASSERT_EQUAL_MESSAGE(2, lines, msg);                  /* "Hold 1.5 s" / "net resets" */
            TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(px * 86 / 96, low, msg); /* clear of the outline (its inside at 89) */
        }
    }
    dui_tile_t t;
    memset(&t, 0, sizeof t);
    t.hold_ms = 2000;
    char h[48];
    dui_hint_text(&t, h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("Hold 2 s", h); /* no verb */
    free(img);
}

/* ---- Polish ---- */

/* The Mini's Network screen: every network's detail page always there, so ".local" (the last page) never moves; a
 * detail page with nothing on it shows the network's state ("Not set"), not a blank page. */
static void test_mini_network_pages(void)
{
    dui_box_t st[5];
    st[0] = box_default();
    st[1] = box_default(), st[1].net.wifi_net = DUI_WIFI_BACKUP; /* the backup in use: its detail page has the signal */
    st[2] = box_default(), st[2].net.wifi_main[0] = st[2].net.wifi_backup[0] = '\0', st[2].net.wifi_net = DUI_WIFI_NONE,
    st[2].net.wifi_ip[0] = '\0';
    st[3] = box_default(), st[3].net.eth_link = true, strcpy(st[3].net.eth_ip, "10.0.0.7");
    st[4] = box_default(), st[4].net.wifi_off = true;
    for (int k = 0; k < 5; k++) {
        build(DUI_PAGE_NETWORK, &st[k]);
        plan(PID_MINI);
        char msg[24];
        snprintf(msg, sizeof msg, "state %d", k);
        TEST_ASSERT_EQUAL_MESSAGE(7, L.pages, msg);
        int p, c;
        const int r = tile_row(tile_with(".local"), &p, &c);
        TEST_ASSERT_EQUAL_MESSAGE(6, p, msg);
        TEST_ASSERT_TRUE_MESSAGE(r == 0 && c >= 1, msg);
        for (int pg = 1; pg < 6; pg += 2) { /* each detail page holds something */
            int n = 0;
            for (int i = 1; i <= 4; i++) n += L.key[pg][i] != DUI_NO_TILE;
            TEST_ASSERT_GREATER_THAN_MESSAGE(0, n, msg);
        }
    }
    dui_tile_t t; /* no main network: its detail page says so */
    build(DUI_PAGE_NETWORK, &st[2]);
    plan(PID_MINI);
    TEST_ASSERT_TRUE(dui_layout_key(&L, 3, 1, 0, &t));
    TEST_ASSERT_EQUAL_STRING("Not set", t.text);
}

/* A state word in a content row never smaller than a one-word label (the Companion screen: "Reconnecting" in its key
 * on two lines, "Mode" no larger than the comfortable size). */
static void test_state_words_outweigh_labels(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x0080, 0x0063, 0x009A};
    dui_box_t b = sat(DUI_SAT_RECON);
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        build(DUI_PAGE_COMPANION, &b);
        plan(PIDS[m]);
        int mode = -1, rec = -1;
        for (int i = 0; i < L.ntiles; i++) {
            if (L.tile[i].kind != DUI_TILE_WORD || !L.tile[i].text) continue;
            if (!strcmp(L.tile[i].text, "Mode")) mode = L.tile[i].fit.px;
            if (!strcmp(L.tile[i].text, "Reconnecting")) rec = L.tile[i].fit.px;
        }
        char msg[24];
        snprintf(msg, sizeof msg, "0x%04X", PIDS[m]);
        TEST_ASSERT_TRUE_MESSAGE(mode > 0 && rec > 0, msg);
        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(mode, rec, msg);
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(sui_comfort_px(L.deck.key_px), mode, msg);
    }
}

/* Sync while an upload holds the flash: "wait" in its tag, dimmed, no action (not a "Not now" toast). */
static void test_sync_waits_for_upload(void)
{
    dui_box_t b = box_default();
    b.box.slot = DUI_SLOT_DIFFERS;
    build(DUI_PAGE_BOX, &b);
    const dui_tile_t *t = button(&S, "Sync now");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL(DUI_A_SYNC, t->action);
    b.box.flash_busy = true;
    build(DUI_PAGE_BOX, &b);
    t = button(&S, "Sync");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING("wait", t->caption);
    TEST_ASSERT_TRUE(t->flags & DUI_F_DIM);
    TEST_ASSERT_EQUAL(DUI_A_NONE, t->action);
}

/* The Menu screen's toast on the Back key (decks with the menu in the status row): a label at the floor, inside the
 * key, on every model it happens on. */
static void test_menu_toast_fits(void)
{
    static const uint16_t PIDS[] = {0x00BA, 0x00C6, 0x0080, 0x0084, 0x009A, 0x0063, 0x006D};
    static const char *TOASTS[] = {"Upload running", "Unlocked \xC2\xB7 hold Restart", "Restarting already"};
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++)
        for (int k = 0; k < 3; k++) {
            dui_box_t b = box_default();
            strcpy(b.toast, TOASTS[k]);
            b.toast_state = DUI_STATE_BAD;
            build(DUI_PAGE_MENU, &b);
            plan(PIDS[m]);
            const dui_deck_t d = deck_of(PIDS[m]);
            char msg[48];
            snprintf(msg, sizeof msg, "0x%04X %s", PIDS[m], TOASTS[k]);
            uint8_t *img = malloc((size_t)d.key_px * d.key_px * 3);
            for (int i = 0; i < L.ntiles; i++) { /* the Back key's toast as drawn: 14 px or more */
                const dui_tile_t *x = &L.tile[i];
                if (x->kind != DUI_TILE_BUTTON || !(x->flags & DUI_F_NAV) || !x->text || x->state == DUI_STATE_NONE) continue;
                fit_min_px = 0;
                dui__text_probe = min_probe;
                dui_render_key(x, d.key_px, img);
                dui__text_probe = NULL;
                TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_LABEL_MIN_PX, fit_min_px, msg);
            }
            free(img);
            fit_layout(&d, &L, msg);
        }
}

/* The ⌫ that cancels when held (the + / Neo pages, the Mini roller) draws "HOLD" top left and a small ✕ top right;
 * plain ⌫ draws neither. */
static void test_cancel_tag_drawn(void)
{
    static const int KEYS[] = {72, 80, 96, 120};
    uint8_t *img = malloc(120 * 120 * 3);
    TEST_ASSERT_NOT_NULL(img);
    for (int k = 0; k < 4; k++) {
        const int px = KEYS[k];
        dui_tile_t t;
        memset(&t, 0, sizeof t);
        t.kind = DUI_TILE_KEY;
        t.tint = DUI_TINT_MAIN;
        t.icon = DUI_ICON_DEL;
        t.action = 1;
        uint8_t *plain = malloc((size_t)px * px * 3);
        dui_render_key(&t, px, plain);
        t.flags |= DUI_F_TAG;
        dui_render_key(&t, px, img);
        int hold = 0, x = 0; /* pixels the tag changes: top left (HOLD), top right (✕), and nowhere else up there */
        for (int yy = 0; yy < px * 30 / 96; yy++)
            for (int xx = 0; xx < px; xx++)
                if (memcmp(img + 3 * (yy * px + xx), plain + 3 * (yy * px + xx), 3)) { if (xx < px / 2) hold++; else x++; }
        free(plain);
        char msg[24];
        snprintf(msg, sizeof msg, "%d px", px);
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 4, hold, msg); /* HOLD */
        TEST_ASSERT_GREATER_THAN_MESSAGE(px / 8, x, msg);    /* ✕ */
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, count_rgb(img, px, 66, 8, 88, 28, SUI_RGB_LABEL, 60), msg);
    }
    free(img);
}

/* A locked Satellite key: the padlock stays U(9) or more clear of the Satellite mark (the mark a size down and
 * lower while the top band is taken). */
static void test_lock_clear_of_satellite(void)
{
    static const int KEYS[] = {72, 80, 96, 112, 120};
    for (int k = 0; k < 5; k++) {
        const int px = KEYS[k];
        uint8_t *a = calloc((size_t)px * px, 3), *b2 = calloc((size_t)px * px, 3), *f = calloc((size_t)px * px, 3);
        TEST_ASSERT_TRUE(a && b2 && f);
        dui_tile_t t;
        memset(&t, 0, sizeof t);
        t.kind = DUI_TILE_BUTTON;
        t.tint = DUI_TINT_BLACK;
        t.icon = DUI_ICON_SAT;
        t.text = "Satellite";
        t.hold_ms = 1500;
        t.flags = DUI_F_LOCKED;
        dui_render_key(&t, px, a);   /* the lock and the mark */
        t.icon = DUI_ICON_NONE;
        dui_render_key(&t, px, b2);  /* the lock alone */
        t.flags = 0;
        t.hold_ms = 0;
        dui_render_key(&t, px, f);   /* neither (and no HOLD) */
        int best = 1 << 30; /* the closest mark pixel to a lock pixel, squared */
        for (int y = 0; y < px / 2; y++)
            for (int x = 0; x < px / 2; x++) {
                const int i = 3 * (y * px + x);
                if (!memcmp(a + i, b2 + i, 3)) continue; /* not the mark here */
                for (int y2 = 0; y2 < px / 2; y2++)
                    for (int x2 = 0; x2 < px / 2; x2++) {
                        const int j = 3 * (y2 * px + x2);
                        if (memcmp(b2 + j, f + j, 3) && (x - x2) * (x - x2) + (y - y2) * (y - y2) < best)
                            best = (x - x2) * (x - x2) + (y - y2) * (y - y2);
                    }
            }
        char msg[24];
        snprintf(msg, sizeof msg, "%d px", px);
        const int want = px * 9 / 96; /* U(9): before, the mark came within about U(7) of the lock */
        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(want * want, best, msg);
        free(a), free(b2), free(f);
    }
}

/* The hotspot's password on one key on every deck with a screen, on
 * the Hotspot screen and the no-network home: whole on one tile at 16 px or more (1 line or 2 inside the key, nothing
 * added), no piece of it on another key. The box's own password (10 characters) and a shorter one. */
static void test_password_one_key(void)
{
    static const uint16_t PIDS[] = {0x0060, 0x0090, 0x0063, 0x0080, 0x006C, 0x00BA, 0x00B9, 0x00B8, 0x0084, 0x009A, 0x00C6};
    static const char *const PW[] = {"k7m2p9xq", "k7m2p9xqhd"};
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++)
        for (size_t w = 0; w < sizeof PW / sizeof PW[0]; w++)
            for (int home = 0; home < 2; home++) {
                dui_box_t b = box_default();
                strcpy(b.net.ap_pass, PW[w]);
                if (home) {
                    b.net.wifi_net = DUI_WIFI_NONE;
                    b.net.wifi_ip[0] = '\0';
                }
                build(home ? DUI_PAGE_HOME : DUI_PAGE_HOTSPOT, &b);
                plan(PIDS[m]);
                char msg[64];
                snprintf(msg, sizeof msg, "pid %04x, %s, %s", PIDS[m], PW[w], home ? "home" : "hotspot");
                const int ti = tile_with(PW[w]);
                TEST_ASSERT_TRUE_MESSAGE(ti >= 0, msg); /* whole, on one tile */
                int pg, col;
                TEST_ASSERT_TRUE_MESSAGE(tile_row(ti, &pg, &col) >= 0, msg); /* placed */
                TEST_ASSERT_TRUE_MESSAGE(L.tile[ti].fit.px >= 16 && !L.tile[ti].fit.hyph, msg);
                for (int j = 0; j < L.ntiles; j++) { /* no other tile shows a piece of it (3 characters or more) */
                    const dui_tile_t *t = &L.tile[j];
                    const int n = t->text ? (t->len ? t->len : (int)strlen(t->text)) : 0;
                    if (j == ti || n < 3) continue;
                    char piece[64];
                    snprintf(piece, sizeof piece, "%.*s", n, t->text);
                    TEST_ASSERT_NULL_MESSAGE(strstr(PW[w], piece), msg);
                }
            }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_home_dock_waiting);
    RUN_TEST(test_home_no_network);
    RUN_TEST(test_status_signs);
    RUN_TEST(test_home_satellite_states);
    RUN_TEST(test_home_no_free_dock);
    RUN_TEST(test_home_connected_and_close);
    RUN_TEST(test_toast_has_a_place);
    RUN_TEST(test_refusal_reasons);
    RUN_TEST(test_pin_titles);
    RUN_TEST(test_unlock_pulse_and_note);
    RUN_TEST(test_home_connected_label);
    RUN_TEST(test_lead_label);
    RUN_TEST(test_menu_buttons_and_dots);
    RUN_TEST(test_network_wifi_rows);
    RUN_TEST(test_companion);
    RUN_TEST(test_choose_page);
    RUN_TEST(test_hotspot);
    RUN_TEST(test_qr_payloads);
    RUN_TEST(test_box_screen);
    RUN_TEST(test_box_uptime);
    RUN_TEST(test_box_slot_buttons);
    RUN_TEST(test_box_flashes_toggle);
    RUN_TEST(test_wifi_use_button);
    RUN_TEST(test_wifi_toggle);
    RUN_TEST(test_network_keymap);
    RUN_TEST(test_wifi_words);
    RUN_TEST(test_pieces_pool_full);
    RUN_TEST(test_action_locked);
    RUN_TEST(test_locks);
    RUN_TEST(test_lock_rules);
    RUN_TEST(test_eth_row);
    RUN_TEST(test_eth_static_pick);
    RUN_TEST(test_make_static_drawn);
    RUN_TEST(test_every_screen_plans);
    RUN_TEST(test_every_screen_fits);
    RUN_TEST(test_status_one_size);
    RUN_TEST(test_bar_one_size);
    RUN_TEST(test_sentence_keys);
    RUN_TEST(test_hint_instruction);
    RUN_TEST(test_mini_network_pages);
    RUN_TEST(test_state_words_outweigh_labels);
    RUN_TEST(test_sync_waits_for_upload);
    RUN_TEST(test_menu_toast_fits);
    RUN_TEST(test_cancel_tag_drawn);
    RUN_TEST(test_lock_clear_of_satellite);
    RUN_TEST(test_setup_screen_qr);
    RUN_TEST(test_password_one_key);
    return UNITY_END();
}
