/* The deck maintenance UI engine (components/deckui): the per-model plan and flow,
 * the row alignment, the drawing, the QR codes and the status bar. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "deckmodel/model.h"
#include "deckui/deckui.h"
#include "deckui_fixtures.h"
#include "statusui/deck_font.h"
#include "fit_check.h"
#include "../../components/deckui/phosphor.h"
#include "../../components/deckui/raster.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers ---- */

enum { PID_M32 = 0x00BA, PID_XL = 0x006C, PID_PLUSXL = 0x00C6, PID_MK2 = 0x0080, PID_M15 = 0x00B9,
       PID_ORIG = 0x0060, PID_V2 = 0x006D, PID_PLUS = 0x0084, PID_NEO = 0x009A, PID_MINI = 0x0063,
       PID_DISCORD = 0x00B3, PID_M6 = 0x00B8, PID_PEDAL = 0x0086 };

static dui_deck_t deck(uint16_t pid)
{
    dui_deck_t d;
    memset(&d, 0, sizeof d);
    const deck_model_t *m = deck_model_find(DECK_VID_ELGATO, pid);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(dui_deck_of(m, &d));
    return d;
}

static dui_screen_t S;
static dui_layout_t L;

static void plan(uint16_t pid, void (*build)(dui_screen_t *, const fx_box_t *), const fx_box_t *b)
{
    const dui_deck_t d = deck(pid);
    build(&S, b);
    dui_plan(&d, &S, &L);
}

static fx_box_t box_with(int scen, bool ap)
{
    fx_box_t b = fx_box_default();
    b.scen = scen;
    b.ap = ap;
    return b;
}

/* A key as text: "." blank, "_" fill, a word its text ("Caption:text" with a caption), "#n verb" a step,
 * "[Label]" a button ("[<]" Back, "[<Title]" Back with a title, "[x]" Close), "(Label)" an icon tile,
 * "@" the glyph, "~wifi" "~eth" "~none" "~hot" the signs ("~wifioff" "~hotoff" crossed in grey), ">" the pager,
 * "QR", "*" the spinner,
 * "!text" a toast. */
static void key_text(const dui_tile_t *t, char *out, size_t n)
{
    char txt[96] = "";
    if (t->text) snprintf(txt, sizeof txt, "%.*s", t->len ? t->len : (int)strlen(t->text), t->text);
    switch (t->kind) {
    case DUI_TILE_EMPTY: snprintf(out, n, "."); break;
    case DUI_TILE_FILL: snprintf(out, n, "_"); break;
    case DUI_TILE_WORD: snprintf(out, n, "%s", txt); break;
    case DUI_TILE_CAP: snprintf(out, n, "%s:%s", t->caption ? t->caption : "", txt); break;
    case DUI_TILE_STEP: snprintf(out, n, "#%d %s", t->num, txt); break;
    case DUI_TILE_BUTTON:
        if (t->flags & DUI_F_NAV)
            snprintf(out, n, "[%s%s]", t->icon == DUI_ICON_CLOSE ? "x" : "<", strcmp(txt, "Back") && t->icon != DUI_ICON_CLOSE ? txt : "");
        else
            snprintf(out, n, "[%s]", txt);
        break;
    case DUI_TILE_ICON: snprintf(out, n, "(%s)", txt); break;
    case DUI_TILE_GLYPH: snprintf(out, n, "@"); break;
    case DUI_TILE_IND:
        snprintf(out, n, "~%s", t->icon == DUI_ICON_WIFI ? "wifi" : t->icon == DUI_ICON_ETH ? "eth" : t->icon == DUI_ICON_HOTSPOT ? "hot"
                                : t->icon == DUI_ICON_WIFI_OFF ? "wifioff" : t->icon == DUI_ICON_HOTSPOT_OFF ? "hotoff" : "none");
        break;
    case DUI_TILE_PAGER: snprintf(out, n, ">"); break;
    case DUI_TILE_QR: snprintf(out, n, "QR"); break;
    case DUI_TILE_SPIN: snprintf(out, n, "*"); break;
    case DUI_TILE_TOAST: snprintf(out, n, "!%s", txt); break;
    default: snprintf(out, n, "?"); break;
    }
}

/* Page `page` of L, every key: keys joined by '|', rows by '\n'. */
static const char *page_text(int page)
{
    static char out[4096];
    size_t u = 0;
    out[0] = '\0';
    for (int r = 0; r < L.deck.rows; r++) {
        if (r) out[u++] = '\n';
        for (int c = 0; c < L.deck.cols; c++) {
            dui_tile_t t;
            char k[128];
            dui_layout_key(&L, page, c, r, &t);
            key_text(&t, k, sizeof k);
            u += (size_t)snprintf(out + u, sizeof out - u, "%s%s", c ? "|" : "", k);
        }
    }
    return out;
}

/* Row r of page `page` only. */
static const char *row_text(int page, int r)
{
    static char out[1024];
    const char *p = page_text(page);
    for (int k = 0; k < r; k++) p = strchr(p, '\n') + 1;
    const char *e = strchr(p, '\n');
    const size_t n = e ? (size_t)(e - p) : strlen(p);
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/* The first key of a row's text. */
static const char *first_key(const char *row)
{
    static char out[128];
    const size_t n = strcspn(row, "|");
    memcpy(out, row, n);
    out[n] = '\0';
    return out;
}

static const dui_tile_t *at(int page, int col, int row)
{
    static dui_tile_t t;
    return dui_layout_key(&L, page, col, row, &t) ? &t : NULL;
}

/* ---- the plan per model ---- */

static void check_home(uint16_t pid, int status_at, int menu_style, int pager)
{
    fx_box_t b = fx_box_default();
    plan(pid, fx_home, &b);
    char msg[32];
    snprintf(msg, sizeof msg, "pid 0x%04X", pid);
    TEST_ASSERT_EQUAL_MESSAGE(status_at, L.status_at, msg);
    TEST_ASSERT_EQUAL_MESSAGE(menu_style, L.menu_style, msg);
    TEST_ASSERT_EQUAL_MESSAGE(pager, L.pager, msg);
}

static void test_plan_status_and_menu_per_model(void)
{
    check_home(PID_M32, DUI_STATUS_KEYS, DUI_MENU_ROW, DUI_PAGER_MENUROW);
    check_home(PID_XL, DUI_STATUS_KEYS, DUI_MENU_ROW, DUI_PAGER_MENUROW);
    check_home(PID_PLUSXL, DUI_STATUS_STRIP, DUI_MENU_ROW, DUI_PAGER_MENUROW);
    check_home(PID_MK2, DUI_STATUS_KEYS, DUI_MENU_ROW, DUI_PAGER_MENUROW);
    check_home(PID_M15, DUI_STATUS_KEYS, DUI_MENU_ROW, DUI_PAGER_MENUROW);
    check_home(PID_ORIG, DUI_STATUS_KEYS, DUI_MENU_ROW, DUI_PAGER_MENUROW);
    check_home(PID_V2, DUI_STATUS_KEYS, DUI_MENU_ROW, DUI_PAGER_MENUROW);
    check_home(PID_PLUS, DUI_STATUS_STRIP, DUI_MENU_KEY, DUI_PAGER_HW);
    check_home(PID_NEO, DUI_STATUS_INFO, DUI_MENU_KEY, DUI_PAGER_HW);
    check_home(PID_MINI, DUI_STATUS_KEYS, DUI_MENU_STATUS, DUI_PAGER_TAP);
    check_home(PID_DISCORD, DUI_STATUS_KEYS, DUI_MENU_STATUS, DUI_PAGER_TAP);
    check_home(PID_M6, DUI_STATUS_KEYS, DUI_MENU_STATUS, DUI_PAGER_TAP);

    dui_deck_t d;
    TEST_ASSERT_FALSE(dui_deck_of(deck_model_find(DECK_VID_ELGATO, PID_PEDAL), &d)); /* no screen */
    d = deck(PID_PLUSXL);
    TEST_ASSERT_EQUAL(DUI_BAR_STRIP, d.bar);
    TEST_ASSERT_EQUAL(1200, d.bar_w);
    TEST_ASSERT_EQUAL(100, d.bar_h);
    TEST_ASSERT_EQUAL(6, d.dials);
    d = deck(PID_NEO);
    TEST_ASSERT_EQUAL(DUI_BAR_INFO, d.bar);
    TEST_ASSERT_EQUAL(248, d.bar_w);
    TEST_ASSERT_EQUAL(58, d.bar_h);
    TEST_ASSERT_TRUE(d.touch_keys);
}

/* The status row on the keys: glyph, words, indicators at the right; narrow decks drop the
 * indicators first, then the glyph; the Mini's Menu key at its right end, with the short words. */
static void test_plan_status_row(void)
{
    fx_box_t b = fx_box_default();
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("@|Waiting|for|Companion|_|_|~hot|~wifi", row_text(0, 0));
    plan(PID_MK2, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("@|Waiting|for|Companion|~wifi", row_text(0, 0));
    plan(PID_MINI, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("@|Waiting|[Menu]", row_text(0, 0));
    b.scen = FX_NONET;
    plan(PID_MINI, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("No|network|[Menu]", row_text(0, 0));
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("@|No|network|_|_|_|~hot|~none", row_text(0, 0));
    b = fx_box_default(); /* a toast takes the row over: its glyph and tint (done: green), its word alone */
    b.toast = "Restarting";
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("@|Restarting|_|_|_|_|~hot|~wifi", row_text(0, 0));
    TEST_ASSERT_EQUAL(DUI_TINT_OK, at(0, 1, 0)->tint);
    TEST_ASSERT_EQUAL(DUI_STATE_OK, at(0, 0, 0)->state);
    TEST_ASSERT_EQUAL(DUI_ROLE_OK, at(0, 1, 0)->role);
    /* No free dock: red, today's hint; the Mini's short form */
    b = fx_box_default();
    b.scen = FX_NODOCK;
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("@|No|free|dock|_|_|~hot|~wifi", row_text(0, 0));
    TEST_ASSERT_EQUAL_STRING("Max|6|decks|.|.|.|.|.", row_text(0, 1));
    TEST_ASSERT_EQUAL(DUI_TINT_BAD, at(0, 1, 0)->tint);
    TEST_ASSERT_EQUAL(DUI_STATE_BAD, at(0, 1, 3)->state); /* the Companion menu button's dot */
    plan(PID_MINI, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("No|dock|[Menu]", row_text(0, 0));
    /* a sub-screen: Back, the title, the indicators, on the neutral tint */
    b = fx_box_default();
    plan(PID_M32, fx_network, &b);
    TEST_ASSERT_EQUAL_STRING("[<]|Network|_|_|_|_|~hot|~wifi", row_text(0, 0));
    TEST_ASSERT_EQUAL(DUI_TINT_NEUTRAL, at(0, 1, 0)->tint);
    TEST_ASSERT_EQUAL(DUI_ROLE_VALUE, at(0, 1, 0)->role);
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL(DUI_TINT_WAIT, at(0, 1, 0)->tint);
    TEST_ASSERT_EQUAL(DUI_ROLE_WAIT, at(0, 1, 0)->role);
}

/* the status row's indicators are buttons that look like icons: the network one goes to Network,
 * the hotspot one to Hotspot (engine ids the navigation maps); on the strip, a tap on the icon's place. */
static uint8_t *img(int w, int h);
static int lum(const uint8_t *p);

static void test_indicators_are_buttons(void)
{
    fx_box_t b = fx_box_default();
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL(DUI_TILE_IND, at(0, 6, 0)->kind);
    TEST_ASSERT_EQUAL(DUI_ACT_HOTSPOT, at(0, 6, 0)->action);
    TEST_ASSERT_EQUAL(DUI_ACT_NET, at(0, 7, 0)->action);
    plan(PID_MK2, fx_network, &b);
    TEST_ASSERT_EQUAL(DUI_ACT_NET, at(0, 4, 0)->action);
    /* a held indicator is drawn brighter, still with no outline */
    dui_tile_t t = *at(0, 4, 0); /* the MK.2's network indicator */
    uint8_t *up = img(96, 96), *down = img(96, 96);
    dui_render_key(&t, 96, up);
    t.flags |= DUI_F_DOWN;
    dui_render_key(&t, 96, down);
    TEST_ASSERT_TRUE(lum(down + 3 * (10 * 96 + 48)) > lum(up + 3 * (10 * 96 + 48)));
    free(up);
    free(down);
    /* the strip (+, + XL): the icons' places at its right end; the hotspot left of the network */
    dui_status_t st;
    memset(&st, 0, sizeof st);
    st.net = DUI_NET_WIFI;
    st.hotspot = true;
    TEST_ASSERT_EQUAL(DUI_ACT_NET, dui_bar_hit(&st, 800, 100, 800 - 22 - 24));
    TEST_ASSERT_EQUAL(DUI_ACT_HOTSPOT, dui_bar_hit(&st, 800, 100, 800 - 22 - 24 - 60));
    TEST_ASSERT_EQUAL(0, dui_bar_hit(&st, 800, 100, 300));
    TEST_ASSERT_EQUAL(DUI_ACT_NET, dui_bar_hit(&st, 1200, 100, 1200 - 30));
    st.hotspot = false; /* its sign stays, crossed, and still opens the Hotspot screen */
    TEST_ASSERT_EQUAL(DUI_ACT_HOTSPOT, dui_bar_hit(&st, 800, 100, 800 - 22 - 24 - 60));
    TEST_ASSERT_EQUAL(0, dui_bar_hit(&st, 800, 100, 800 - 22 - 24 - 120));
    st.eth_link = true; /* Ethernet linked: its sign rightmost, Wi-Fi left of it, the hotspot left of that */
    TEST_ASSERT_EQUAL(DUI_ACT_NET, dui_bar_hit(&st, 800, 100, 800 - 22 - 24));
    TEST_ASSERT_EQUAL(DUI_ACT_NET, dui_bar_hit(&st, 800, 100, 800 - 22 - 24 - 60));
    TEST_ASSERT_EQUAL(DUI_ACT_HOTSPOT, dui_bar_hit(&st, 800, 100, 800 - 22 - 24 - 120));
    /* the Neo's info bar: two at most, the route's (Wi-Fi) and Ethernet kept, the hotspot gone */
    TEST_ASSERT_EQUAL(DUI_ACT_NET, dui_bar_hit(&st, 248, 58, 248 - 8 - 11));
    TEST_ASSERT_EQUAL(DUI_ACT_NET, dui_bar_hit(&st, 248, 58, 248 - 8 - 11 - 26));
    TEST_ASSERT_EQUAL(0, dui_bar_hit(&st, 248, 58, 248 - 8 - 11 - 52));
}

static void test_plan_pager_kind_on_sub_screens(void)
{
    fx_box_t b = fx_box_default();
    const struct { uint16_t pid; int pager; } T[] = {
        {PID_M32, DUI_PAGER_KEY}, {PID_PLUSXL, DUI_PAGER_HW}, {PID_MK2, DUI_PAGER_KEY},
        {PID_PLUS, DUI_PAGER_HW}, {PID_NEO, DUI_PAGER_HW}, {PID_MINI, DUI_PAGER_KEY},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        plan(T[i].pid, fx_network, &b);
        TEST_ASSERT_EQUAL(DUI_MENU_NONE, L.menu_style);
        TEST_ASSERT_EQUAL(T[i].pager, L.pager);
    }
}

/* Sub-screens put Back on the top-left key, on every model. */
static void test_plan_back_top_left(void)
{
    const uint16_t P[] = {PID_M32, PID_PLUSXL, PID_MK2, PID_PLUS, PID_NEO, PID_MINI};
    fx_box_t b = fx_box_default();
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        plan(P[i], fx_box, &b);
        TEST_ASSERT_EQUAL(0, L.back_col);
        TEST_ASSERT_EQUAL(0, L.back_row);
        for (int p = 0; p < L.pages; p++) {
            const dui_tile_t *t = at(p, 0, 0);
            TEST_ASSERT_NOT_NULL(t);
            TEST_ASSERT_EQUAL(DUI_TILE_BUTTON, t->kind);
            TEST_ASSERT_TRUE(t->flags & DUI_F_NAV);
            TEST_ASSERT_EQUAL(DUI_ICON_BACK, t->icon);
            TEST_ASSERT_EQUAL(DUI_ACT_BACK, t->action);
        }
    }
    /* home opened on demand: Close there instead */
    b.close = true;
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("[x]|Waiting|for|Companion|_|_|~hot|~wifi", row_text(0, 0));
    TEST_ASSERT_EQUAL(DUI_ACT_CLOSE, at(0, 0, 0)->action);
    plan(PID_PLUS, fx_home, &b); /* the status on the strip: Close takes the top-left key */
    TEST_ASSERT_EQUAL_STRING("[x]", first_key(row_text(0, 0)));
    TEST_ASSERT_EQUAL(0, L.back_col);
    TEST_ASSERT_EQUAL(0, L.back_row);
}

static void test_plan_menu_key(void)
{
    fx_box_t b = fx_box_default();
    plan(PID_PLUS, fx_home, &b);
    TEST_ASSERT_EQUAL(3, L.menu_col);
    TEST_ASSERT_EQUAL(1, L.menu_row);
    TEST_ASSERT_EQUAL_STRING("[Menu]", strrchr(row_text(0, 1), '|') + 1);
    TEST_ASSERT_EQUAL(DUI_ACT_MENU, at(0, 3, 1)->action);
    plan(PID_NEO, fx_home, &b);
    TEST_ASSERT_EQUAL(3, L.menu_col);
    TEST_ASSERT_EQUAL(1, L.menu_row);
    for (int p = 0; p < L.pages; p++) TEST_ASSERT_EQUAL(DUI_ACT_MENU, at(p, 3, 1)->action);
    plan(PID_MINI, fx_home, &b);
    TEST_ASSERT_EQUAL(2, L.menu_col);
    TEST_ASSERT_EQUAL(0, L.menu_row);
    /* a menu row: the four buttons along the bottom, no Menu key */
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL(-1, L.menu_col);
    TEST_ASSERT_EQUAL_STRING("[Network]|[Companion]|[Hotspot]|[Box]|.|.|.|.", row_text(0, 3));
    TEST_ASSERT_EQUAL(FX_ACT_NETWORK, at(0, 0, 3)->action);
    TEST_ASSERT_EQUAL(DUI_STATE_WAIT, at(0, 1, 3)->state); /* Companion's state dot */
    TEST_ASSERT_EQUAL(DUI_STATE_OK, at(0, 2, 3)->state);   /* the hotspot is on */
    /* sub-screens have no menu */
    plan(PID_PLUS, fx_network, &b);
    TEST_ASSERT_EQUAL(-1, L.menu_col);
}

/* Decks of 6 keys or fewer use the whole grid for sub-screens; the Back key carries the title. */
static void test_plan_mini_full_grid(void)
{
    fx_box_t b = fx_box_default();
    plan(PID_MINI, fx_network, &b);
    TEST_ASSERT_TRUE(L.full);
    TEST_ASSERT_EQUAL(0, L.status_rows);
    TEST_ASSERT_EQUAL(2, L.content_rows);
    TEST_ASSERT_EQUAL_STRING("[<Network]|(Ethernet)|No cable", row_text(0, 0));
    plan(PID_MINI, fx_menu, &b);
    TEST_ASSERT_EQUAL_STRING("[<Menu]|[Network]|[Companion]\n[Hotspot]|[Box]|.", page_text(0));
    plan(PID_MK2, fx_network, &b);
    TEST_ASSERT_FALSE(L.full);
    TEST_ASSERT_EQUAL(1, L.status_rows);
    plan(PID_PLUS, fx_network, &b); /* 8 keys: Back with no title, the title on the strip */
    TEST_ASSERT_FALSE(L.full);
    TEST_ASSERT_EQUAL_STRING("[<]", first_key(row_text(0, 0)));
}

/* ---- the content flow ---- */

/* The optional step goes exactly when its group doesn't fit its row. */
static void test_flow_optional_step(void)
{
    fx_box_t b = fx_box_default(); /* "1 Add" + 4 octets + port: 6 tiles */
    plan(PID_M32, fx_home, &b);    /* 8 a row: kept */
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:192|168|8|225|Port:5343|.|.", row_text(0, 1));
    plan(PID_MK2, fx_home, &b); /* 5 a row, the address on two keys: 4 tiles, kept */
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:192.168.|:8.225|Port:5343|.", row_text(0, 1));
    plan(PID_MINI, fx_home, &b); /* 3 a row: the step goes, the address and its port stay together */
    TEST_ASSERT_EQUAL_STRING("Box IP:192.168.|:8.225|Port:5343", row_text(0, 1));
    b.scen = FX_NONET; /* "1 Join" + the name and the password, each on one key */
    plan(PID_MK2, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Join|Hotspot:CompanionBox-8D1C|Password:k7m2p9xq|.|.", row_text(0, 1));
    TEST_ASSERT_EQUAL_STRING("#2 Open|Setup page:192.168.4.1|.|.|.", row_text(1, 1));
}

static void build_groups(dui_screen_t *s, const int *sizes, const int *flags, int n)
{
    static const char *W[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o"};
    dui_screen_init(s);
    dui_title(s, "Test");
    int k = 0;
    for (int g = 0; g < n; g++) {
        dui_group(s, DUI_ZONE_MAIN, flags ? flags[g] : 0);
        for (int i = 0; i < sizes[g]; i++) dui_word(s, W[k++ % 15], DUI_ROLE_VALUE);
    }
}

static void plan_groups(uint16_t pid, const int *sizes, const int *flags, int n)
{
    const dui_deck_t d = deck(pid);
    build_groups(&S, sizes, flags, n);
    dui_plan(&d, &S, &L);
}

/* A group that fits a row starts a new row rather than wrapping; a packed one that fits what is left
 * goes on in the row. */
static void test_flow_group_starts_new_row(void)
{
    const int sizes[] = {3, 4, 2}, flags[] = {0, 0, DUI_G_PACK};
    plan_groups(PID_M32, sizes, flags, 3); /* sub-screen: 3 content rows of 8 */
    TEST_ASSERT_EQUAL_STRING("a|b|c|.|.|.|.|.", row_text(0, 1));
    TEST_ASSERT_EQUAL_STRING("d|e|f|g|h|i|.|.", row_text(0, 2));
    const int sizes2[] = {3, 4, 2};
    plan_groups(PID_M32, sizes2, NULL, 3);
    TEST_ASSERT_EQUAL_STRING("d|e|f|g|.|.|.|.", row_text(0, 2));
    TEST_ASSERT_EQUAL_STRING("h|i|.|.|.|.|.|.", row_text(0, 3));
    /* a group wider than a row goes on in the next one */
    const int sizes3[] = {11};
    plan_groups(PID_M32, sizes3, NULL, 1);
    TEST_ASSERT_EQUAL_STRING("a|b|c|d|e|f|g|h", row_text(0, 1));
    TEST_ASSERT_EQUAL_STRING("i|j|k|.|.|.|.|.", row_text(0, 2));
}

/* A group that fits a page but not the rest of this one starts the next page; the pager key is
 * reserved only when there are pages. */
static void test_flow_group_starts_new_page_and_pager(void)
{
    const int one[] = {10}; /* MK.2 sub-screen: 2 content rows of 5 */
    plan_groups(PID_MK2, one, NULL, 1);
    TEST_ASSERT_EQUAL(1, L.pages);
    TEST_ASSERT_EQUAL(-1, L.pager_col);
    TEST_ASSERT_EQUAL_STRING("f|g|h|i|j", row_text(0, 2)); /* the last key holds content */

    const int two[] = {3, 7};
    plan_groups(PID_MK2, two, NULL, 2);
    TEST_ASSERT_EQUAL(2, L.pages);
    TEST_ASSERT_EQUAL(4, L.pager_col);
    TEST_ASSERT_EQUAL(2, L.pager_row);
    TEST_ASSERT_EQUAL_STRING("a|b|c|.|.", row_text(0, 1));
    TEST_ASSERT_EQUAL_STRING(".|.|.|.|>", row_text(0, 2));
    TEST_ASSERT_EQUAL_STRING("d|e|f|g|h", row_text(1, 1));
    TEST_ASSERT_EQUAL_STRING("i|j|.|.|>", row_text(1, 2));
    TEST_ASSERT_EQUAL(DUI_ACT_PAGER, at(1, 4, 2)->action);
    TEST_ASSERT_EQUAL(2, at(1, 4, 2)->num);
    TEST_ASSERT_EQUAL(1, at(1, 4, 2)->cur); /* the page shown */

    /* a menu row: ">" at its right end, only with pages (MK.2 home: 4 menu buttons + ">") */
    fx_box_t b = fx_box_default();
    plan(PID_MK2, fx_home, &b);
    TEST_ASSERT_EQUAL(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("[Network]|[Companion]|[Hotspot]|[Box]|>", row_text(0, 2));
    b.ap = false;
    plan(PID_MK2, fx_home, &b);
    TEST_ASSERT_EQUAL(1, L.pages);
    TEST_ASSERT_EQUAL_STRING("[Network]|[Companion]|[Hotspot]|[Box]|.", row_text(0, 2));
    /* hardware pagers reserve no key */
    b.ap = true;
    plan(PID_NEO, fx_network, &b);
    TEST_ASSERT_TRUE(L.pages >= 2);
    TEST_ASSERT_EQUAL(-1, L.pager_col);
}

/* Anchored groups (the hotspot) sit at the bottom rows of the last page, else on a page of their own. */
static void test_flow_anchor(void)
{
    fx_box_t b = fx_box_default();
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL(1, L.pages);
    TEST_ASSERT_EQUAL_STRING("Hotspot:Companion|Box-8D1C|Password:k7m2p9xq|.|.|.|.|.", row_text(0, 2)); /* the password on one key */
    TEST_ASSERT_EQUAL(DUI_TINT_SVC, at(0, 0, 2)->tint);
    TEST_ASSERT_EQUAL(DUI_TINT_MAIN, at(0, 1, 1)->tint);
    /* + XL: 3 content rows; the service zone in the bottom one, a blank row between */
    plan(PID_PLUSXL, fx_home, &b);
    TEST_ASSERT_EQUAL(1, L.pages);
    TEST_ASSERT_EQUAL_STRING(".|.|.|.|.|.|.|.|.", row_text(0, 1));
    TEST_ASSERT_EQUAL_STRING("Hotspot:Companion|Box-8D1C|Password:k7m2p9xq|.|.|.|.|.|.", row_text(0, 2)); /* 112 px: whole */
    /* MK.2: one content row, full: a page of its own; each value on one key */
    plan(PID_MK2, fx_home, &b);
    TEST_ASSERT_EQUAL(2, L.pages);
    TEST_ASSERT_EQUAL_STRING("Hotspot:CompanionBox-8D1C|Password:k7m2p9xq|.|.|.", row_text(1, 1));
}

/* On the big decks: a row of 4 or more holds the 4 octets together, the port beside them or on the next
 * row of the page. On the small decks: the address on one key when it fits there on one line, else on
 * two keys of two octets each, its port on the next. */
static void test_flow_octets(void)
{
    fx_box_t b = fx_box_default();
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:192|168|8|225|Port:5343|.|.", row_text(0, 1));
    /* the small decks: one key, its port beside it */
    plan(PID_NEO, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:192.168.8.225|Port:5343|.\nHotspot:CompanionBox-8D1C|Password:k7m2p9xq|.|[Menu]",
                             page_text(0));
    plan(PID_PLUS, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("#1 Add|Box IP:192.168.8.225|Port:5343|.\nHotspot:CompanionBox-8D1C|Password:k7m2p9xq|.|[Menu]",
                             page_text(0));
    plan(PID_MINI, fx_home, &b); /* on 80 px it doesn't fit one key on one line: two keys, then its port */
    TEST_ASSERT_EQUAL_STRING("Box IP:192.168.|:8.225|Port:5343", row_text(0, 1));
    b = fx_box_default(); /* the Neo home on demand: "Connected Companion", then its address on a key */
    b.scen = FX_CONNECTED;
    b.close = true;
    plan(PID_NEO, fx_home, &b);
    TEST_ASSERT_EQUAL_STRING("[x]|Connected Companion|IP:192.168.8.155|.", row_text(0, 0));
    /* MK.2 Network: the address on two keys side by side in the row */
    b = fx_box_default();
    plan(PID_MK2, fx_network, &b);
    bool found = false;
    for (int p = 0; p < L.pages && !found; p++) found = strstr(page_text(p), "IP:192.168.|:8.225") != NULL;
    TEST_ASSERT_TRUE(found);
    /* an address after other tiles of its group goes on whole: on a small deck a key with its port */
    const dui_deck_t neo = deck(PID_NEO);
    dui_screen_init(&S);
    S.menu = true;
    dui_group(&S, DUI_ZONE_MAIN, 0);
    dui_word(&S, "a", DUI_ROLE_LABEL);
    dui_word(&S, "b", DUI_ROLE_LABEL);
    dui_word(&S, "c", DUI_ROLE_LABEL);
    dui_ipv4(&S, NULL, "10.0.0.12");
    dui_port(&S, NULL, "5343");
    dui_plan(&neo, &S, &L);
    TEST_ASSERT_EQUAL_STRING("a|b|c|.\n10.0.0.12|5343|.|[Menu]", page_text(0));
    /* ... and with room for the port beside its octets, one row (+ XL's rows of 9: octets over keys) */
    const dui_deck_t pxl = deck(PID_PLUSXL);
    dui_plan(&pxl, &S, &L);
    TEST_ASSERT_EQUAL_STRING("a|b|c|10|0|0|12|5343|.", row_text(0, 0));
}

/* Values over keys (statusui's pieces) on the big decks: one key when it reads comfortably there, else
 * pieces at their natural breaks, together on a row when they fit. on the small decks a value stays on one
 * key, on 2 lines at its natural break. */
static int ttlen(const dui_tile_t *t);

static void test_flow_value_pieces(void)
{
    fx_box_t b = fx_box_default();
    plan(PID_M32, fx_network, &b);
    TEST_ASSERT_EQUAL_STRING("(Main)|Simbox-|Home|Signal:-58|IP:192|168|8|225", row_text(0, 2)); /* the reference fixture */
    TEST_ASSERT_EQUAL_STRING("(Backup)|Office-5G|Standby|.|.|.|.|>", row_text(0, 3));
    TEST_ASSERT_EQUAL_STRING("Name:Companion|Box-8D1C|.local|.|.|.|.|.", row_text(1, 1));
    plan(PID_MK2, fx_network, &b); /* 72 px: "Simbox-Home" on one key, "Simbox-" / "Home" */
    const dui_tile_t *v = NULL;
    for (int j = 0; j < L.ntiles; j++)
        if (L.tile[j].text && !strncmp(L.tile[j].text, "Simbox-Home", 11) && ttlen(&L.tile[j]) == 11) v = &L.tile[j];
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(2, v->fit.nl);
    TEST_ASSERT_EQUAL(7, v->fit.cut);
    TEST_ASSERT_GREATER_OR_EQUAL(DUI_TEXT_MIN_PX, v->fit.px);
    /* the Mini's hotspot: each value on its key */
    plan(PID_MINI, fx_hotspot, &b);
    TEST_ASSERT_EQUAL_STRING("[<Hotspot]|Hotspot:CompanionBox-8D1C|.", row_text(0, 0));
    TEST_ASSERT_EQUAL_STRING("Password:k7m2p9xq", first_key(row_text(0, 1)));
}

/* ---- Readability ---- */

static const uint16_t SMALL_PIDS[] = {PID_MK2, PID_M15, PID_ORIG, PID_V2, PID_PLUS, PID_NEO, PID_MINI, PID_DISCORD, PID_M6};
static const uint16_t ALL_PIDS[] = {PID_M32, PID_XL, PID_PLUSXL, PID_MK2, PID_M15, PID_ORIG, PID_V2,
                                    PID_PLUS, PID_NEO, PID_MINI, PID_DISCORD, PID_M6};

static int ttlen(const dui_tile_t *t) { return t->text ? (t->len ? t->len : (int)strlen(t->text)) : 0; }

/* Every fixture screen, in its variants, through fn(name). */
static void each_fixture(void (*fn)(const char *name))
{
    static const struct { const char *name; void (*build)(dui_screen_t *, const fx_box_t *); } SCR[] = {
        {"home", fx_home}, {"menu", fx_menu}, {"network", fx_network}, {"companion", fx_companion},
        {"hotspot", fx_hotspot}, {"qr", fx_qr}, {"box", fx_box}};
    for (size_t k = 0; k < sizeof SCR / sizeof SCR[0]; k++)
        for (int scen = 0; scen < FX_SCENS; scen++)
            for (int v = 0; v < 4; v++) {
                fx_box_t b = fx_box_default();
                b.scen = scen;
                b.ap = v != 1;
                b.ap_open = v == 2;
                b.backup = v == 3;
                b.eth = v == 2;
                b.slot = v % 3;
                b.close = v == 1;
                SCR[k].build(&S, &b);
                fn(SCR[k].name);
            }
}

static uint16_t R92_PID;
static char R92_MSG[64];

/* An added '-' only in a status word or label with no natural break, 3 letters at least either side, and the
 * second line starting with a consonant and a vowel ("Recon-" / "necting"). */
static void check_hyphen(const dui_tile_t *t)
{
    if (!t->fit.hyph) return;
    TEST_ASSERT_EQUAL_MESSAGE(DUI_TILE_WORD, t->kind, R92_MSG);
    TEST_ASSERT_TRUE_MESSAGE(t->role != DUI_ROLE_VALUE && t->role != DUI_ROLE_ACCENT, R92_MSG);
    const int n = ttlen(t), c = t->fit.cut;
    TEST_ASSERT_TRUE_MESSAGE(c >= 3 && n - c >= 3, R92_MSG);
    for (int i = 0; i < n; i++)
        TEST_ASSERT_TRUE_MESSAGE((t->text[i] >= 'a' && t->text[i] <= 'z') || (t->text[i] >= 'A' && t->text[i] <= 'Z'), R92_MSG);
    TEST_ASSERT_NULL_MESSAGE(strchr("aeiouyAEIOUY", t->text[c]), R92_MSG);
    TEST_ASSERT_NOT_NULL_MESSAGE(strchr("aeiouyAEIOUY", t->text[c + 1]), R92_MSG);
}

/* On a small deck no tile holds a piece of a word or a value: each value is one tile; an IPv4 address one tile
 * on one line, or two of two octets each, one line each at one size, together on one row and page; no
 * word hyphenated. */
static void check_small_whole(const char *name)
{
    const dui_deck_t d = deck(R92_PID);
    dui_plan(&d, &S, &L);
    snprintf(R92_MSG, sizeof R92_MSG, "pid 0x%04X %s", R92_PID, name);
    for (int i = 0; i < S.ntiles; i++) {
        const dui_tile_t *t = &S.tile[i];
        const bool value = (t->flags & DUI_F_PIECES) && ttlen(t), ip = (t->flags & DUI_F_OCTET) && t->run >= 4;
        if (!value && !ip) continue;
        const int full = ip ? (int)strlen(t->text) : ttlen(t); /* an address: the whole pooled text */
        bool whole = false;
        for (int j = 0; j < L.ntiles && !whole; j++) {
            const dui_tile_t *a = &L.tile[j];
            if (a->text != t->text) continue;
            if (ttlen(a) == full && (!ip || (a->flags & DUI_F_IP))) whole = true;
            else if (ip && j + 1 < L.ntiles && (a->flags & DUI_F_IP) && a->run >= 2) { /* the pair */
                const dui_tile_t *b = &L.tile[j + 1];
                whole = (b->flags & DUI_F_IP) && b->text == a->text + ttlen(a) && ttlen(a) + ttlen(b) == full &&
                        a->text[ttlen(a) - 1] == '.' && a->fit.px == b->fit.px;
                bool same_key_row = false; /* on one page, side by side */
                for (int p = 0; p < L.pages && !same_key_row; p++)
                    for (int k = 0; k + 1 < DUI_KEYS_MAX; k++)
                        if (L.key[p][k] == j && L.key[p][k + 1] == j + 1 && (k + 1) % d.cols) same_key_row = true;
                whole &= same_key_row;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(whole, R92_MSG);
    }
    for (int j = 0; j < L.ntiles; j++) {
        TEST_ASSERT_FALSE_MESSAGE(L.tile[j].flags & DUI_F_OCTET, R92_MSG);
        check_hyphen(&L.tile[j]);
        if (L.tile[j].flags & DUI_F_IP) TEST_ASSERT_EQUAL_MESSAGE(1, L.tile[j].fit.nl, R92_MSG); /* one line */
    }
}

static void test_r92_small_decks_never_split(void)
{
    for (size_t m = 0; m < sizeof SMALL_PIDS / sizeof SMALL_PIDS[0]; m++) {
        R92_PID = SMALL_PIDS[m];
        each_fixture(check_small_whole);
    }
    /* the address over two keys, the first two octets with their dot, then the last two; one line each,
     * one size, the caption on the first */
    fx_box_t b = fx_box_default();
    plan(PID_MK2, fx_home, &b);
    const dui_tile_t *ip = NULL;
    for (int j = 0; j < L.ntiles && !ip; j++)
        if (L.tile[j].flags & DUI_F_IP) ip = &L.tile[j];
    TEST_ASSERT_NOT_NULL(ip);
    TEST_ASSERT_EQUAL(8, ttlen(ip)); /* "192.168." */
    TEST_ASSERT_EQUAL_STRING_LEN("192.168.", ip->text, 8);
    TEST_ASSERT_EQUAL(5, ttlen(ip + 1)); /* "8.225" */
    TEST_ASSERT_EQUAL_STRING_LEN("8.225", ip[1].text, 5);
    TEST_ASSERT_EQUAL(1, ip->fit.nl);
    TEST_ASSERT_EQUAL(1, ip[1].fit.nl);
    TEST_ASSERT_EQUAL(ip->fit.px, ip[1].fit.px);
    TEST_ASSERT_GREATER_OR_EQUAL(DUI_TEXT_MIN_PX, ip->fit.px);
    TEST_ASSERT_EQUAL_STRING("Box IP", ip->caption);
    TEST_ASSERT_NULL(ip[1].caption);
    /* the big decks keep the octets over keys; "Reconnecting" never over keys: whole on a key, or on two lines
     * of one key at its syllable-like break */
    plan(PID_M32, fx_home, &b);
    TEST_ASSERT_TRUE(strstr(row_text(0, 1), "192|168|8|225") != NULL);
    b.scen = FX_RECON;
    for (size_t m = 0; m < sizeof ALL_PIDS / sizeof ALL_PIDS[0]; m++) {
        plan(ALL_PIDS[m], fx_home, &b);
        snprintf(R92_MSG, sizeof R92_MSG, "pid 0x%04X recon", ALL_PIDS[m]);
        for (int j = 0; j < L.ntiles; j++) {
            const dui_tile_t *t = &L.tile[j];
            if (t->kind == DUI_TILE_WORD && t->text) TEST_ASSERT_NULL(strstr(t->text, "Recon-"));
            check_hyphen(t);
            if (t->fit.hyph && t->text && !strncmp(t->text, "Reconnecting", 12)) TEST_ASSERT_EQUAL(5, t->fit.cut);
        }
    }
}

/* The smallest sizes, real pixels, on every model: values and words 16, captions 12, button labels 14 (the
 * font has no 13). */
static void check_min_sizes(const char *name)
{
    const dui_deck_t d = deck(R92_PID);
    dui_plan(&d, &S, &L);
    for (int p = 0; p < L.pages; p++)
        for (int r = 0; r < d.rows; r++)
            for (int c = 0; c < d.cols; c++) {
                dui_tile_t t;
                if (!dui_layout_key(&L, p, c, r, &t)) continue;
                snprintf(R92_MSG, sizeof R92_MSG, "pid 0x%04X %s p%d (%d,%d) %.20s", R92_PID, name, p, c, r, t.text ? t.text : "");
                const bool text = (t.kind == DUI_TILE_WORD || t.kind == DUI_TILE_CAP || t.kind == DUI_TILE_STEP) && ttlen(&t);
                if (text) TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_TEXT_MIN_PX, t.fit.px, R92_MSG);
                if (text && t.kind == DUI_TILE_CAP && t.caption && t.caption[0])
                    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_CAP_MIN_PX, t.fit.cap_px, R92_MSG);
                if (t.kind == DUI_TILE_BUTTON && ttlen(&t)) /* the Back key's title too */
                    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_LABEL_MIN_PX, dui_label_px(t.text, ttlen(&t), d.key_px), R92_MSG);
                if (t.kind == DUI_TILE_BUTTON && t.hold_ms && t.hint) /* its release-early hint too */
                    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_LABEL_MIN_PX, dui_hint_px(&t, d.key_px), R92_MSG);
            }
}

static void test_r92_minimum_sizes(void)
{
    TEST_ASSERT_EQUAL(16, DUI_TEXT_MIN_PX);
    TEST_ASSERT_EQUAL(12, DUI_CAP_MIN_PX);
    TEST_ASSERT_EQUAL(14, DUI_LABEL_MIN_PX);
    for (size_t m = 0; m < sizeof ALL_PIDS / sizeof ALL_PIDS[0]; m++) {
        R92_PID = ALL_PIDS[m];
        each_fixture(check_min_sizes);
    }
}

/* Every fixture screen on every model: the ink inside the margins, descenders included, and
 * inside the buttons' outlines. */
static void check_fits(const char *name)
{
    const dui_deck_t d = deck(R92_PID);
    dui_plan(&d, &S, &L);
    char where[48];
    snprintf(where, sizeof where, "pid 0x%04X %s", R92_PID, name);
    fit_layout(&d, &L, where);
}

/* The strip and the info bar keep the floors too: their texts at 16 px or more, a toast 14. */
static void check_bar_sizes(const char *name)
{
    const dui_deck_t d = deck(R92_PID);
    dui_plan(&d, &S, &L);
    static uint8_t bar[1200 * 100 * 3];
    fit_min_px = 0;
    dui__text_probe = min_probe;
    dui_render_bar(&S.status, S.sub, 0, L.pages, d.bar_w, d.bar_h, bar);
    dui__text_probe = NULL;
    snprintf(R92_MSG, sizeof R92_MSG, "pid 0x%04X %s bar", R92_PID, name);
    if (fit_min_px) TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(S.status.toast ? DUI_LABEL_MIN_PX : DUI_TEXT_MIN_PX, fit_min_px, R92_MSG);
}

static void test_r92_bar_sizes(void)
{
    const uint16_t bars[] = {PID_PLUS, PID_PLUSXL, PID_NEO};
    for (size_t m = 0; m < sizeof bars / sizeof bars[0]; m++) {
        R92_PID = bars[m];
        each_fixture(check_bar_sizes);
    }
}

static void test_r92_fits_in_key(void)
{
    for (size_t m = 0; m < sizeof ALL_PIDS / sizeof ALL_PIDS[0]; m++) {
        R92_PID = ALL_PIDS[m];
        each_fixture(check_fits);
    }
}

/* What doesn't fit is dropped and flagged: a refused group's tiles go to the sink (not into the last
 * group), a full pool gives "", content past DUI_PAGES_MAX is dropped. */
static void test_overflow(void)
{
    dui_screen_init(&S);
    for (int g = 0; g < DUI_GROUPS_MAX; g++) {
        dui_group(&S, DUI_ZONE_MAIN, g == DUI_GROUPS_MAX - 1 ? DUI_G_ANCHOR : 0);
        dui_word(&S, "w", DUI_ROLE_VALUE);
    }
    TEST_ASSERT_FALSE(S.overflow);
    dui_group(&S, DUI_ZONE_MAIN, 0); /* the 9th */
    dui_tile_t *t = dui_word(&S, "lost", DUI_ROLE_VALUE);
    TEST_ASSERT_TRUE(S.overflow);
    TEST_ASSERT_EQUAL_PTR(&S.sink, t);
    TEST_ASSERT_EQUAL(1, S.group[DUI_GROUPS_MAX - 1].n); /* the anchored group didn't take it */
    TEST_ASSERT_EQUAL(DUI_GROUPS_MAX, S.ntiles);
    const dui_deck_t m32 = deck(PID_M32);
    dui_plan(&m32, &S, &L);
    TEST_ASSERT_TRUE(L.overflow); /* the screen's */

    dui_screen_init(&S); /* the pool */
    char big[DUI_POOL + 1]; /* one char more than the pool holds with its NUL */
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    TEST_ASSERT_EQUAL_STRING("", dui_str(&S, big));
    TEST_ASSERT_TRUE(S.overflow);

    dui_screen_init(&S); /* the pages: 40 tiles on the Mini's sub-screen, 4 a page */
    dui_title(&S, "Many");
    dui_group(&S, DUI_ZONE_MAIN, 0);
    for (int i = 0; i < DUI_SCREEN_TILES; i++) dui_word(&S, "w", DUI_ROLE_VALUE);
    TEST_ASSERT_FALSE(S.overflow);
    const dui_deck_t mini = deck(PID_MINI);
    dui_plan(&mini, &S, &L);
    TEST_ASSERT_EQUAL(DUI_PAGES_MAX, L.pages);
    TEST_ASSERT_TRUE(L.overflow);
    plan(PID_M32, fx_box, &(fx_box_t){.scen = FX_DOCK, .ap = true}); /* a screen that fits */
    TEST_ASSERT_FALSE(L.overflow);
}

/* ---- row alignment ---- */

static dui_tile_t T[8];

static void check_row_aligned(dui_tile_t *const *row, int n, int key_px, const char *msg)
{
    int base = -1;
    int px[DUI_ROLES + 1];
    for (int i = 0; i < DUI_ROLES + 1; i++) px[i] = -1;
    bool cap = false;
    for (int i = 0; i < n; i++)
        if (row[i] && (row[i]->kind == DUI_TILE_CAP || row[i]->kind == DUI_TILE_STEP)) cap = true;
    for (int i = 0; i < n; i++) {
        const dui_tile_t *t = row[i];
        if (!t || (t->kind != DUI_TILE_WORD && t->kind != DUI_TILE_CAP && t->kind != DUI_TILE_STEP)) continue;
        TEST_ASSERT_TRUE_MESSAGE(t->fit.nl == 1 || t->fit.nl == 2, msg);
        TEST_ASSERT_EQUAL_MESSAGE(cap, t->fit.rowcap, msg);
        if (base < 0) base = t->fit.base;
        TEST_ASSERT_EQUAL_MESSAGE(base, t->fit.base, msg); /* one baseline */
        if (t->fit.nl != 1) continue;
        const int role = t->kind == DUI_TILE_STEP ? DUI_ROLES : t->role;
        if (px[role] < 0) px[role] = t->fit.px;
        TEST_ASSERT_EQUAL_MESSAGE(px[role], t->fit.px, msg); /* one size per role */
    }
    if (base >= 0) { /* under the caption line, or centred */
        if (cap) TEST_ASSERT_GREATER_THAN_MESSAGE(key_px * 60 / 96, base, msg);
        else TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(key_px * 66 / 96, base, msg);
        TEST_ASSERT_LESS_THAN_MESSAGE(key_px - 4, base, msg);
    }
}

static dui_tile_t mk(int kind, const char *caption, const char *text, int role)
{
    dui_tile_t t;
    memset(&t, 0, sizeof t);
    t.kind = (uint8_t)kind;
    t.caption = caption;
    t.text = text;
    t.role = (uint8_t)role;
    return t;
}

static void test_align_caption_row(void)
{
    T[0] = mk(DUI_TILE_STEP, NULL, "Add", DUI_ROLE_VALUE);
    T[0].num = 1;
    T[1] = mk(DUI_TILE_CAP, "Box IP", "192", DUI_ROLE_ACCENT);
    T[2] = mk(DUI_TILE_WORD, NULL, "168", DUI_ROLE_ACCENT);
    T[3] = mk(DUI_TILE_WORD, NULL, "8", DUI_ROLE_ACCENT);
    T[4] = mk(DUI_TILE_CAP, "Port", "5343", DUI_ROLE_ACCENT);
    T[5] = mk(DUI_TILE_WORD, NULL, "Waiting", DUI_ROLE_WAIT);
    dui_tile_t *row[] = {&T[0], &T[1], &T[2], &T[3], &T[4], NULL, &T[5]};
    const int sizes[] = {72, 80, 96, 112, 120};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        dui_align_row(row, 7, sizes[k]);
        check_row_aligned(row, 7, sizes[k], "caption row");
        TEST_ASSERT_EQUAL(1, T[3].fit.nl);
        TEST_ASSERT_EQUAL(T[1].fit.px, T[3].fit.px); /* "8" as large as "5343": one size per role */
        TEST_ASSERT_LESS_OR_EQUAL(sizes[k] * 36 / 96 + 1, T[1].fit.px);
    }
}

static void test_align_plain_row(void)
{
    T[0] = mk(DUI_TILE_WORD, NULL, "Simbox-", DUI_ROLE_VALUE);
    T[1] = mk(DUI_TILE_WORD, NULL, "Home", DUI_ROLE_VALUE);
    T[2] = mk(DUI_TILE_WORD, NULL, "Standby", DUI_ROLE_LABEL);
    T[3] = mk(DUI_TILE_ICON, NULL, "Main", DUI_ROLE_VALUE);
    dui_tile_t *row[] = {&T[3], &T[0], &T[1], &T[2]};
    const int sizes[] = {72, 80, 96, 112, 120};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        dui_align_row(row, 4, sizes[k]);
        check_row_aligned(row, 4, sizes[k], "plain row");
        TEST_ASSERT_EQUAL(T[0].fit.px, T[1].fit.px);
        TEST_ASSERT_EQUAL(0, T[3].fit.nl); /* not a text tile */
        /* centred: the tallest role's capitals about the key's middle */
        const sui_font_size_t *f = sui_font_find(T[0].fit.px > T[2].fit.px ? T[0].fit.px : T[2].fit.px);
        TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_INT_WITHIN(2, sizes[k] / 2, T[0].fit.base - f->cap / 2);
    }
}

static dui_tile_t *const *one_of(dui_tile_t *t)
{
    static dui_tile_t *one[1];
    one[0] = t;
    return one;
}

/* A word one key can't hold on one line at 16 px: two lines inside the key; a word with no natural break at a
 * syllable-like point with a '-' added ("Recon-" / "necting", never "Reconn" / "ecting"); a value at its
 * natural break, never given a '-'. */
static void test_align_two_lines(void)
{
    T[0] = mk(DUI_TILE_WORD, NULL, "Reconnecting", DUI_ROLE_WAIT);
    T[1] = mk(DUI_TILE_WORD, NULL, "to", DUI_ROLE_LABEL);
    dui_tile_t *row[] = {&T[0], &T[1]};
    dui_align_row(row, 2, 72);
    TEST_ASSERT_EQUAL(2, T[0].fit.nl);
    TEST_ASSERT_EQUAL(1, T[0].fit.hyph);
    TEST_ASSERT_EQUAL(5, T[0].fit.cut); /* "Recon-" | "necting" */
    TEST_ASSERT_GREATER_OR_EQUAL(DUI_TEXT_MIN_PX, T[0].fit.px);
    TEST_ASSERT_EQUAL(1, T[1].fit.nl);
    T[0] = mk(DUI_TILE_WORD, NULL, "k7m2p9xqzzzwr", DUI_ROLE_VALUE); /* a value: never an added '-' */
    dui_align_row(one_of(&T[0]), 1, 72);
    TEST_ASSERT_EQUAL(0, T[0].fit.hyph);
    T[0] = mk(DUI_TILE_CAP, "Wi-Fi", "CompanionBox-8D1C", DUI_ROLE_VALUE);
    dui_tile_t *one[] = {&T[0]};
    dui_align_row(one, 1, 72);
    TEST_ASSERT_EQUAL(2, T[0].fit.nl);
    TEST_ASSERT_EQUAL(9, T[0].fit.cut); /* "Companion" / "Box-8D1C": before the capital */
    TEST_ASSERT_GREATER_OR_EQUAL(DUI_TEXT_MIN_PX, T[0].fit.px);
    T[0] = mk(DUI_TILE_CAP, "IP", "192.168.", DUI_ROLE_ACCENT); /* an address's key: one line */
    T[0].flags |= DUI_F_IP;
    dui_align_row(one, 1, 72);
    TEST_ASSERT_EQUAL(1, T[0].fit.nl);
    TEST_ASSERT_GREATER_OR_EQUAL(DUI_TEXT_MIN_PX, T[0].fit.px);
}

/* The lowest row of rows [y0, y1) of a rendered key with ink (off the black background). */
static int ink_bottom(const uint8_t *p, int px, int y0, int y1)
{
    for (int y = y1 - 1; y >= y0; y--)
        for (int x = SUI_MARGIN; x < px - SUI_MARGIN; x++)
            if (p[3 * (y * px + x)] > 60 || p[3 * (y * px + x) + 1] > 60 || p[3 * (y * px + x) + 2] > 60) return y;
    return -1;
}

/* What is drawn: the one-line texts of a row (a step's verb, caption values, plain words, of different
 * roles and sizes) end on one ink baseline; words with no descender. */
static void test_align_ink_baseline(void)
{
    const int sizes[] = {72, 80, 96, 112, 120};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        const int px = sizes[k];
        uint8_t *p = calloc((size_t)px * px, 3);
        char msg[32];
        T[0] = mk(DUI_TILE_STEP, NULL, "Add", DUI_ROLE_VALUE);
        T[0].num = 1;
        T[1] = mk(DUI_TILE_CAP, "Box IP", "192", DUI_ROLE_ACCENT);
        T[2] = mk(DUI_TILE_WORD, NULL, "168", DUI_ROLE_ACCENT);
        T[3] = mk(DUI_TILE_CAP, "Uptime", "2h 14m", DUI_ROLE_VALUE);
        T[4] = mk(DUI_TILE_WORD, NULL, "In use", DUI_ROLE_OK); /* a state word that holds one line */
        dui_tile_t *row[] = {&T[0], &T[1], &T[2], &T[3], &T[4]};
        dui_align_row(row, 5, px);
        int base = -1;
        for (int i = 0; i < 5; i++) {
            dui_render_key(&T[i], px, p);
            const int b = ink_bottom(p, px, px * 45 / 100, px - 2); /* under the step's circle, the caption */
            snprintf(msg, sizeof msg, "caption row, key %d, tile %d", px, i);
            TEST_ASSERT_GREATER_THAN_MESSAGE(px / 2, b, msg);
            if (base < 0) base = b;
            TEST_ASSERT_INT_WITHIN_MESSAGE(1, base, b, msg);
        }
        T[0] = mk(DUI_TILE_WORD, NULL, "Simbox-", DUI_ROLE_VALUE);
        T[1] = mk(DUI_TILE_WORD, NULL, "Home", DUI_ROLE_VALUE);
        T[2] = mk(DUI_TILE_WORD, NULL, "No cable", DUI_ROLE_LABEL);
        T[3] = mk(DUI_TILE_WORD, NULL, "8", DUI_ROLE_ACCENT);
        dui_align_row(row, 4, px);
        base = -1;
        for (int i = 0; i < 4; i++) {
            dui_render_key(&T[i], px, p);
            const int b = ink_bottom(p, px, 0, px);
            snprintf(msg, sizeof msg, "plain row, key %d, tile %d", px, i);
            if (base < 0) base = b;
            TEST_ASSERT_INT_WITHIN_MESSAGE(1, base, b, msg);
        }
        free(p);
    }
}

/* Every row of every fixture screen on every model is aligned. */
static void test_align_every_layout(void)
{
    const uint16_t P[] = {PID_M32, PID_PLUSXL, PID_MK2, PID_PLUS, PID_NEO, PID_MINI};
    void (*const B[])(dui_screen_t *, const fx_box_t *) = {fx_home, fx_menu, fx_network, fx_companion, fx_hotspot, fx_qr, fx_box};
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        for (size_t k = 0; k < sizeof B / sizeof B[0]; k++)
            for (int scen = 0; scen < FX_SCENS; scen++) {
                fx_box_t b = box_with(scen, true);
                plan(P[i], B[k], &b);
                for (int p = 0; p < L.pages; p++)
                    for (int r = 0; r < L.deck.rows; r++) {
                        dui_tile_t row[9];
                        dui_tile_t *ptr[9];
                        for (int c = 0; c < L.deck.cols; c++) {
                            dui_layout_key(&L, p, c, r, &row[c]);
                            ptr[c] = &row[c];
                        }
                        char msg[64];
                        snprintf(msg, sizeof msg, "pid %04X screen %d scen %d page %d row %d", P[i], (int)k, scen, p, r);
                        check_row_aligned(ptr, L.deck.cols, L.deck.key_px, msg);
                    }
            }
}

/* ---- drawing ---- */

static uint8_t *img(int w, int h)
{
    uint8_t *p = calloc((size_t)w * h, 3);
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

static int lum(const uint8_t *p) { return p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]); }

static void test_icons_inside_box(void)
{
    const float sizes[] = {22, 32, 36, 51};
    for (int icon = DUI_ICON_NONE + 1; icon < DUI_ICONS; icon++)
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            const float s = sizes[k];
            const int w = (int)(s * 2);
            uint8_t *p = img(w, w);
            dui_canvas_t c = {p, w, w};
            dui_icon(&c, icon, w / 2.0f, w / 2.0f, s, 0xFFFFFF, -50, DUI_F_ON);
            int x0 = w, y0 = w, x1 = -1, y1 = -1;
            for (int y = 0; y < w; y++)
                for (int x = 0; x < w; x++)
                    if (lum(p + 3 * (y * w + x)) > 8) {
                        if (x < x0) x0 = x;
                        if (y < y0) y0 = y;
                        if (x > x1) x1 = x;
                        if (y > y1) y1 = y;
                    }
            char msg[48];
            snprintf(msg, sizeof msg, "icon %d at %.0f px", icon, s);
            TEST_ASSERT_TRUE_MESSAGE(x1 >= 0, msg); /* some ink */
            const float lo = w / 2.0f - s / 2 - 1, hi = w / 2.0f + s / 2 + 1;
            TEST_ASSERT_TRUE_MESSAGE(x0 >= lo && y0 >= lo && x1 < hi && y1 < hi, msg);
            free(p);
        }
}

/* No pixel of p (w x w) has a hue other than ink's: every channel ink's share of the brightest (the icons are
 * one ink, no red dot), within 2 of 255 for antialiasing. */
static void assert_one_ink(const uint8_t *p, int w, uint32_t ink, const char *msg)
{
    const int ir = ink >> 16 & 255, ig = ink >> 8 & 255, ib = ink & 255, imax = ir > ig ? (ir > ib ? ir : ib) : (ig > ib ? ig : ib);
    for (int i = 0; i < w * w; i++) {
        const uint8_t *q = p + 3 * i;
        const int m = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]);
        if (m < 8) continue;
        TEST_ASSERT_INT_WITHIN_MESSAGE(3, ir * m / imax, q[0], msg);
        TEST_ASSERT_INT_WITHIN_MESSAGE(3, ig * m / imax, q[1], msg);
        TEST_ASSERT_INT_WITHIN_MESSAGE(3, ib * m / imax, q[2], msg);
    }
}

/* Satellite mode is Lucide's satellite (ISC; its 24-unit box is the engine's), drawn in
 * the deck's Phosphor stroke (1.5 units) in the key's one ink: the body (a square turned 45 degrees, corners (8.5,
 * 11.5) (12.5, 15.5) (18.5, 9.5) (14.5, 5.5)), the two panels either side of it, the antenna to the upper right and
 * the signal arc round (3, 21) at the lower left; empty inside the body, between the panels' halves, and at the box's
 * other corners. */
static void test_icon_satellite(void)
{
    const int w = 96;
    const float cx = w / 2.0f, cy = w / 2.0f, u = 72 / 24.0f;
    const uint32_t inks[] = {0xFFFFFF, SUI_RGB_ACCENT, DUI_RGB_DIM};
    for (size_t k = 0; k < sizeof inks / sizeof inks[0]; k++) {
        uint8_t *p = img(w, w);
        dui_canvas_t c = {p, w, w};
        dui_icon(&c, DUI_ICON_SAT, cx, cy, 72, inks[k], 0, 0);
#define LU(x, y) lum(p + 3 * ((int)(cy + ((y) - 12) * u) * w + (int)(cx + ((x) - 12) * u)))
        const int ink = k < 2 ? 150 : 40;
        TEST_ASSERT_TRUE(LU(10.5f, 13.5f) > ink && LU(15.5f, 12.5f) > ink && LU(16.5f, 7.5f) > ink); /* the body's sides */
        TEST_ASSERT_TRUE(LU(13.5f, 10.5f) <= 8);                                                   /* ... empty inside */
        TEST_ASSERT_TRUE(LU(7.5f, 4.5f) > ink && LU(7.5f, 8.5f) > ink);   /* the upper-left panel */
        TEST_ASSERT_TRUE(LU(19.5f, 12.5f) > ink && LU(19.5f, 16.5f) > ink); /* the lower-right one */
        TEST_ASSERT_TRUE(LU(9.5f, 6.5f) <= 8 && LU(17.5f, 14.5f) <= 8);   /* ... each open in the middle */
        TEST_ASSERT_TRUE(LU(17.75f, 6.25f) > ink);                        /* the antenna */
        TEST_ASSERT_TRUE(LU(3 + 6 * 0.7071f, 21 - 6 * 0.7071f) > ink);     /* the signal arc */
        TEST_ASSERT_TRUE(LU(2, 2) <= 8 && LU(22, 22) <= 8 && LU(22, 2) <= 8); /* the other corners */
        assert_one_ink(p, w, inks[k], "satellite: one ink");
#undef LU
        free(p);
    }
}

/* WCAG 2 contrast of two 0xRRGGBB colours. */
static double lin_c(unsigned v)
{
    const double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double rel_lum(uint32_t c) { return 0.2126 * lin_c(c >> 16 & 255) + 0.7152 * lin_c(c >> 8 & 255) + 0.0722 * lin_c(c & 255); }
static double ratio(uint32_t a, uint32_t b)
{
    const double la = rel_lum(a) + 0.05, lb = rel_lum(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

/* a button's colours meet WCAG AA on its fill: the outline 3:1 (it reads as a button, not an info tile or an
 * empty key), the padlock 3:1, text 4.5:1 (the label, a danger or selected label, the HOLD tag, a tag, the release-early
 * hint). DUI_RGB_DIM is the disabled look only, and stays clearly below the label. */
static void test_button_palette_contrast(void)
{
    TEST_ASSERT_TRUE(ratio(DUI_RGB_BTN_EDGE, DUI_RGB_BTN) >= 3.0);
    TEST_ASSERT_TRUE(ratio(DUI_RGB_LOCK, DUI_RGB_BTN) >= 3.0);
    TEST_ASSERT_TRUE(ratio(SUI_RGB_VALUE, DUI_RGB_BTN) >= 4.5);
    TEST_ASSERT_TRUE(ratio(DUI_RGB_DANGER, DUI_RGB_BTN) >= 4.5);
    TEST_ASSERT_TRUE(ratio(SUI_RGB_ACCENT, DUI_RGB_BTN_SEL) >= 4.5);
    TEST_ASSERT_TRUE(ratio(SUI_RGB_ACCENT, DUI_RGB_BTN) >= 4.5);  /* a tag */
    TEST_ASSERT_TRUE(ratio(SUI_RGB_LABEL, DUI_RGB_BTN) >= 4.5);   /* HOLD */
    TEST_ASSERT_TRUE(ratio(SUI_RGB_WAIT, DUI_RGB_BTN) >= 4.5);    /* the hint */
    TEST_ASSERT_TRUE(ratio(DUI_RGB_DIM, DUI_RGB_BTN) < ratio(SUI_RGB_LABEL, DUI_RGB_BTN) / 2); /* disabled reads disabled */
}

/* A locked key is drawn disabled: its label and icon grey as a key that does nothing now
 * (DUI_F_DIM), the normal outline, and a grey padlock top left, which a key that only does nothing now lacks. */
static void test_locked_key_disabled(void)
{
    dui_tile_t t;
    memset(&t, 0, sizeof t);
    t.kind = DUI_TILE_BUTTON;
    t.icon = DUI_ICON_RESTART;
    t.text = "Restart";
    t.hold_ms = 2000;
    t.flags = DUI_F_LOCKED;
    for (int k = 0; k < 2; k++) {
        uint8_t *p = img(96, 96);
        dui_render_key(&t, 96, p);
        int white = 0, lock = 0, edge = 0, dim = 0;
        for (int i = 0; i < 96 * 96; i++) {
            const uint8_t *q = p + 3 * i;
            const int x = i % 96, y = i / 96;
            white += q[0] > 230 && q[1] > 230 && q[2] > 230;
            /* the padlock's grey (DUI_RGB_LOCK) in its corner (its icon centred at 21, 20, 18 px across): the Regular
             * strokes are about a pixel wide there, so their antialiased ink counts too: grey (not the outline's blue),
             * brighter than the disabled ink (DUI_RGB_DIM) */
            lock += x < 34 && y < 34 && q[0] > 0x70 && abs(q[0] - q[2]) < 20 && abs(q[0] - q[1]) < 20;
            edge += q[0] == 0x64 && q[1] == 0x75 && q[2] == 0x8F;
            dim += q[0] == 0x5F && q[1] == 0x66 && q[2] == 0x70;
        }
        TEST_ASSERT_EQUAL(0, white); /* the label and the icon grey, locked or doing nothing now */
        TEST_ASSERT_TRUE(dim > 100);
        TEST_ASSERT_TRUE(edge > 50); /* the normal outline */
        if (k == 0) TEST_ASSERT_TRUE(lock > 10); /* locked: the padlock */
        else TEST_ASSERT_EQUAL(0, lock);       /* does nothing now: none */
        free(p);
        t.flags = DUI_F_DIM;
        t.hold_ms = 0;
    }
}

/* Phosphor's Circuitry (Regular), at 72 px (3 px a unit; a Phosphor unit is 24 / 256 of ours,
 * centred at 128): the board's edge all round, the traces, the two pads as rings (their holes dark), and the board's
 * empty areas empty. */
static void test_icon_circuitry(void)
{
    const int w = 96;
    const float cx = w / 2.0f, cy = w / 2.0f, u = 72 / 24.0f;
    uint8_t *p = img(w, w);
    dui_canvas_t c = {p, w, w};
    dui_icon(&c, DUI_ICON_CIRCUITRY, cx, cy, 72, 0xFFFFFF, 0, 0);
#define PH(x, y) lum(p + 3 * ((int)(cy + ((y) - 128) * 0.09375f * u) * w + (int)(cx + ((x) - 128) * 0.09375f * u)))
    TEST_ASSERT_TRUE(PH(40, 128) > 150 && PH(216, 128) > 150 && PH(128, 40) > 150 && PH(128, 216) > 150); /* board */
    TEST_ASSERT_TRUE(PH(88, 120) > 150);  /* the left trace */
    TEST_ASSERT_TRUE(PH(120, 128) > 150); /* the diagonal one */
    TEST_ASSERT_TRUE(PH(152, 196) > 150); /* ... down to the bottom edge */
    TEST_ASSERT_TRUE(PH(136, 56) > 150);  /* the top-right trace */
    TEST_ASSERT_TRUE(PH(104, 168) > 150 && PH(184, 104) > 150); /* the pads' rings */
    TEST_ASSERT_TRUE(PH(88, 168) <= 60 && PH(168, 104) <= 60);  /* their holes */
    TEST_ASSERT_TRUE(PH(60, 200) <= 8);   /* the empty lower left */
    TEST_ASSERT_TRUE(PH(190, 180) <= 8);  /* the empty lower right */
    TEST_ASSERT_TRUE(PH(112, 60) <= 8);   /* between the two top traces */
#undef PH
    free(p);
}

/* Companion is Phosphor's webhooks-logo (Regular), exactly as the other Phosphor glyphs
 * are drawn (its outline filled, centred in its box), in the key's one ink. */
static void test_icon_companion(void)
{
    const int w = 96;
    const float cx = w / 2.0f, cy = w / 2.0f, size = 72;
    const uint32_t inks[] = {0xFFFFFF, SUI_RGB_ACCENT, DUI_RGB_DIM};
    for (size_t k = 0; k < sizeof inks / sizeof inks[0]; k++) {
        uint8_t *p = img(w, w), *ref = img(w, w);
        dui_canvas_t c = {p, w, w}, cr = {ref, w, w};
        dui_icon(&c, DUI_ICON_COMPANION, cx, cy, size, inks[k], 0, 0);
        const float pk = size / 24 * 24.0f / PH_BOX; /* px per Phosphor unit, as icons.c's glyph() */
        rs_fill_contours(&cr, ph_pts, ph_end, ph_glyph[PH_WEBHOOKS_LOGO].c0, ph_glyph[PH_WEBHOOKS_LOGO].nc, cx - PH_BOX / 2 * pk,
                         cy - PH_BOX / 2 * pk, pk / PH_UNIT, NULL, inks[k], 1);
        TEST_ASSERT_EQUAL_MEMORY(ref, p, (size_t)w * w * 3);
        int lit = 0;
        for (int i = 0; i < w * w; i++) lit += lum(p + 3 * i) > 40;
        TEST_ASSERT_GREATER_THAN(300, lit); /* drawn */
        assert_one_ink(p, w, inks[k], "companion: one ink");
        free(p);
        free(ref);
    }
}

static void test_icons_symmetric(void)
{
    /* Lucide's satellite and Phosphor's webhooks-logo are not symmetric; nor Phosphor's hard-drive (the Dock, its light at the right) and
     * info (its i has a foot). Phosphor's own data is symmetric to within a tenth of a px at 40 px (its round caps'
     * arcs, e.g. power's and broadcast's, end a fraction of a unit apart): up to 32 of 255 on one edge pixel. */
    const int SYM[] = {DUI_ICON_WIFI, DUI_ICON_ETH, DUI_ICON_NET, DUI_ICON_DECK, DUI_ICON_HOTSPOT, DUI_ICON_MENU,
                       DUI_ICON_RESTART, DUI_ICON_SUN_LOW, DUI_ICON_SUN_HIGH, DUI_ICON_LOCK, DUI_ICON_CLOSE,
                       DUI_ICON_LOGO};
    const int w = 64;
    for (size_t i = 0; i < sizeof SYM / sizeof SYM[0]; i++) {
        uint8_t *p = img(w, w);
        dui_canvas_t c = {p, w, w};
        dui_icon(&c, SYM[i], w / 2.0f, w / 2.0f, 40, 0xFFFFFF, -50, 0);
        int worst = 0, ink = 0;
        for (int y = 0; y < w; y++)
            for (int x = 0; x < w / 2; x++) {
                const int d = abs(p[3 * (y * w + x)] - p[3 * (y * w + w - 1 - x)]);
                if (d > worst) worst = d;
                ink += p[3 * (y * w + x)] > 128;
            }
        char msg[32];
        snprintf(msg, sizeof msg, "icon %d", SYM[i]);
        TEST_ASSERT_GREATER_THAN_MESSAGE(20, ink, msg); /* drawn */
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(32, worst, msg);
        free(p);
    }
    /* Back and forward are each other's mirror image */
    uint8_t *a = img(w, w), *b = img(w, w);
    dui_canvas_t ca = {a, w, w}, cb = {b, w, w};
    dui_icon(&ca, DUI_ICON_BACK, w / 2.0f, w / 2.0f, 40, 0xFFFFFF, 0, 0);
    dui_icon(&cb, DUI_ICON_FWD, w / 2.0f, w / 2.0f, 40, 0xFFFFFF, 0, 0);
    int worst = 0, ink = 0;
    for (int y = 0; y < w; y++)
        for (int x = 0; x < w; x++) {
            const int d = abs(a[3 * (y * w + x)] - b[3 * (y * w + w - 1 - x)]);
            if (d > worst) worst = d;
            ink += a[3 * (y * w + x)] > 128;
        }
    TEST_ASSERT_GREATER_THAN(20, ink);
    TEST_ASSERT_LESS_OR_EQUAL(12, worst);
    free(a);
    free(b);
}

/* every icon draws something at every size it is used at (a 13 px padlock to a 51 px button icon and the bars'
 * 40), and the Phosphor glyphs fill their box as the old drawings did: their ink spans most of it one way or the
 * other (the carets and the x are Phosphor's smaller glyphs, at least half). */
static void test_icons_all_drawn(void)
{
    const float sizes[] = {13, 22, 34, 40, 51};
    for (int icon = DUI_ICON_NONE + 1; icon < DUI_ICONS; icon++)
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            const float sz = sizes[k];
            const int w = (int)sz + 8;
            uint8_t *p = img(w, w);
            dui_canvas_t c = {p, w, w};
            dui_icon(&c, icon, w / 2.0f, w / 2.0f, sz, 0xFFFFFF, -50, 0);
            int ink = 0, x0 = w, x1 = -1, y0 = w, y1 = -1;
            for (int y = 0; y < w; y++)
                for (int x = 0; x < w; x++)
                    if (lum(p + 3 * (y * w + x)) > 128) {
                        ink++;
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
            char msg[40];
            snprintf(msg, sizeof msg, "icon %d at %.0f px", icon, sz);
            TEST_ASSERT_GREATER_THAN_MESSAGE((int)(sz * sz * 0.04f), ink, msg);
            const int span = (x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0) + 1;
            TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE((int)(sz * 0.5f), span, msg);
            free(p);
        }
}

/* the generated Phosphor data (phosphor.c) is sane: every glyph has contours, each closed contour has 3 points
 * or more and no repeated point, the contours follow one another (each glyph's own, every one used once), every point
 * inside Phosphor's box, and each glyph has a contour of real size (holes and separate shapes may wind either way:
 * the fill is nonzero). */
static void test_phosphor_contours(void)
{
    TEST_ASSERT_EQUAL(PH_NPTS, ph_end[PH_NCONTOURS - 1]);
    int next = 0;
    for (int g = 0; g < PH_COUNT; g++) {
        char msg[24];
        snprintf(msg, sizeof msg, "glyph %d", g);
        TEST_ASSERT_EQUAL_MESSAGE(next, ph_glyph[g].c0, msg); /* in order, none skipped */
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, ph_glyph[g].nc, msg);
        double big = 0;
        for (int ci = ph_glyph[g].c0; ci < ph_glyph[g].c0 + ph_glyph[g].nc; ci++) {
            const int p0 = ci ? ph_end[ci - 1] : 0, p1 = ph_end[ci];
            TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(3, p1 - p0, msg);
            double a = 0;
            for (int i = p0; i < p1; i++) {
                const int j = i + 1 < p1 ? i + 1 : p0;
                TEST_ASSERT_TRUE_MESSAGE(ph_pts[2 * i] >= 0 && ph_pts[2 * i] <= PH_BOX * PH_UNIT, msg);
                TEST_ASSERT_TRUE_MESSAGE(ph_pts[2 * i + 1] >= 0 && ph_pts[2 * i + 1] <= PH_BOX * PH_UNIT, msg);
                TEST_ASSERT_FALSE_MESSAGE(ph_pts[2 * i] == ph_pts[2 * j] && ph_pts[2 * i + 1] == ph_pts[2 * j + 1], msg);
                a += (double)ph_pts[2 * i] * ph_pts[2 * j + 1] - (double)ph_pts[2 * j] * ph_pts[2 * i + 1];
            }
            TEST_ASSERT_TRUE_MESSAGE(fabs(a) > 0, msg); /* not degenerate */
            if (fabs(a) / 2 > big) big = fabs(a) / 2;
        }
        TEST_ASSERT_TRUE_MESSAGE(big / ((double)PH_UNIT * PH_UNIT) > 600, msg); /* 1 % of the box or more */
        next = ph_glyph[g].c0 + ph_glyph[g].nc;
    }
    TEST_ASSERT_EQUAL(PH_NCONTOURS, next);
}

/* a contour partly off the canvas on both sides fills what is on the canvas and nothing else (the row buffer's
 * clamps), and so does one running off it far to the right, past RS_FILL_MAX_W; the canvas sits in a larger buffer whose
 * margins must stay untouched. */
static void test_fill_contours_clamps(void)
{
    const int w = 40, h = 20, m = 64;
    uint8_t *buf = calloc((size_t)(w * h * 3 + 2 * m), 1);
    TEST_ASSERT_NOT_NULL(buf);
    const dui_canvas_t c = {buf + m, w, h};
    /* a band from x -100 to 100 px, y 5 to 15, and one from x 0 to 600 at y 16 to 19, in 1/16 units at k = 1/16 */
    static const int16_t pts[] = {-100 * 16, 5 * 16, 100 * 16, 5 * 16, 100 * 16, 15 * 16, -100 * 16, 15 * 16,
                                  0, 16 * 16, 600 * 16, 16 * 16, 600 * 16, 19 * 16, 0, 19 * 16};
    static const uint16_t ends[] = {4, 8};
    rs_fill_contours(&c, pts, ends, 0, 1, 0, 0, 1.0f / 16, NULL, 0xFFFFFF, 1);
    rs_fill_contours(&c, pts, ends, 1, 1, 0, 0, 1.0f / 16, NULL, 0xFFFFFF, 1);
    TEST_ASSERT_EQUAL_HEX8(255, c.rgb[3 * (17 * w + 39)]); /* the wide one, to the canvas's right edge */
    for (int i = 0; i < m; i++) TEST_ASSERT_EQUAL_HEX8(0, buf[i]); /* before the canvas */
    for (int i = 0; i < m; i++) TEST_ASSERT_EQUAL_HEX8(0, buf[m + w * h * 3 + i]); /* after it */
    TEST_ASSERT_EQUAL_HEX8(0, c.rgb[3 * (2 * w + 5)]);      /* above the band */
    TEST_ASSERT_EQUAL_HEX8(255, c.rgb[3 * (10 * w + 0)]);   /* the band, at both edges of the canvas */
    TEST_ASSERT_EQUAL_HEX8(255, c.rgb[3 * (10 * w + 39)]);
    free(buf);
}

/* the Wi-Fi bars by rssi (wifi-high / -medium / -low): the unlit arcs at 30 %, the dot always lit; the crossed
 * Wi-Fi's slash red (a problem) and the glyph cut clear around it. */
static void test_icon_wifi_bars(void)
{
    const int w = 96;
    const float cx = w / 2.0f, cy = w / 2.0f, k = 72.0f / 256; /* px per Phosphor unit at 72 px */
#define PW(x, y) (p + 3 * ((int)(cy + ((y) - 128) * k) * w + (int)(cx + ((x) - 128) * k)))
    static const struct {
        int8_t rssi;
        bool outer, middle;
    } C[] = {{-50, true, true}, {-65, false, true}, {-80, false, false}, {0, true, true}};
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        uint8_t *p = img(w, w);
        dui_canvas_t c = {p, w, w};
        dui_icon(&c, DUI_ICON_WIFI, cx, cy, 72, 0xFFFFFF, C[i].rssi, 0);
        TEST_ASSERT_TRUE(lum(PW(128, 204)) > 230); /* the dot */
        TEST_ASSERT_TRUE(lum(PW(128, 152)) > 230); /* the inner arc (wifi-low: 140 to 164 at the top) */
        const int mid = lum(PW(128, 104)), out = lum(PW(128, 56)); /* the middle and outer arcs' tops */
        TEST_ASSERT_TRUE(C[i].middle ? mid > 230 : (mid > 50 && mid < 110));
        TEST_ASSERT_TRUE(C[i].outer ? out > 230 : (out > 50 && out < 110));
        free(p);
    }
    uint8_t *p = img(w, w);
    dui_canvas_t c = {p, w, w};
    dui_icon(&c, DUI_ICON_NONET, cx, cy, 72, SUI_RGB_LABEL, 0, 0);
    const uint8_t *s = PW(128, 128); /* on the slash (48, 40)-(208, 216): red */
    TEST_ASSERT_TRUE(s[0] > 200 && s[1] < 140 && s[2] < 140);
    const uint8_t *a = PW(200, 72); /* the outer arc right of the slash, away from it: the glyph's grey */
    TEST_ASSERT_TRUE(abs(a[0] - a[2]) < 20 && a[0] > 100);
#undef PW
    free(p);
}

static bool near(const uint8_t *p, uint32_t rgb, int tol)
{
    return abs(p[0] - (int)(rgb >> 16 & 255)) <= tol && abs(p[1] - (int)(rgb >> 8 & 255)) <= tol &&
           abs(p[2] - (int)(rgb & 255)) <= tol;
}

static dui_tile_t button(int icon, const char *label)
{
    dui_tile_t t = mk(DUI_TILE_BUTTON, NULL, label, DUI_ROLE_VALUE);
    t.icon = (uint8_t)icon;
    return t;
}

/* The outline: 2 px (on 96) round the inset rounded rect, 5 px in (scaled). */
static void test_button_outline_inset(void)
{
    const int sizes[] = {72, 80, 96, 112, 120};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        const int px = sizes[k];
        uint8_t *p = img(px, px);
        const dui_tile_t t = button(DUI_ICON_INFO, "Box");
        dui_render_key(&t, px, p);
        const int y = px / 2, inset = (5 * px + 48) / 96;
        int edge = -1;
        for (int x = 0; x < px / 4 && edge < 0; x++)
            if (near(p + 3 * (y * px + x), DUI_RGB_BTN_EDGE, 40)) edge = x;
        char msg[24];
        snprintf(msg, sizeof msg, "key %d", px);
        TEST_ASSERT_INT_WITHIN_MESSAGE(1, inset - 1, edge, msg); /* the stroke's outer half starts 1 px out */
        TEST_ASSERT_TRUE_MESSAGE(near(p + 3 * (y * px + 0), 0x000000, 0), msg);           /* outside: black */
        TEST_ASSERT_TRUE_MESSAGE(near(p + 3 * (y * px + inset + 3), DUI_RGB_BTN, 2), msg); /* inside: the fill */
        int bottom = -1; /* the bottom edge too */
        for (int k2 = 0; k2 < px / 4 && bottom < 0; k2++)
            if (near(p + 3 * ((px - 1 - k2) * px + px / 2), DUI_RGB_BTN_EDGE, 40)) bottom = k2;
        TEST_ASSERT_INT_WITHIN_MESSAGE(1, inset - 1, bottom, msg);
        free(p);
    }
}

/* Hold progress 0.5 fills about the lower half of the button. */
static void test_button_hold_half(void)
{
    const int sizes[] = {72, 96, 120};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        const int px = sizes[k];
        uint8_t *p = img(px, px);
        dui_tile_t t = button(DUI_ICON_RESTART, "Restart");
        t.hold_ms = 2000;
        t.hold = 128;
        t.flags = DUI_F_DOWN;
        dui_render_key(&t, px, p);
        const int x = px * 12 / 96; /* left of the icon and the label, under the HOLD tag */
        int top = -1, bottom = -1;
        for (int y = px / 5; y < px - px / 10; y++)
            if (near(p + 3 * (y * px + x), DUI_RGB_HOLD, 3)) {
                if (top < 0) top = y;
                bottom = y;
            }
        char msg[24];
        snprintf(msg, sizeof msg, "key %d", px);
        TEST_ASSERT_INT_WITHIN_MESSAGE(px / 24 + 1, px / 2, top, msg);
        TEST_ASSERT_GREATER_THAN_MESSAGE(px * 80 / 96, bottom, msg);
        TEST_ASSERT_TRUE_MESSAGE(near(p + 3 * ((px / 4) * px + x), DUI_RGB_BTN_DOWN, 3), msg); /* above: the pressed fill */
        t.hold = 255; /* done: all of it */
        t.flags |= DUI_F_DANGER;
        dui_render_key(&t, px, p);
        TEST_ASSERT_TRUE_MESSAGE(near(p + 3 * ((px / 4) * px + x), DUI_RGB_HOLD_DANGER, 3), msg);
        free(p);
    }
}

/* ---- QR codes ---- */

static int qr_version(const uint8_t *qr) { return (dui_qr_size(qr) - 17) / 4; }

static void test_qr_join_and_setup_url(void)
{
    char join[128];
    dui_wifi_join(join, sizeof join, "CompanionBox-8D1C", "k7m2xq9fhd"); /* the 10-char hotspot password */
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:CompanionBox-8D1C;P:k7m2xq9fhd;;", join);
    static const char *PAY[2];
    PAY[0] = join;
    PAY[1] = "http://192.168.4.1/";
    for (int i = 0; i < 2; i++) {
        uint8_t qr[DUI_QR_BUF];
        TEST_ASSERT_TRUE(dui_qr_encode(PAY[i], qr));
        TEST_ASSERT_LESS_OR_EQUAL(4, qr_version(qr));
        TEST_ASSERT_GREATER_OR_EQUAL(1, qr_version(qr));
        TEST_ASSERT_GREATER_OR_EQUAL(2, dui_qr_module_px(72, dui_qr_size(qr)));
        /* the finder pattern's corner modules */
        TEST_ASSERT_TRUE(dui_qr_dark(qr, 0, 0));
        TEST_ASSERT_TRUE(dui_qr_dark(qr, dui_qr_size(qr) - 1, 0));
        TEST_ASSERT_TRUE(dui_qr_dark(qr, 0, dui_qr_size(qr) - 1));
    }
    uint8_t qr[DUI_QR_BUF];
    /* the smallest version: the join string (45 bytes) at version 3 (ECC L: M holds 42), 3 px a module
     * on a 96 px key; the setup URL at version 2 (ECC M) */
    TEST_ASSERT_TRUE(dui_qr_encode(join, qr));
    TEST_ASSERT_EQUAL(3, qr_version(qr));
    TEST_ASSERT_EQUAL(3, dui_qr_module_px(96, dui_qr_size(qr)));
    TEST_ASSERT_TRUE(dui_qr_encode("http://192.168.4.1/", qr));
    TEST_ASSERT_EQUAL(2, qr_version(qr));
    char longer[200];
    memset(longer, 'x', sizeof longer - 1);
    longer[sizeof longer - 1] = '\0';
    TEST_ASSERT_FALSE(dui_qr_encode(longer, qr)); /* past version 4 */
    TEST_ASSERT_EQUAL(2, dui_qr_module_px(72, 33));
    TEST_ASSERT_EQUAL(3, dui_qr_module_px(112, 33));
}

static void test_wifi_join_escapes(void)
{
    char out[96];
    dui_wifi_join(out, sizeof out, "My;Net,\"x\":\\", "p;w");
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:My\\;Net\\,\\\"x\\\"\\:\\\\;P:p\\;w;;", out);
    dui_wifi_join(out, sizeof out, "Open", NULL);
    TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:Open;;", out);
    dui_wifi_join(out, sizeof out, "Open", "");
    TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:Open;;", out);
    TEST_ASSERT_EQUAL(22, dui_wifi_join(NULL, 0, "Open", NULL));
}

/* A QR tile: black modules on white, a whole number of px each, centred with at least 1 module round. */
static void test_qr_tile(void)
{
    const int px = 72;
    uint8_t *p = img(px, px);
    dui_tile_t t = mk(DUI_TILE_QR, NULL, "http://192.168.4.1/", 0);
    dui_render_key(&t, px, p);
    uint8_t qr[DUI_QR_BUF];
    TEST_ASSERT_TRUE(dui_qr_encode(t.text, qr));
    const int n = dui_qr_size(qr), m = dui_qr_module_px(px, n), off = (px - m * n) / 2;
    TEST_ASSERT_GREATER_OR_EQUAL(m, off); /* the quiet zone */
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const uint8_t *q = p + 3 * ((off + y * m + m / 2) * px + off + x * m + m / 2);
            TEST_ASSERT_EQUAL(dui_qr_dark(qr, x, y) ? 0 : 255, q[0]);
        }
    TEST_ASSERT_EQUAL(255, p[0]);
    free(p);
}

/* ---- the status bar ---- */

/* Pixels of rgb in the rect [x0, x1) x [y0, y1). */
static int count(const uint8_t *p, int w, int x0, int y0, int x1, int y1, uint32_t rgb, int tol)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (near(p + 3 * (y * w + x), rgb, tol)) n++;
    return n;
}

/* The bar of st: the tint at its corners and along its top; the text in its colour, all of it inside
 * [text_x0, text_x1) and off the top and bottom edges (the text's pixels: where the bar differs from
 * the same bar without the text). */
static void check_bar(int w, int h, const dui_status_t *st, bool sub, int pages, uint32_t tint, uint32_t text,
                      int text_x0, int text_x1)
{
    uint8_t *p = img(w, h), *q = img(w, h);
    dui_render_bar(st, sub, 0, pages, w, h, p);
    dui_status_t none = *st; /* the same without the text */
    memset(none.word, 0, sizeof none.word);
    memset(none.short_word, 0, sizeof none.short_word);
    memset(none.title, 0, sizeof none.title);
    none.text = NULL;
    none.toast = NULL;
    dui_render_bar(&none, sub, 0, pages, w, h, q);
    char msg[32];
    snprintf(msg, sizeof msg, "bar %dx%d", w, h);
    TEST_ASSERT_TRUE_MESSAGE(near(p, tint, 0), msg);
    TEST_ASSERT_TRUE_MESSAGE(near(p + 3 * (w * h - 1), tint, 0), msg);
    TEST_ASSERT_EQUAL_MESSAGE(w * 2, count(p, w, 0, 0, w, 2, tint, 0), msg); /* the top 2 rows: tint only */
    int ink = 0, outside = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *a = p + 3 * (y * w + x), *b = q + 3 * (y * w + x);
            if (!memcmp(a, b, 3)) continue;
            if (x < text_x0 || x >= text_x1 || y < 2 || y >= h - 2) outside++;
            if (near(a, text, 10)) ink++;
        }
    TEST_ASSERT_GREATER_THAN_MESSAGE(50, ink, msg); /* the text, in its colour */
    TEST_ASSERT_EQUAL_MESSAGE(0, outside, msg);     /* ... inside its room */
    free(p);
    free(q);
}

/* The text's room on the bar: from after the glyph (none on a sub-screen) to before the icons, and on
 * the strip with pages to 14 px short of the first page dot (the dots: 13 px apart, 4.5 px round, the
 * last 170 px from the right). */
static int room_x0(int h, bool sub) { return h < 80 ? (sub ? 8 : 8 + 26 + 8) : (sub ? 22 : 22 + 60 + 18); }
static int room_x1(int w, int h, int pages, bool hotspot)
{
    if (h < 80) return w - 8 - 30 - (hotspot ? 26 : 0);
    if (pages < 2) return w - 22 - 130;
    return (int)((float)w - 170 - 13.0f * (float)(pages - 1) - 4.5f - 14);
}

static void test_bar_strip_and_info(void)
{
    fx_box_t b = fx_box_default(); /* the hotspot on */
    fx_home(&S, &b); /* Waiting for Companion: amber text on the amber tint; the glyph is amber too */
    check_bar(800, 100, &S.status, false, 1, SUI_RGB_TINT_WAIT, SUI_RGB_WAIT, room_x0(100, false), room_x1(800, 100, 1, true));
    check_bar(1200, 100, &S.status, false, 3, SUI_RGB_TINT_WAIT, SUI_RGB_WAIT, room_x0(100, false), room_x1(1200, 100, 3, true));
    check_bar(248, 58, &S.status, false, 2, SUI_RGB_TINT_WAIT, SUI_RGB_WAIT, room_x0(58, false), room_x1(248, 58, 2, true));
    b.scen = FX_NONET;
    fx_home(&S, &b);
    check_bar(248, 58, &S.status, false, 1, SUI_RGB_TINT_BAD, SUI_RGB_BAD, room_x0(58, false), room_x1(248, 58, 1, true));
    check_bar(800, 100, &S.status, false, 1, SUI_RGB_TINT_BAD, SUI_RGB_BAD, room_x0(100, false), room_x1(800, 100, 1, true));
    b.ap = false; /* no hotspot icon: more room on the info bar */
    fx_home(&S, &b);
    check_bar(248, 58, &S.status, false, 1, SUI_RGB_TINT_BAD, SUI_RGB_BAD, room_x0(58, false), room_x1(248, 58, 1, false));
    /* a sub-screen: the title in white on the neutral tint, no glyph */
    b = fx_box_default();
    fx_network(&S, &b);
    check_bar(800, 100, &S.status, true, 2, DUI_RGB_TINT_NEUTRAL, SUI_RGB_VALUE, room_x0(100, true), room_x1(800, 100, 2, true));
    check_bar(248, 58, &S.status, true, 1, DUI_RGB_TINT_NEUTRAL, SUI_RGB_VALUE, room_x0(58, true), room_x1(248, 58, 1, true));
    /* a long text shrinks to its room: on the info bar, and on the strip short of 8 pages' dots */
    dui_status_t st = S.status;
    st.title[0] = "Scan";
    st.title[1] = "to";
    st.title[2] = "join";
    st.title[3] = "the very long hotspot title for the strip";
    check_bar(248, 58, &st, true, 1, DUI_RGB_TINT_NEUTRAL, SUI_RGB_VALUE, room_x0(58, true), room_x1(248, 58, 1, true));
    check_bar(800, 100, &st, true, 8, DUI_RGB_TINT_NEUTRAL, SUI_RGB_VALUE, room_x0(100, true), room_x1(800, 100, 8, true));
    check_bar(1200, 100, &st, true, 8, DUI_RGB_TINT_NEUTRAL, SUI_RGB_VALUE, room_x0(100, true), room_x1(1200, 100, 8, true));
    /* the toast on the strip: green (done), in place of the text */
    b.toast = "Restarting";
    fx_home(&S, &b);
    uint8_t *p = img(800, 100);
    dui_render_bar(&S.status, false, 0, 1, 800, 100, p);
    TEST_ASSERT_GREATER_THAN(50, count(p, 800, 0, 0, 800, 100, SUI_RGB_OK, 10));
    free(p);
}

/* The nine-dot logo (the page's i-keys: dots r 2.6 at 5/12/19 of a 24-unit box): nine separate filled
 * dots on a 3x3 grid, inside its box, the middle one at the centre. */
static void test_icon_logo(void)
{
    const int w = 96;
    static uint8_t px[96 * 96 * 3];
    memset(px, 0, sizeof px);
    const dui_canvas_t c = {px, w, w};
    dui_icon(&c, DUI_ICON_LOGO, 48, 48, 72, 0xFFFFFF, 0, 0); /* 3 px a unit */
    const float k = 3.0f;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) { /* each dot's centre is lit, the gaps between them are dark */
            const int x = (int)(48 + (i - 1) * 7 * k), y = (int)(48 + (j - 1) * 7 * k);
            TEST_ASSERT_TRUE_MESSAGE(px[3 * (y * w + x)] > 200, "dot centre");
            if (i < 2) TEST_ASSERT_TRUE_MESSAGE(px[3 * (y * w + x + (int)(3.5f * k))] < 40, "gap between dots");
        }
    int minx = w, maxx = -1;
    for (int y = 0; y < w; y++)
        for (int x = 0; x < w; x++)
            if (px[3 * (y * w + x)] > 16) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
            }
    TEST_ASSERT_INT_WITHIN(2, (int)(48 - 9.6f * k), minx); /* dots 7 units apart, r 2.6: ±9.6 units */
    TEST_ASSERT_INT_WITHIN(2, (int)(48 + 9.6f * k), maxx);
}

/* Fingerprints: the same content (copied strings too) the same; any part of a key's look changes it. */
static void test_tile_hash(void)
{
    dui_tile_t a;
    memset(&a, 0, sizeof a);
    a.kind = DUI_TILE_BUTTON;
    a.icon = DUI_ICON_RESTART;
    a.text = "Restart";
    a.hint = "restart";
    a.hold_ms = 2000;
    char copy[16];
    strcpy(copy, "Restart");
    dui_tile_t b = a;
    b.text = copy;
    TEST_ASSERT_EQUAL_UINT32(dui_tile_hash(&a), dui_tile_hash(&b));
    const uint32_t h = dui_tile_hash(&a);
    b = a;
    b.flags |= DUI_F_DOWN;
    TEST_ASSERT_NOT_EQUAL(h, dui_tile_hash(&b));
    b = a;
    b.hold = 128;
    TEST_ASSERT_NOT_EQUAL(h, dui_tile_hash(&b));
    b = a;
    b.flags |= DUI_F_HINT;
    TEST_ASSERT_NOT_EQUAL(h, dui_tile_hash(&b));
    b = a;
    b.flags |= DUI_F_LOCKED;
    TEST_ASSERT_NOT_EQUAL(h, dui_tile_hash(&b));
    b = a;
    b.phase = 7;
    TEST_ASSERT_NOT_EQUAL(h, dui_tile_hash(&b));
    b = a;
    copy[0] = 'r';
    b.text = copy;
    TEST_ASSERT_NOT_EQUAL(h, dui_tile_hash(&b));
    b = a;
    b.text = "Restart now";
    b.len = 7; /* the same 7 chars shown */
    TEST_ASSERT_EQUAL_UINT32(h, dui_tile_hash(&b));
    b = a;
    b.fit.px = 20;
    TEST_ASSERT_NOT_EQUAL(h, dui_tile_hash(&b));
    dui_tile_t e;
    memset(&e, 0, sizeof e);
    TEST_ASSERT_NOT_EQUAL(0, dui_tile_hash(&e)); /* 0 is free for "never drawn" */

    dui_status_t st;
    memset(&st, 0, sizeof st);
    st.state = DUI_STATE_WAIT;
    st.word[0] = "Waiting";
    st.text = "Waiting for Companion";
    const uint32_t bh = dui_bar_hash(&st, false, 0, 2);
    TEST_ASSERT_EQUAL_UINT32(bh, dui_bar_hash(&st, false, 0, 2));
    TEST_ASSERT_NOT_EQUAL(bh, dui_bar_hash(&st, false, 1, 2));
    TEST_ASSERT_NOT_EQUAL(bh, dui_bar_hash(&st, true, 0, 2));
    dui_status_t s2 = st;
    s2.toast = "Restarting";
    TEST_ASSERT_NOT_EQUAL(bh, dui_bar_hash(&s2, false, 0, 2));
    s2 = st;
    s2.rssi = -70;
    TEST_ASSERT_NOT_EQUAL(bh, dui_bar_hash(&s2, false, 0, 2));
    s2 = st;
    s2.phase = 3;
    TEST_ASSERT_NOT_EQUAL(bh, dui_bar_hash(&s2, false, 0, 2));
}

/* Animation: at most DUI_ANIM_MS a frame (5 fps); only the spinner and the waiting glyph move. */
static void test_phase(void)
{
    TEST_ASSERT_EQUAL(1000 / DUI_ANIM_MS, 5);
    TEST_ASSERT_EQUAL(dui_phase(0, DUI_SPIN_MS), dui_phase(DUI_ANIM_MS - 1, DUI_SPIN_MS));
    TEST_ASSERT_NOT_EQUAL(dui_phase(0, DUI_SPIN_MS), dui_phase(DUI_ANIM_MS, DUI_SPIN_MS));
    TEST_ASSERT_EQUAL((uint8_t)(DUI_ANIM_MS * 256 / DUI_SPIN_MS), dui_phase(DUI_ANIM_MS, DUI_SPIN_MS));
    TEST_ASSERT_EQUAL(dui_phase(5 * DUI_ANIM_MS, DUI_PULSE_MS), dui_phase(5 * DUI_ANIM_MS + 199, DUI_PULSE_MS));
    int changes = 0;
    for (int64_t t = 0; t < 10000; t += 10)
        changes += dui_phase(t, DUI_PULSE_MS) != dui_phase(t + 10, DUI_PULSE_MS);
    TEST_ASSERT_TRUE(changes <= 10000 / DUI_ANIM_MS);
    /* the strip / info bar (a whole bar encoded each time) pulses at 2 fps at most */
    TEST_ASSERT_EQUAL(500, DUI_BAR_ANIM_MS);
    int bar_changes = 0;
    for (int64_t t = 0; t < 10000; t += 10) {
        const bool ch = dui_bar_phase(t) != dui_bar_phase(t + 10);
        bar_changes += ch;
        if (ch) TEST_ASSERT_EQUAL(0, (t + 10) % DUI_BAR_ANIM_MS); /* only on a 500 ms step */
    }
    TEST_ASSERT_TRUE(bar_changes >= 10 && bar_changes <= 10000 / DUI_BAR_ANIM_MS); /* it does pulse */
    dui_tile_t t;
    memset(&t, 0, sizeof t);
    t.kind = DUI_TILE_SPIN;
    TEST_ASSERT_TRUE(dui_tile_animated(&t));
    t.kind = DUI_TILE_GLYPH;
    t.state = DUI_STATE_WAIT;
    TEST_ASSERT_TRUE(dui_tile_animated(&t));
    t.state = DUI_STATE_OK;
    TEST_ASSERT_FALSE(dui_tile_animated(&t));
    t.kind = DUI_TILE_BUTTON;
    TEST_ASSERT_FALSE(dui_tile_animated(&t));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plan_status_and_menu_per_model);
    RUN_TEST(test_plan_status_row);
    RUN_TEST(test_indicators_are_buttons);
    RUN_TEST(test_plan_pager_kind_on_sub_screens);
    RUN_TEST(test_plan_back_top_left);
    RUN_TEST(test_plan_menu_key);
    RUN_TEST(test_plan_mini_full_grid);
    RUN_TEST(test_flow_optional_step);
    RUN_TEST(test_flow_group_starts_new_row);
    RUN_TEST(test_flow_group_starts_new_page_and_pager);
    RUN_TEST(test_flow_anchor);
    RUN_TEST(test_flow_octets);
    RUN_TEST(test_flow_value_pieces);
    RUN_TEST(test_r92_small_decks_never_split);
    RUN_TEST(test_r92_minimum_sizes);
    RUN_TEST(test_r92_fits_in_key);
    RUN_TEST(test_r92_bar_sizes);
    RUN_TEST(test_overflow);
    RUN_TEST(test_align_caption_row);
    RUN_TEST(test_align_plain_row);
    RUN_TEST(test_align_two_lines);
    RUN_TEST(test_align_ink_baseline);
    RUN_TEST(test_align_every_layout);
    RUN_TEST(test_icons_inside_box);
    RUN_TEST(test_icons_symmetric);
    RUN_TEST(test_icons_all_drawn);
    RUN_TEST(test_phosphor_contours);
    RUN_TEST(test_icon_wifi_bars);
    RUN_TEST(test_fill_contours_clamps);
    RUN_TEST(test_icon_satellite);
    RUN_TEST(test_icon_circuitry);
    RUN_TEST(test_button_palette_contrast);
    RUN_TEST(test_locked_key_disabled);
    RUN_TEST(test_icon_companion);
    RUN_TEST(test_icon_logo);
    RUN_TEST(test_tile_hash);
    RUN_TEST(test_phase);
    RUN_TEST(test_button_outline_inset);
    RUN_TEST(test_button_hold_half);
    RUN_TEST(test_qr_join_and_setup_url);
    RUN_TEST(test_wifi_join_escapes);
    RUN_TEST(test_qr_tile);
    RUN_TEST(test_bar_strip_and_info);
    return UNITY_END();
}
