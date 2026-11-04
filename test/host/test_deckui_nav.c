/* The maintenance UI's navigation (components/deckui/nav.c): taps and
 * holds, locks, Back and Menu, the pagers, dials and touch keys, the idle return, the
 * home's own paging and presses while the screen changes. Each test drives a deck as surfaces will:
 * build the page's screen, plan it, hand the layout to the navigation, feed it events. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "deckmodel/model.h"
#include "deckui/deckui.h"
#include "deckui/nav.h"
#include "deckui/screens.h"

void setUp(void) {}
void tearDown(void) {}

enum { PID_M32 = 0x00BA, PID_MK2 = 0x0080, PID_PLUS = 0x0084, PID_NEO = 0x009A, PID_MINI = 0x0063 };

/* ---- the rig: one deck ---- */

static struct {
    dui_deck_t d;
    dui_box_t b;
    dui_screen_t s;
    dui_layout_t L;
    dui_nav_t n;
    int64_t now;
    int rebuilds, closes, confirms;
    uint16_t action[16];
    int nact;
} R;

static dui_box_t box_default(void)
{
    dui_box_t b;
    memset(&b, 0, sizeof b);
    b.mode = DUI_MODE_DOCK;
    b.sat.state = DUI_SAT_FINDING;
    b.sat.ncomps = 2;
    strcpy(b.sat.comp[0].name, "Studio-Mac");
    strcpy(b.sat.comp[0].ip, "192.168.8.155");
    strcpy(b.sat.comp[1].name, "FOH-PC");
    strcpy(b.sat.comp[1].ip, "192.168.8.40");
    b.sat.target = b.sat.comp[0];
    b.dock.port = 5343;
    strcpy(b.net.wifi_main, "Simbox-Home");
    strcpy(b.net.wifi_backup, "Office-5G");
    b.net.wifi_net = DUI_WIFI_MAIN;
    b.net.rssi = -58;
    strcpy(b.net.wifi_ip, "192.168.8.225");
    b.net.eth_link = true;
    strcpy(b.net.eth_ip, "192.168.8.226");
    b.net.ap_on = true;
    strcpy(b.net.ap_ssid, "CompanionBox-8D1C");
    strcpy(b.net.ap_pass, "k7m2p9xq");
    b.net.ap_secured = true;
    strcpy(b.net.ap_ip, "192.168.4.1");
    strcpy(b.net.name, "CompanionBox-8D1C");
    strcpy(b.box.version, "0.1.0");
    b.box.uptime_s = 8040;
    b.box.slot = DUI_SLOT_DIFFERS;
    strcpy(b.box.other_version, "0.0.9");
    b.box.other_valid = true;
    b.brightness = 70;
    b.sound = true;
    return b;
}

/* Satellite, choose: six Companions found, so the small decks' homes have pages to turn (values stay
 * on one key, so the defaults fit fewer pages). */
static dui_box_t box_many(void)
{
    dui_box_t b = box_default();
    b.mode = DUI_MODE_SAT;
    b.sat.state = DUI_SAT_CHOOSE;
    b.sat.ncomps = 6;
    static const char *N[6] = {"Studio", "FOH", "Edit", "Stage", "Booth", "Spare"};
    for (int i = 0; i < 6; i++) {
        snprintf(b.sat.comp[i].name, sizeof b.sat.comp[i].name, "%s", N[i]);
        snprintf(b.sat.comp[i].ip, sizeof b.sat.comp[i].ip, "10.0.0.%d", 10 + i);
    }
    return b;
}

static void rebuild(void)
{
    dui_build(&R.s, dui_nav_page(&R.n), &R.b);
    dui_plan(&R.d, &R.s, &R.L);
    dui_nav_layout(&R.n, &R.L);
    R.rebuilds++;
}

static void apply(const dui_nav_out_t *o)
{
    TEST_ASSERT_TRUE_MESSAGE(o->action < DUI_A_COUNT, "an engine id handed out");
    TEST_ASSERT_TRUE_MESSAGE(o->action < DUI_A_GO_MENU || o->action > DUI_A_GO_CHOOSE, "a GO action handed out");
    if (o->action && R.nact < 16) R.action[R.nact++] = o->action;
    if (o->close) R.closes++;
    if (o->sound == DUI_SOUND_CONFIRM) R.confirms++;
    if (o->rebuild) rebuild();
}

static void rig(uint16_t pid, dui_box_t b)
{
    memset(&R, 0, sizeof R);
    TEST_ASSERT_TRUE(dui_deck_of(deck_model_find(DECK_VID_ELGATO, pid), &R.d));
    R.b = b;
    R.now = 1000;
    dui_nav_init(&R.n, &R.d, R.now);
    rebuild();
}

static dui_nav_out_t key(int col, int row, bool down)
{
    dui_nav_out_t o;
    dui_nav_key(&R.n, &R.L, col, row, down, R.now, &o);
    apply(&o);
    return o;
}

/* A tap: down, 80 ms, up; the up's output. */
static dui_nav_out_t tap(int col, int row)
{
    key(col, row, true);
    R.now += 80;
    return key(col, row, false);
}

/* Time passes to now + ms in 50 ms ticks; the outputs' redraws ORed into *acc (may be NULL). */
static void wait_ms(int64_t ms, dui_nav_out_t *acc)
{
    const int64_t end = R.now + ms;
    while (R.now < end) {
        R.now = R.now + 50 <= end ? R.now + 50 : end;
        dui_nav_out_t o;
        dui_nav_tick(&R.n, R.now, &o);
        apply(&o);
        if (acc) {
            acc->keys |= o.keys;
            acc->all |= o.all;
            acc->rebuild |= o.rebuild;
            if (o.action) acc->action = o.action;
            if (o.sound) acc->sound = o.sound;
        }
    }
}

/* Turns the pages until page p shows, as a person would: the "›" key, a dial or a touch key, or a tap on
 * a content key (the Mini's home). */
static void show_page(int p)
{
    for (int guard = 0; dui_nav_pg(&R.n) != p && guard < DUI_PAGES_MAX; guard++) {
        dui_nav_out_t o;
        if (R.L.pager == DUI_PAGER_HW) {
            if (R.d.dials) dui_nav_dial_turn(&R.n, 1, R.now, &o);
            else dui_nav_touch(&R.n, 1, R.now, &o);
            apply(&o);
        } else if (R.L.pager == DUI_PAGER_TAP) {
            tap(0, R.L.status_rows);
        } else {
            int c = R.L.pager_col, r = R.L.pager_row;
            TEST_ASSERT_TRUE_MESSAGE(c >= 0, "no pager key");
            tap(c, r);
        }
        R.now += 50;
    }
    TEST_ASSERT_EQUAL(p, dui_nav_pg(&R.n));
}

/* The key whose tile is a button or the pager doing `action` (label NULL), or the button labelled
 * label: on the page shown, else on the first page that has it, turned to (show). False: none. */
static bool locate(uint16_t action, const char *label, bool show, int *col, int *row)
{
    for (int k = 0; k <= R.L.pages; k++) {
        const int p = k == 0 ? dui_nav_pg(&R.n) : k - 1;
        for (int r = 0; r < R.d.rows; r++)
            for (int c = 0; c < R.d.cols; c++) {
                dui_tile_t t;
                if (!dui_layout_key(&R.L, p, c, r, &t) ||
                    (t.kind != DUI_TILE_BUTTON && t.kind != DUI_TILE_PAGER && !(t.kind == DUI_TILE_IND && t.action)))
                    continue;
                if (label ? !(t.text && !strcmp(t.text, label)) : t.action != action) continue;
                if (show) show_page(p);
                else if (p != dui_nav_pg(&R.n)) continue;
                *col = c;
                *row = r;
                return true;
            }
    }
    return false;
}

/* On the page shown only. */
static bool find_key(uint16_t action, int *col, int *row) { return locate(action, NULL, false, col, row); }
/* Turning to its page. */
static bool find_label(const char *label, int *col, int *row) { return locate(0, label, true, col, row); }

static void tap_action(uint16_t action)
{
    int c, r;
    TEST_ASSERT_TRUE_MESSAGE(locate(action, NULL, true, &c, &r), "no key for the action");
    tap(c, r);
}

static uint64_t bit(int col, int row) { return 1ull << (row * R.d.cols + col); }

static dui_tile_t decorated(int col, int row)
{
    dui_tile_t t;
    dui_layout_key(&R.L, dui_nav_pg(&R.n), col, row, &t);
    dui_nav_decorate(&R.n, col, row, R.now, &t);
    return t;
}

/* Box: from home (a menu row, or Menu then Box). */
static void go_box(void)
{
    if (R.L.menu_style != DUI_MENU_ROW) tap_action(DUI_ACT_MENU);
    tap_action(DUI_A_GO_BOX);
    TEST_ASSERT_EQUAL(DUI_PAGE_BOX, dui_nav_page(&R.n));
}

/* ---- taps ---- */

static void test_tap_fires_on_release_on_the_same_key(void)
{
    rig(PID_M32, box_default());
    int c, r;
    TEST_ASSERT_TRUE(find_key(DUI_A_GO_BOX, &c, &r));
    dui_nav_out_t o = key(c, r, true);
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    TEST_ASSERT_EQUAL_UINT64(bit(c, r), o.keys); /* shown pressed */
    TEST_ASSERT_FALSE(o.rebuild);
    TEST_ASSERT_TRUE(decorated(c, r).flags & DUI_F_DOWN);
    R.now += 100;
    o = key(c, r, false);
    TEST_ASSERT_TRUE(o.rebuild);
    TEST_ASSERT_TRUE(o.all);
    TEST_ASSERT_EQUAL(DUI_PAGE_BOX, dui_nav_page(&R.n));
    TEST_ASSERT_EQUAL(0, R.nact); /* a GO action stays inside */

    int dc, dr, bc, br;
    TEST_ASSERT_TRUE(find_label("Dimmer", &dc, &dr));
    TEST_ASSERT_TRUE(find_label("Brighter", &bc, &br));
    key(dc, dr, true);
    R.now += 50;
    o = key(bc, br, false); /* a release on another key */
    TEST_ASSERT_EQUAL(0, o.action);
    TEST_ASSERT_EQUAL(0, R.nact);
    o = key(bc, br, true); /* a second key while one is down: nothing */
    TEST_ASSERT_EQUAL(DUI_SOUND_NONE, o.sound);
    o = key(bc, br, false);
    TEST_ASSERT_EQUAL(0, o.action);
    TEST_ASSERT_TRUE(decorated(dc, dr).flags & DUI_F_DOWN);
    R.now += 50;
    o = key(dc, dr, false); /* its own release: fires, once */
    TEST_ASSERT_EQUAL(DUI_A_DIMMER, o.action);
    TEST_ASSERT_EQUAL_UINT64(bit(dc, dr), o.keys);
    TEST_ASSERT_FALSE(o.rebuild);
    TEST_ASSERT_FALSE(decorated(dc, dr).flags & DUI_F_DOWN);
    o = key(dc, dr, false);
    TEST_ASSERT_EQUAL(0, o.action);
    TEST_ASSERT_EQUAL(1, R.nact);
    /* an info tile clicks and does nothing */
    dui_tile_t t;
    dui_layout_key(&R.L, 0, 1, 1, &t);
    TEST_ASSERT_NOT_EQUAL(DUI_TILE_BUTTON, t.kind);
    o = key(1, 1, true);
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    TEST_ASSERT_EQUAL_UINT64(0, o.keys);
    o = key(1, 1, false);
    TEST_ASSERT_EQUAL(0, o.action);
    TEST_ASSERT_FALSE(o.rebuild);
}

/* ---- holds ---- */

static void test_hold_fires_once_at_its_time(void)
{
    rig(PID_M32, box_default());
    go_box();
    int c, r;
    TEST_ASSERT_TRUE(find_label("Restart", &c, &r));
    dui_nav_out_t o = key(c, r, true);
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    TEST_ASSERT_EQUAL(0, o.action);
    dui_nav_out_t acc = {0};
    wait_ms(1000, &acc);
    TEST_ASSERT_EQUAL_UINT64(bit(c, r), acc.keys); /* the fill redrawn as it grows */
    TEST_ASSERT_EQUAL(0, acc.action);
    dui_tile_t t = decorated(c, r);
    TEST_ASSERT_TRUE(t.flags & DUI_F_DOWN);
    TEST_ASSERT_INT_WITHIN(3, 128, t.hold); /* half way */
    wait_ms(DUI_HOLD_RESTART_MS - 1000 - 50, &acc);
    TEST_ASSERT_EQUAL(0, R.nact);
    TEST_ASSERT_EQUAL(DUI_PAGE_BOX, dui_nav_page(&R.n));
    wait_ms(50, NULL); /* exactly at 2000 ms */
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_RESTART, R.action[0]);
    TEST_ASSERT_EQUAL(1, R.confirms);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n)); /* then home */
    wait_ms(3000, NULL); /* still held: once */
    o = key(c, r, false);
    TEST_ASSERT_EQUAL(0, o.action);
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(1, R.confirms);
}

static void test_hold_released_early_shows_the_hint(void)
{
    rig(PID_M32, box_default());
    tap_action(DUI_A_GO_HOTSPOT);
    int c, r;
    TEST_ASSERT_TRUE(find_label("Turn off", &c, &r));
    key(c, r, true);
    wait_ms(DUI_HOLD_AP_OFF_MS - 100, NULL);
    dui_nav_out_t o = key(c, r, false);
    TEST_ASSERT_EQUAL(0, o.action);
    TEST_ASSERT_EQUAL(0, R.nact);
    TEST_ASSERT_EQUAL_UINT64(bit(c, r), o.keys);
    dui_tile_t t = decorated(c, r);
    TEST_ASSERT_TRUE(t.flags & DUI_F_HINT);
    TEST_ASSERT_FALSE(t.flags & DUI_F_DOWN);
    TEST_ASSERT_EQUAL(0, t.hold);
    dui_nav_out_t acc = {0};
    wait_ms(DUI_NAV_HINT_MS - 50, &acc);
    TEST_ASSERT_EQUAL_UINT64(0, acc.keys);
    TEST_ASSERT_TRUE(decorated(c, r).flags & DUI_F_HINT);
    wait_ms(50, &acc); /* 1.4 s: gone, redrawn */
    TEST_ASSERT_EQUAL_UINT64(bit(c, r), acc.keys);
    TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_HINT);
    TEST_ASSERT_EQUAL(0, R.nact);
    /* held long enough: the hotspot goes off, once (held on 3 s past its time), no hint */
    key(c, r, true);
    wait_ms(DUI_HOLD_AP_OFF_MS, NULL);
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_AP_OFF, R.action[0]);
    TEST_ASSERT_EQUAL(1, R.confirms);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOTSPOT, dui_nav_page(&R.n));
    wait_ms(3000, NULL);
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(1, R.confirms);
    o = key(c, r, false);
    TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_HINT);
}

/* The other mode: a hold, then home. */
static void test_mode_switch_hold_then_home(void)
{
    rig(PID_MK2, box_default());
    tap_action(DUI_A_GO_COMPANION);
    int c, r;
    TEST_ASSERT_TRUE(find_label("Satellite", &c, &r));
    key(c, r, true);
    wait_ms(DUI_HOLD_MODE_MS, NULL);
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_MODE_SAT, R.action[0]);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));
    key(c, r, false);
    /* Choose (satellite): the pick list, no action handed out (test_choose_page_picks) */
    dui_box_t b = box_default();
    b.mode = DUI_MODE_SAT;
    b.sat.auto_disc = true;
    rig(PID_MK2, b);
    tap_action(DUI_A_GO_COMPANION);
    TEST_ASSERT_TRUE(find_label("Choose", &c, &r));
    tap(c, r);
    TEST_ASSERT_EQUAL(0, R.nact);
    TEST_ASSERT_EQUAL(DUI_PAGE_CHOOSE, dui_nav_page(&R.n));
}

/* ---- locks ---- */

static void test_locked_button(void)
{
    dui_box_t b = box_default();
    b.locked = true;
    rig(PID_M32, b);
    go_box();
    static const char *LOCKED[] = {"Restart", "Sync now"};
    static const uint16_t ACT[] = {DUI_A_RESTART, DUI_A_SYNC};
    for (int k = 0; k < 2; k++) {
        int c, r;
        TEST_ASSERT_TRUE(find_label(LOCKED[k], &c, &r));
        dui_nav_out_t o = key(c, r, true);
        TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound); /* a press like any other, no low tone */
        TEST_ASSERT_EQUAL(0, o.action);
        TEST_ASSERT_FALSE(o.pin);
        dui_nav_out_t acc = {0};
        wait_ms(3000, &acc); /* held past Restart's 2 s: nothing fires, no fill */
        TEST_ASSERT_EQUAL(0, acc.action);
        TEST_ASSERT_EQUAL(0, R.confirms);
        dui_tile_t t = decorated(c, r);
        TEST_ASSERT_TRUE(t.flags & DUI_F_LOCKED);
        TEST_ASSERT_TRUE(t.flags & DUI_F_DOWN); /* it looks pressed while held, like any other key */
        TEST_ASSERT_EQUAL(0, t.hold);
        o = key(c, r, false);
        TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_DOWN); /* ... and released after */
        TEST_ASSERT_EQUAL(0, o.action);
        TEST_ASSERT_TRUE(o.pin); /* its release opens the PIN pad */
        TEST_ASSERT_EQUAL(ACT[k], o.pin_action);
        TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_HINT);
    }
    TEST_ASSERT_EQUAL(0, R.nact);
    TEST_ASSERT_EQUAL(0, R.confirms);
    /* unlocked (the right PIN): the screen comes back, the key pressed hasn't fired; held now, it does */
    R.b.locked = false;
    dui_nav_out_t o;
    dui_nav_box_changed(&R.n, &o);
    apply(&o);
    TEST_ASSERT_EQUAL(DUI_PAGE_BOX, dui_nav_page(&R.n));
    TEST_ASSERT_EQUAL(0, R.nact);
    int c, r;
    TEST_ASSERT_TRUE(find_label("Restart", &c, &r));
    TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_LOCKED);
    key(c, r, true);
    wait_ms(DUI_HOLD_RESTART_MS, NULL); /* the hold still applies */
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_RESTART, R.action[0]);
    key(c, r, false);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n)); /* Restart goes home, as before */
    /* what the password leaves alone still works */
    R.b.locked = true;
    dui_nav_box_changed(&R.n, &o);
    apply(&o);
    go_box();
    TEST_ASSERT_TRUE(find_label("Brighter", &c, &r));
    tap(c, r);
    TEST_ASSERT_EQUAL(2, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_BRIGHTER, R.action[1]);
}

/* the indicators: a tap on the network icon opens Network, on the hotspot icon Hotspot; a click and
 * the pressed look while held; on the strip a tap on the icon's place does the same. */
static void test_indicator_taps(void)
{
    rig(PID_M32, box_default());
    int c, r;
    TEST_ASSERT_TRUE(find_key(DUI_ACT_HOTSPOT, &c, &r));
    dui_nav_out_t o = key(c, r, true);
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    TEST_ASSERT_TRUE(decorated(c, r).flags & DUI_F_DOWN);
    R.now += 80;
    key(c, r, false);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOTSPOT, dui_nav_page(&R.n));
    TEST_ASSERT_TRUE(find_key(DUI_ACT_NET, &c, &r));
    tap(c, r);
    TEST_ASSERT_EQUAL(DUI_PAGE_NETWORK, dui_nav_page(&R.n));
    TEST_ASSERT_EQUAL(0, R.nact); /* the navigation's own */
    /* the + strip */
    rig(PID_PLUS, box_default());
    dui_status_t st = R.s.status;
    dui_nav_bar_tap(&R.n, &st, R.d.bar_w, R.d.bar_h, R.d.bar_w - 30, R.now, &o);
    apply(&o);
    TEST_ASSERT_EQUAL(DUI_PAGE_NETWORK, dui_nav_page(&R.n));
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    dui_nav_bar_tap(&R.n, &st, R.d.bar_w, R.d.bar_h, 200, R.now, &o); /* elsewhere: nothing */
    TEST_ASSERT_FALSE(o.rebuild);
    TEST_ASSERT_EQUAL(DUI_SOUND_NONE, o.sound);
}

/* A press on the PIN pad keeps the deck from going idle (home, the on-demand close). */
static void test_pin_activity_keeps_awake(void)
{
    rig(PID_M32, box_default());
    go_box();
    dui_nav_out_t acc = {0};
    wait_ms(DUI_NAV_IDLE_MS - 1000, &acc);
    dui_nav_activity(&R.n, R.now);
    wait_ms(30000, &acc);
    TEST_ASSERT_EQUAL(DUI_PAGE_BOX, dui_nav_page(&R.n));
    wait_ms(31000, &acc);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));
}

/* ---- Back and Menu ---- */

static void back(void)
{
    int c, r;
    TEST_ASSERT_TRUE_MESSAGE(find_key(DUI_ACT_BACK, &c, &r), "no Back");
    TEST_ASSERT_EQUAL(0, c);
    TEST_ASSERT_EQUAL(0, r);
    tap(c, r);
}

/* A menu-row deck: sub-screens back to home; QR back to Hotspot. */
static void test_back_on_a_menu_row_deck(void)
{
    static const uint16_t PIDS[] = {PID_M32, PID_MK2};
    static const struct { uint16_t go; int page; } SUB[] = {
        {DUI_A_GO_NETWORK, DUI_PAGE_NETWORK}, {DUI_A_GO_COMPANION, DUI_PAGE_COMPANION},
        {DUI_A_GO_HOTSPOT, DUI_PAGE_HOTSPOT}, {DUI_A_GO_BOX, DUI_PAGE_BOX}};
    for (size_t m = 0; m < 2; m++) {
        rig(PIDS[m], box_default());
        TEST_ASSERT_EQUAL(DUI_MENU_ROW, R.L.menu_style);
        TEST_ASSERT_FALSE(find_key(DUI_ACT_MENU, &(int){0}, &(int){0}));
        for (size_t k = 0; k < 4; k++) {
            tap_action(SUB[k].go);
            TEST_ASSERT_EQUAL(SUB[k].page, dui_nav_page(&R.n));
            back();
            TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));
        }
        tap_action(DUI_A_GO_HOTSPOT);
        tap_action(DUI_A_GO_QR);
        TEST_ASSERT_EQUAL(DUI_PAGE_QR, dui_nav_page(&R.n));
        back();
        TEST_ASSERT_EQUAL(DUI_PAGE_HOTSPOT, dui_nav_page(&R.n));
        back();
        TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));
    }
}

/* Companion's Choose opens the pick list and hands nothing out; Back returns to Companion; a pick is
 * handed out (Companion.Choose) and shows Companion again. */
static void test_choose_page_picks(void)
{
    dui_box_t b = box_default();
    b.mode = DUI_MODE_SAT;
    b.sat.auto_disc = true;
    rig(PID_M32, b);
    tap_action(DUI_A_GO_COMPANION);
    tap_action(DUI_A_GO_CHOOSE);
    TEST_ASSERT_EQUAL(DUI_PAGE_CHOOSE, dui_nav_page(&R.n));
    TEST_ASSERT_EQUAL(0, R.nact);
    back();
    TEST_ASSERT_EQUAL(DUI_PAGE_COMPANION, dui_nav_page(&R.n));
    TEST_ASSERT_EQUAL(0, R.nact);
    tap_action(DUI_A_GO_CHOOSE);
    tap_action(DUI_A_PICK0 + 1);
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_PICK0 + 1, R.action[0]);
    TEST_ASSERT_EQUAL(DUI_PAGE_COMPANION, dui_nav_page(&R.n));
}

/* A deck whose menu is one key: Menu opens Menu, its sub-screens go back to it, it goes back home;
 * QR back to Hotspot. */
static void test_back_on_a_menu_key_deck(void)
{
    static const uint16_t PIDS[] = {PID_PLUS, PID_NEO, PID_MINI};
    static const struct { uint16_t go; int page; } SUB[] = {
        {DUI_A_GO_NETWORK, DUI_PAGE_NETWORK}, {DUI_A_GO_COMPANION, DUI_PAGE_COMPANION},
        {DUI_A_GO_HOTSPOT, DUI_PAGE_HOTSPOT}, {DUI_A_GO_BOX, DUI_PAGE_BOX}};
    for (size_t m = 0; m < 3; m++) {
        rig(PIDS[m], box_default());
        TEST_ASSERT_NOT_EQUAL(DUI_MENU_ROW, R.L.menu_style);
        tap_action(DUI_ACT_MENU);
        TEST_ASSERT_EQUAL(DUI_PAGE_MENU, dui_nav_page(&R.n));
        for (size_t k = 0; k < 4; k++) {
            tap_action(SUB[k].go);
            TEST_ASSERT_EQUAL(SUB[k].page, dui_nav_page(&R.n));
            back();
            TEST_ASSERT_EQUAL(DUI_PAGE_MENU, dui_nav_page(&R.n));
        }
        tap_action(DUI_A_GO_HOTSPOT);
        tap_action(DUI_A_GO_QR);
        TEST_ASSERT_EQUAL(DUI_PAGE_QR, dui_nav_page(&R.n));
        back();
        TEST_ASSERT_EQUAL(DUI_PAGE_HOTSPOT, dui_nav_page(&R.n));
        back();
        TEST_ASSERT_EQUAL(DUI_PAGE_MENU, dui_nav_page(&R.n));
        back();
        TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));
    }
}

/* ---- pagers ---- */

static void test_pager_wraps(void)
{
    rig(PID_MK2, box_default()); /* home: the "›" at the menu row's right end */
    TEST_ASSERT_EQUAL(DUI_PAGER_MENUROW, R.L.pager);
    const int pages = R.L.pages;
    TEST_ASSERT_TRUE(pages >= 2);
    for (int i = 1; i <= pages; i++) {
        int c, r;
        TEST_ASSERT_TRUE(find_key(DUI_ACT_PAGER, &c, &r));
        TEST_ASSERT_EQUAL(R.d.cols - 1, c);
        key(c, r, true);
        R.now += 80;
        dui_nav_out_t o = key(c, r, false);
        TEST_ASSERT_TRUE(o.all);
        TEST_ASSERT_FALSE(o.rebuild);
        TEST_ASSERT_EQUAL(i % pages, dui_nav_pg(&R.n));
    }
    /* a sub-screen with pages: the "›" key */
    tap_action(DUI_A_GO_NETWORK);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    TEST_ASSERT_EQUAL(DUI_PAGER_KEY, R.L.pager);
    TEST_ASSERT_TRUE(R.L.pages >= 2);
    for (int i = 1; i <= R.L.pages; i++) {
        tap_action(DUI_ACT_PAGER);
        TEST_ASSERT_EQUAL(i % R.L.pages, dui_nav_pg(&R.n));
    }
}

/* The Mini's home: a tap on a key that isn't a button turns the page: a content key, a blank one, and
 * the status row's glyph and words; Menu opens Menu, Close closes, a choice picks. */
static void test_tap_to_page_on_the_mini(void)
{
    rig(PID_MINI, box_default());
    TEST_ASSERT_EQUAL(DUI_PAGER_TAP, R.L.pager);
    const int pages = R.L.pages;
    TEST_ASSERT_TRUE(pages >= 2);
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&R.L, 0, 0, 0, &t)); /* the status row: the glyph, the words */
    TEST_ASSERT_EQUAL(DUI_TILE_GLYPH, t.kind);
    TEST_ASSERT_TRUE(dui_layout_key(&R.L, 0, 1, 0, &t));
    TEST_ASSERT_EQUAL(DUI_TILE_WORD, t.kind);
    dui_nav_out_t o = tap(0, 0);
    TEST_ASSERT_TRUE(o.all);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    o = tap(1, 0);
    TEST_ASSERT_TRUE(o.all);
    TEST_ASSERT_EQUAL(2 % pages, dui_nav_pg(&R.n));
    for (int i = 1; i <= pages; i++) {
        const int before = dui_nav_pg(&R.n);
        TEST_ASSERT_TRUE(dui_layout_key(&R.L, before, 0, 1, &t));
        TEST_ASSERT_NOT_EQUAL(DUI_TILE_BUTTON, t.kind);
        dui_nav_out_t d = key(0, 1, true);
        TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, d.sound);
        TEST_ASSERT_EQUAL(before, dui_nav_pg(&R.n)); /* on its release */
        R.now += 80;
        o = key(0, 1, false);
        TEST_ASSERT_TRUE(o.all);
        TEST_ASSERT_EQUAL((before + 1) % pages, dui_nav_pg(&R.n));
    }
    /* a blank content key turns it too (on a home that has one: the hotspot's join code fills the dock home's last) */
    int bp = -1, bc = -1;
    static const int SAT[] = {-1, DUI_SAT_FINDING, DUI_SAT_RECON, DUI_SAT_CONNECTED};
    for (size_t k = 0; k < sizeof SAT / sizeof SAT[0] && bp < 0; k++) {
        dui_box_t hb = box_default();
        if (SAT[k] >= 0) {
            hb.mode = DUI_MODE_SAT;
            hb.sat.state = SAT[k];
        }
        rig(PID_MINI, hb);
        for (int p = 0; R.L.pages >= 2 && p < R.L.pages && bp < 0; p++)
            for (int c = 0; c < R.d.cols && bp < 0; c++)
                if (!dui_layout_key(&R.L, p, c, 1, &t)) bp = p, bc = c;
    }
    TEST_ASSERT_TRUE_MESSAGE(bp >= 0, "no blank content key on any of the Mini's homes");
    show_page(bp);
    R.now += 50;
    o = tap(bc, 1);
    TEST_ASSERT_TRUE(o.all);
    TEST_ASSERT_EQUAL((bp + 1) % R.L.pages, dui_nav_pg(&R.n));
    /* a button (a choice) doesn't: it picks */
    dui_box_t b = box_default();
    b.mode = DUI_MODE_SAT;
    b.sat.state = DUI_SAT_CHOOSE;
    rig(PID_MINI, b);
    TEST_ASSERT_TRUE(R.L.pages >= 2);
    int pc, pr;
    TEST_ASSERT_TRUE(find_label("Studio-Mac", &pc, &pr));
    TEST_ASSERT_EQUAL(1, pr);
    tap(pc, pr);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_PICK0, R.action[0]);
    /* on demand: Close at the top-left closes, it doesn't turn */
    R.b.ondemand = true;
    dui_nav_out_t od;
    dui_nav_ondemand(&R.n, true, R.now, &od);
    apply(&od);
    R.now += DUI_NAV_SETTLE_MS;
    TEST_ASSERT_TRUE(dui_layout_key(&R.L, 0, 0, 0, &t));
    TEST_ASSERT_EQUAL(DUI_ACT_CLOSE, t.action);
    o = tap(0, 0);
    TEST_ASSERT_TRUE(o.close);
    TEST_ASSERT_FALSE(o.all);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    /* the Menu key opens Menu; sub-screens page with "›" (full grid) */
    tap_action(DUI_ACT_MENU);
    TEST_ASSERT_EQUAL(DUI_PAGE_MENU, dui_nav_page(&R.n));
    tap_action(DUI_A_GO_NETWORK);
    TEST_ASSERT_EQUAL(DUI_PAGER_KEY, R.L.pager);
    TEST_ASSERT_TRUE(dui_layout_key(&R.L, 0, 1, 0, &t));
    TEST_ASSERT_NOT_EQUAL(DUI_TILE_BUTTON, t.kind);
    tap(1, 0);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    tap_action(DUI_ACT_PAGER);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
}

/* The Mini's Choose home with 3 Companions: page 1 is all choices; after a press its pages still turn by
 * hand, from the status row. */
static void test_mini_choose_pages_by_hand(void)
{
    dui_box_t b = box_default();
    b.mode = DUI_MODE_SAT;
    b.sat.state = DUI_SAT_CHOOSE;
    b.sat.ncomps = 3;
    strcpy(b.sat.comp[2].name, "Edit-Bay");
    strcpy(b.sat.comp[2].ip, "192.168.8.61");
    rig(PID_MINI, b);
    const int pages = R.L.pages;
    TEST_ASSERT_TRUE(pages >= 2);
    for (int c = 0; c < 3; c++) {
        dui_tile_t t;
        TEST_ASSERT_TRUE(dui_layout_key(&R.L, 0, c, 1, &t));
        TEST_ASSERT_EQUAL(DUI_TILE_BUTTON, t.kind);
    }
    tap(1, 0); /* a press: the status words turn to page 2 */
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    wait_ms(10000, NULL); /* touched: no paging by itself from here */
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    for (int i = 2; i <= pages; i++) { /* by hand, round to page 1 with its three choices */
        tap(0, 0);
        TEST_ASSERT_EQUAL(i % pages, dui_nav_pg(&R.n));
        R.now += 100;
    }
    int c, r;
    TEST_ASSERT_TRUE(find_label("Edit-Bay", &c, &r));
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    tap(c, r);
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_PICK0 + 2, R.action[0]);
}

/* + : dial turn = page +/- n, press = Back. Neo: the touch keys. */
static void test_dials_and_touch_keys(void)
{
    rig(PID_PLUS, box_many());
    TEST_ASSERT_EQUAL(DUI_PAGER_HW, R.L.pager);
    const int pages = R.L.pages;
    TEST_ASSERT_TRUE(pages >= 2);
    dui_nav_out_t o;
    dui_nav_dial_turn(&R.n, 1, R.now, &o);
    apply(&o);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    TEST_ASSERT_TRUE(o.all);
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    dui_nav_dial_turn(&R.n, -2, R.now, &o);
    TEST_ASSERT_EQUAL((1 - 2 + 2 * pages) % pages, dui_nav_pg(&R.n));
    dui_nav_dial_turn(&R.n, pages, R.now, &o); /* a full turn: the same page, nothing to draw or hear */
    TEST_ASSERT_EQUAL((1 - 2 + 2 * pages) % pages, dui_nav_pg(&R.n));
    TEST_ASSERT_FALSE(o.all);
    TEST_ASSERT_EQUAL(DUI_SOUND_NONE, o.sound);
    dui_nav_dial_press(&R.n, R.now, &o); /* home: Back does nothing */
    apply(&o);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));
    TEST_ASSERT_FALSE(o.rebuild);
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    tap_action(DUI_ACT_MENU);
    tap_action(DUI_A_GO_BOX);
    dui_nav_dial_press(&R.n, R.now, &o);
    apply(&o);
    TEST_ASSERT_EQUAL(DUI_PAGE_MENU, dui_nav_page(&R.n));
    dui_nav_dial_press(&R.n, R.now, &o);
    apply(&o);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));

    rig(PID_NEO, box_many());
    TEST_ASSERT_EQUAL(DUI_PAGER_HW, R.L.pager);
    TEST_ASSERT_TRUE(R.L.pages >= 2);
    dui_nav_touch(&R.n, 1, R.now, &o);
    apply(&o);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    dui_nav_touch(&R.n, -1, R.now, &o);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    dui_nav_touch(&R.n, -1, R.now, &o);
    TEST_ASSERT_EQUAL(R.L.pages - 1, dui_nav_pg(&R.n));
    /* one page: nothing turns, no click */
    tap_action(DUI_ACT_MENU);
    TEST_ASSERT_EQUAL(1, R.L.pages);
    dui_nav_touch(&R.n, 1, R.now, &o);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    TEST_ASSERT_FALSE(o.all);
    TEST_ASSERT_EQUAL(DUI_SOUND_NONE, o.sound);
}

/* ---- time ---- */

static void test_idle_goes_home(void)
{
    rig(PID_M32, box_default());
    go_box();
    wait_ms(30000, NULL);
    int c, r;
    TEST_ASSERT_TRUE(find_label("Dimmer", &c, &r));
    tap(c, r); /* the clock starts again */
    wait_ms(DUI_NAV_IDLE_MS - 100, NULL);
    TEST_ASSERT_EQUAL(DUI_PAGE_BOX, dui_nav_page(&R.n));
    dui_nav_out_t acc = {0};
    wait_ms(100, &acc);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));
    TEST_ASSERT_TRUE(acc.rebuild);
    TEST_ASSERT_EQUAL(0, R.closes);
    /* a key held down isn't idle */
    R.now += DUI_NAV_SETTLE_MS; /* the idle return wasn't the person's: presses wait for it to settle */
    go_box();
    TEST_ASSERT_TRUE(find_label("Dimmer", &c, &r));
    key(c, r, true);
    wait_ms(DUI_NAV_IDLE_MS + 5000, NULL);
    TEST_ASSERT_EQUAL(DUI_PAGE_BOX, dui_nav_page(&R.n));
    key(c, r, false);
}

static void test_idle_closes_on_demand(void)
{
    dui_box_t b = box_default();
    strcpy(b.dock.peer, "192.168.8.155");
    b.connected = true;
    rig(PID_M32, b);
    wait_ms(120000, NULL); /* not on demand: nothing closes */
    TEST_ASSERT_EQUAL(0, R.closes);
    go_box();
    R.b.ondemand = true;
    dui_nav_out_t o;
    dui_nav_ondemand(&R.n, true, R.now, &o);
    apply(&o);
    TEST_ASSERT_TRUE(o.rebuild);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n)); /* it opens at home */
    wait_ms(DUI_NAV_IDLE_MS - 50, NULL);
    TEST_ASSERT_EQUAL(0, R.closes);
    wait_ms(50, NULL);
    TEST_ASSERT_EQUAL(1, R.closes);
    wait_ms(10000, NULL); /* once */
    TEST_ASSERT_EQUAL(1, R.closes);
    /* the Close key */
    dui_nav_ondemand(&R.n, true, R.now, &o);
    apply(&o);
    TEST_ASSERT_TRUE(find_key(DUI_ACT_CLOSE, &(int){0}, &(int){0}));
    R.now += DUI_NAV_SETTLE_MS;
    tap_action(DUI_ACT_CLOSE);
    TEST_ASSERT_EQUAL(2, R.closes);
    R.b.ondemand = false;
    dui_nav_ondemand(&R.n, false, R.now, &o);
    apply(&o);
    TEST_ASSERT_FALSE(find_key(DUI_ACT_CLOSE, &(int){0}, &(int){0}));
    wait_ms(DUI_NAV_IDLE_MS * 2, NULL);
    TEST_ASSERT_EQUAL(2, R.closes);
}

/* The home pages by itself while untouched, every 3 s (2 s past 3 pages); a press stops it until
 * 60 s pass without one. */
static void test_auto_advance(void)
{
    rig(PID_MK2, box_default());
    const int pages = R.L.pages;
    TEST_ASSERT_TRUE(pages >= 2 && pages <= 3);
    dui_nav_out_t acc = {0};
    wait_ms(2950, &acc);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    TEST_ASSERT_FALSE(acc.all);
    wait_ms(50, &acc);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    TEST_ASSERT_TRUE(acc.all);
    TEST_ASSERT_FALSE(acc.rebuild);
    wait_ms(3000, NULL);
    TEST_ASSERT_EQUAL(2 % pages, dui_nav_pg(&R.n));
    /* a press stops it */
    wait_ms(1000, NULL);
    const int pg = dui_nav_pg(&R.n);
    tap(2, 1);
    wait_ms(DUI_NAV_IDLE_MS - 100, NULL);
    TEST_ASSERT_EQUAL(pg, dui_nav_pg(&R.n));
    wait_ms(100, NULL); /* 60 s without a press: it starts again, 3 s later */
    TEST_ASSERT_EQUAL(pg, dui_nav_pg(&R.n));
    wait_ms(2950, NULL);
    TEST_ASSERT_EQUAL(pg, dui_nav_pg(&R.n));
    wait_ms(50, NULL);
    TEST_ASSERT_EQUAL((pg + 1) % pages, dui_nav_pg(&R.n));
    /* only home pages by itself */
    R.now += DUI_NAV_SETTLE_MS;
    tap_action(DUI_A_GO_NETWORK);
    TEST_ASSERT_TRUE(R.L.pages >= 2);
    wait_ms(DUI_NAV_IDLE_MS - 100, NULL);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));

    /* more than 3 pages: every 2 s (a home that long, as the navigation sees its layout: the real homes
     * are shorter) */
    rig(PID_MINI, box_many());
    R.L.pages = 4;
    dui_nav_layout(&R.n, &R.L);
    TEST_ASSERT_TRUE(R.L.pages > 3);
    wait_ms(1950, NULL);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
    wait_ms(50, NULL);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    wait_ms(2000, NULL);
    TEST_ASSERT_EQUAL(2, dui_nav_pg(&R.n));
    /* a press via a dial or touch key stops it too */
    rig(PID_NEO, box_many());
    TEST_ASSERT_TRUE(R.L.pages >= 2);
    dui_nav_out_t o;
    dui_nav_touch(&R.n, 1, R.now, &o);
    wait_ms(10000, NULL);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
}

/* A key-down while the screen changes under it does nothing until its release. */
static void test_keydown_during_a_screen_change(void)
{
    rig(PID_MK2, box_default());
    wait_ms(3000, NULL); /* the home turned its page by itself */
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    int c, r;
    TEST_ASSERT_TRUE(find_key(DUI_ACT_PAGER, &c, &r));
    R.now += DUI_NAV_SETTLE_MS - 100;
    dui_nav_out_t o = key(c, r, true);
    TEST_ASSERT_EQUAL(DUI_SOUND_NONE, o.sound);
    TEST_ASSERT_EQUAL_UINT64(0, o.keys);
    TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_DOWN);
    R.now += 200; /* past the window, but still the same press */
    o = key(c, r, false);
    TEST_ASSERT_FALSE(o.all);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    wait_ms(5000, NULL); /* it counted as a press: no more paging by itself */
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    o = key(c, r, true); /* a new press works */
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    R.now += 80;
    key(c, r, false);
    TEST_ASSERT_EQUAL(2 % R.L.pages, dui_nav_pg(&R.n));
    /* the person's own change leaves the next press alone */
    tap_action(DUI_A_GO_BOX);
    o = key(0, 0, true);
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    R.now += 80;
    key(0, 0, false);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));

    /* the tile under a held key changes (the box changed): its release does nothing */
    go_box();
    TEST_ASSERT_TRUE(find_label("Sync now", &c, &r));
    key(c, r, true);
    R.b.box.slot = DUI_SLOT_SYNCING;
    dui_nav_box_changed(&R.n, &o);
    TEST_ASSERT_TRUE(o.rebuild);
    TEST_ASSERT_TRUE(o.all);
    apply(&o);
    TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_DOWN);
    R.now += 80;
    o = key(c, r, false);
    TEST_ASSERT_EQUAL(0, o.action);
    TEST_ASSERT_EQUAL(0, R.nact);
    /* the box changing elsewhere keeps the press */
    R.b.box.slot = DUI_SLOT_DIFFERS;
    dui_nav_box_changed(&R.n, &o);
    apply(&o);
    TEST_ASSERT_TRUE(find_label("Sync now", &c, &r));
    key(c, r, true);
    R.b.box.uptime_s += 60;
    dui_nav_box_changed(&R.n, &o);
    apply(&o);
    TEST_ASSERT_TRUE(decorated(c, r).flags & DUI_F_DOWN);
    R.now += 80;
    o = key(c, r, false);
    TEST_ASSERT_EQUAL(DUI_A_SYNC, o.action);

    /* on demand opening under a press */
    rig(PID_M32, box_default());
    go_box();
    TEST_ASSERT_TRUE(find_label("Dimmer", &c, &r));
    key(c, r, true);
    R.b.ondemand = true;
    dui_nav_ondemand(&R.n, true, R.now, &o);
    apply(&o);
    R.now += 80;
    o = key(c, r, false);
    TEST_ASSERT_EQUAL(0, o.action);
    R.now += 50; /* a fresh key-down in the window after it: ignored */
    o = key(1, 1, true);
    TEST_ASSERT_EQUAL(DUI_SOUND_NONE, o.sound);
    key(1, 1, false);
}

/* A dial turning the page under a held button: its release does nothing. */
static void test_dial_turn_under_a_press(void)
{
    rig(PID_PLUS, box_default());
    go_box();
    TEST_ASSERT_TRUE(R.L.pages >= 2);
    int c, r;
    TEST_ASSERT_TRUE(find_label("Dimmer", &c, &r));
    key(c, r, true);
    dui_nav_out_t o;
    dui_nav_dial_turn(&R.n, 1, R.now, &o);
    apply(&o);
    TEST_ASSERT_TRUE(o.all);
    TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_DOWN);
    R.now += 80;
    o = key(c, r, false);
    TEST_ASSERT_EQUAL(0, o.action);
    TEST_ASSERT_EQUAL(0, R.nact);
}

/* On demand closing while a key is down (its release goes to Companion): reopened, the deck works, and
 * idle closes it again; a hold held across the close never fires. */
static void test_ondemand_close_drops_the_press(void)
{
    dui_box_t b = box_default();
    strcpy(b.dock.peer, "192.168.8.155");
    b.connected = true;
    rig(PID_M32, b);
    R.b.ondemand = true;
    dui_nav_out_t o;
    dui_nav_ondemand(&R.n, true, R.now, &o);
    apply(&o);
    R.now += DUI_NAV_SETTLE_MS;
    int c, r;
    TEST_ASSERT_TRUE(find_key(DUI_A_GO_BOX, &c, &r));
    key(c, r, true); /* held ... */
    R.b.ondemand = false; /* ... when it closes (the service button, the RPC, the web page) */
    dui_nav_ondemand(&R.n, false, R.now, &o);
    apply(&o);
    R.now += 5000;
    R.b.ondemand = true; /* reopened */
    dui_nav_ondemand(&R.n, true, R.now, &o);
    apply(&o);
    R.now += DUI_NAV_SETTLE_MS;
    TEST_ASSERT_TRUE(find_key(DUI_A_GO_BOX, &c, &r));
    o = key(c, r, true);
    TEST_ASSERT_EQUAL(DUI_SOUND_CLICK, o.sound);
    R.now += 80;
    key(c, r, false);
    TEST_ASSERT_EQUAL(DUI_PAGE_BOX, dui_nav_page(&R.n));
    wait_ms(DUI_NAV_IDLE_MS, NULL);
    TEST_ASSERT_EQUAL(1, R.closes);
    TEST_ASSERT_TRUE(R.n.idle);
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));

    /* a hold across the close: nothing fires */
    R.now += DUI_NAV_SETTLE_MS;
    go_box();
    TEST_ASSERT_TRUE(find_label("Restart", &c, &r));
    key(c, r, true);
    wait_ms(500, NULL);
    R.b.ondemand = false;
    dui_nav_ondemand(&R.n, false, R.now, &o);
    apply(&o);
    wait_ms(DUI_HOLD_RESTART_MS * 2, NULL);
    TEST_ASSERT_EQUAL(0, R.nact);
    TEST_ASSERT_EQUAL(0, R.confirms);
    key(c, r, false);
    TEST_ASSERT_EQUAL(0, R.nact);
}

/* Companion takes a box-owned deck mid-hold and gives it back (dui_nav_cancel): the stale hold never
 * fires, presses work, idle counts. */
static void test_cancel_drops_a_stale_hold(void)
{
    rig(PID_M32, box_default());
    go_box();
    int c, r;
    TEST_ASSERT_TRUE(find_label("Restart", &c, &r));
    key(c, r, true);
    wait_ms(500, NULL);
    TEST_ASSERT_TRUE(decorated(c, r).flags & DUI_F_DOWN);
    dui_nav_cancel(&R.n); /* Companion takes it: the release goes there */
    wait_ms(1000, NULL);
    dui_nav_cancel(&R.n); /* and gives it back, no screen change in between */
    dui_nav_out_t acc = {0};
    wait_ms(DUI_HOLD_RESTART_MS * 2, &acc);
    TEST_ASSERT_EQUAL(0, R.nact);
    TEST_ASSERT_EQUAL(0, R.confirms);
    TEST_ASSERT_EQUAL_UINT64(0, acc.keys);
    dui_tile_t t = decorated(c, r);
    TEST_ASSERT_FALSE(t.flags & DUI_F_DOWN);
    TEST_ASSERT_EQUAL(0, t.hold);
    key(c, r, false); /* a late release, if it comes: nothing */
    TEST_ASSERT_EQUAL(0, R.nact);
    TEST_ASSERT_TRUE(find_label("Dimmer", &c, &r)); /* presses work */
    tap(c, r);
    TEST_ASSERT_EQUAL(1, R.nact);
    TEST_ASSERT_EQUAL(DUI_A_DIMMER, R.action[0]);
    wait_ms(DUI_NAV_IDLE_MS, NULL); /* idle counts */
    TEST_ASSERT_EQUAL(DUI_PAGE_HOME, dui_nav_page(&R.n));
    /* the hint goes too */
    R.now += DUI_NAV_SETTLE_MS;
    go_box();
    TEST_ASSERT_TRUE(find_label("Restart", &c, &r));
    tap(c, r);
    TEST_ASSERT_TRUE(decorated(c, r).flags & DUI_F_HINT);
    dui_nav_cancel(&R.n);
    TEST_ASSERT_FALSE(decorated(c, r).flags & DUI_F_HINT);
}

/* Only the release of the key seen going down is a press: one held when on demand opened, released
 * after, leaves the home paging by itself. */
static void test_untracked_release_is_not_a_press(void)
{
    rig(PID_MK2, box_default());
    TEST_ASSERT_TRUE(R.L.pages >= 2);
    R.now += 1000;
    dui_nav_out_t o = key(2, 1, false); /* never went down here */
    TEST_ASSERT_EQUAL(DUI_SOUND_NONE, o.sound);
    wait_ms(2000, NULL); /* 3 s after the start: it pages by itself */
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    key(2, 1, true); /* held before on demand opens */
    R.b.ondemand = true;
    dui_nav_ondemand(&R.n, true, R.now, &o);
    apply(&o);
    R.now += 500;
    key(2, 1, false); /* released after: not a press */
    TEST_ASSERT_FALSE(R.n.touched);
    wait_ms(2500, NULL);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
}

/* A page past the last after a rebuild (fewer pages) shows the first. */
static void test_pages_shrink(void)
{
    rig(PID_MK2, box_default());
    tap_action(DUI_ACT_PAGER);
    TEST_ASSERT_EQUAL(1, dui_nav_pg(&R.n));
    R.b.net.ap_on = false; /* the anchored hotspot goes: one page */
    dui_nav_out_t o;
    dui_nav_box_changed(&R.n, &o);
    apply(&o);
    TEST_ASSERT_EQUAL(1, R.L.pages);
    TEST_ASSERT_EQUAL(0, dui_nav_pg(&R.n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tap_fires_on_release_on_the_same_key);
    RUN_TEST(test_hold_fires_once_at_its_time);
    RUN_TEST(test_hold_released_early_shows_the_hint);
    RUN_TEST(test_mode_switch_hold_then_home);
    RUN_TEST(test_locked_button);
    RUN_TEST(test_pin_activity_keeps_awake);
    RUN_TEST(test_indicator_taps);
    RUN_TEST(test_back_on_a_menu_row_deck);
    RUN_TEST(test_back_on_a_menu_key_deck);
    RUN_TEST(test_choose_page_picks);
    RUN_TEST(test_pager_wraps);
    RUN_TEST(test_tap_to_page_on_the_mini);
    RUN_TEST(test_dials_and_touch_keys);
    RUN_TEST(test_idle_goes_home);
    RUN_TEST(test_idle_closes_on_demand);
    RUN_TEST(test_auto_advance);
    RUN_TEST(test_keydown_during_a_screen_change);
    RUN_TEST(test_dial_turn_under_a_press);
    RUN_TEST(test_mini_choose_pages_by_hand);
    RUN_TEST(test_ondemand_close_drops_the_press);
    RUN_TEST(test_cancel_drops_a_stale_hold);
    RUN_TEST(test_untracked_release_is_not_a_press);
    RUN_TEST(test_pages_shrink);
    return UNITY_END();
}
