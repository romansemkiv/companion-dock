/* The deck's PIN pad and its unlock (components/deckui/pin.c): the layout per model (dots, never
 * digits; every key there), entry, delete, OK, cancel, the idle cancel, wrong, locked out with the
 * countdown, no PIN set, right, the roller, the pages, and the unlock's end. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "deckmodel/model.h"
#include "deckui/deckui.h"
#include "deckui/nav.h"
#include "deckui/pin.h"

void setUp(void) {}
void tearDown(void) {}

enum { PID_M32 = 0x00BA, PID_XL = 0x006C, PID_PXL = 0x00C6, PID_MK2 = 0x0080, PID_ORIG = 0x0060, PID_PLUS = 0x0084,
       PID_NEO = 0x009A, PID_MINI = 0x0063, PID_M6 = 0x00B8 };

static dui_deck_t D;
static dui_pin_t P;
static dui_layout_t L;
static int64_t NOW;

static void deck(uint16_t pid) { TEST_ASSERT_TRUE(dui_deck_of(deck_model_find(DECK_VID_ELGATO, pid), &D)); }

static dui_pin_out_t open_pad(uint16_t pid, bool have_pin, int wait_s)
{
    deck(pid);
    memset(&P, 0, sizeof P);
    NOW = 10000;
    dui_pin_out_t o;
    dui_pin_open(&P, &D, 0x0042, have_pin, wait_s, NOW, &o);
    dui_pin_layout(&D, &P, NOW, &L);
    return o;
}

/* The key doing action on page pg; false: none. */
static bool find(uint16_t action, int pg, int *col, int *row)
{
    for (int r = 0; r < D.rows; r++)
        for (int c = 0; c < D.cols; c++) {
            dui_tile_t t;
            if (dui_layout_key(&L, pg, c, r, &t) && t.action == action) {
                *col = c;
                *row = r;
                return true;
            }
        }
    return false;
}

static void at(uint16_t action, int pg, int col, int row)
{
    int c = -1, r = -1;
    char msg[48];
    snprintf(msg, sizeof msg, "action 0x%x page %d", action, pg);
    TEST_ASSERT_TRUE_MESSAGE(find(action, pg, &c, &r), msg);
    TEST_ASSERT_EQUAL_MESSAGE(col, c, msg);
    TEST_ASSERT_EQUAL_MESSAGE(row, r, msg);
}

static dui_pin_out_t press(uint16_t action)
{
    int c, r;
    if (!find(action, P.pg, &c, &r)) {
        TEST_ASSERT_TRUE_MESSAGE(P.style == DUI_PIN_PAGES && find(action, P.pg ^ 1, &c, &r), "no key");
        dui_pin_out_t o;
        dui_pin_touch(&P, 1, NOW, &o);
        dui_pin_layout(&D, &P, NOW, &L);
    }
    dui_pin_out_t o;
    dui_pin_key(&P, &L, c, r, true, NOW, &o);
    NOW += 80;
    dui_pin_key(&P, &L, c, r, false, NOW, &o);
    dui_pin_layout(&D, &P, NOW, &L);
    NOW += 100;
    return o;
}

static void type(const char *digits)
{
    for (const char *k = digits; *k; k++) press((uint16_t)(DUI_PIN_A_DIGIT + (*k - '0')));
}

static dui_pin_out_t tick_to(int64_t t)
{
    dui_pin_out_t acc = {0};
    while (NOW < t) {
        NOW = NOW + 50 < t ? NOW + 50 : t;
        dui_pin_out_t o;
        dui_pin_tick(&P, NOW, &o);
        acc.redraw |= o.redraw;
        if (o.closed) acc.last_press = o.last_press;
        acc.closed |= o.closed;
        if (o.sound) acc.sound = o.sound;
    }
    dui_pin_layout(&D, &P, NOW, &L);
    return acc;
}

static const char *message(void)
{
    static char m[64];
    dui_pin_message(&P, NOW, m, sizeof m);
    return m;
}

/* ---- the layout per model ---- */

static void test_style_per_model(void)
{
    static const struct { uint16_t pid; int style; } C[] = {
        {PID_M32, DUI_PIN_PHONE}, {PID_XL, DUI_PIN_PHONE},  {PID_PXL, DUI_PIN_PHONE_BAR}, {PID_MK2, DUI_PIN_ROWS},
        {PID_ORIG, DUI_PIN_ROWS}, {PID_PLUS, DUI_PIN_PAGES}, {PID_NEO, DUI_PIN_PAGES},     {PID_MINI, DUI_PIN_ROLLER},
        {PID_M6, DUI_PIN_ROLLER},
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        deck(C[i].pid);
        TEST_ASSERT_EQUAL_MESSAGE(C[i].style, dui_pin_style(&D), deck_model_find(DECK_VID_ELGATO, C[i].pid)->name);
    }
}

static void test_layout_phone(void)
{
    for (int k = 0; k < 2; k++) {
        open_pad(k ? PID_XL : PID_M32, true, 0);
        TEST_ASSERT_EQUAL(1, L.pages);
        at(DUI_PIN_A_CANCEL, 0, 0, 0);
        for (int dgt = 1; dgt <= 9; dgt++) at((uint16_t)(DUI_PIN_A_DIGIT + dgt), 0, 2 + (dgt - 1) % 3, 1 + (dgt - 1) / 3);
        at(DUI_PIN_A_DEL, 0, 5, 1);
        at(DUI_PIN_A_DIGIT + 0, 0, 5, 2);
        at(DUI_PIN_A_OK, 0, 5, 3);
        dui_tile_t t;
        TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 3, 0, &t)); /* the dots in the status row, over 2 keys */
        TEST_ASSERT_EQUAL(DUI_TILE_DOTS, t.kind);
        TEST_ASSERT_EQUAL(0, t.cur);
        TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 4, 0, &t));
        TEST_ASSERT_EQUAL(DUI_TILE_DOTS, t.kind);
        TEST_ASSERT_EQUAL(4, t.cur);
    }
}

static void test_layout_plus_xl(void)
{
    open_pad(PID_PXL, true, 0);
    at(DUI_PIN_A_CANCEL, 0, 0, 0);
    for (int dgt = 1; dgt <= 9; dgt++) at((uint16_t)(DUI_PIN_A_DIGIT + dgt), 0, 3 + (dgt - 1) % 3, 1 + (dgt - 1) / 3);
    at(DUI_PIN_A_DEL, 0, 6, 1);
    at(DUI_PIN_A_DIGIT + 0, 0, 6, 2);
    at(DUI_PIN_A_OK, 0, 6, 3);
    for (int r = 0; r < D.rows; r++) /* the dots are on the strip */
        for (int c = 0; c < D.cols; c++) {
            dui_tile_t t;
            dui_layout_key(&L, 0, c, r, &t);
            TEST_ASSERT_NOT_EQUAL(DUI_TILE_DOTS, t.kind);
        }
}

static void test_layout_rows(void)
{
    open_pad(PID_MK2, true, 0);
    at(DUI_PIN_A_CANCEL, 0, 0, 0);
    at(DUI_PIN_A_DEL, 0, 1, 0);
    at(DUI_PIN_A_OK, 0, 4, 0);
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 2, 0, &t));
    TEST_ASSERT_EQUAL(DUI_TILE_DOTS, t.kind);
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 3, 0, &t));
    TEST_ASSERT_EQUAL(DUI_TILE_DOTS, t.kind);
    for (int dgt = 1; dgt <= 5; dgt++) at((uint16_t)(DUI_PIN_A_DIGIT + dgt), 0, dgt - 1, 1);
    for (int dgt = 6; dgt <= 9; dgt++) at((uint16_t)(DUI_PIN_A_DIGIT + dgt), 0, dgt - 6, 2);
    at(DUI_PIN_A_DIGIT + 0, 0, 4, 2);
}

static void test_layout_pages(void)
{
    for (int k = 0; k < 2; k++) {
        open_pad(k ? PID_NEO : PID_PLUS, true, 0);
        TEST_ASSERT_EQUAL(2, L.pages);
        for (int dgt = 1; dgt <= 4; dgt++) at((uint16_t)(DUI_PIN_A_DIGIT + dgt), 0, dgt - 1, 0);
        at(DUI_PIN_A_DIGIT + 5, 0, 0, 1);
        for (int dgt = 6; dgt <= 9; dgt++) at((uint16_t)(DUI_PIN_A_DIGIT + dgt), 1, dgt - 6, 0);
        at(DUI_PIN_A_DIGIT + 0, 1, 0, 1);
        for (int pg = 0; pg < 2; pg++) {
            at(DUI_PIN_A_DEL, pg, 1, 1);
            at(DUI_PIN_A_OK, pg, 2, 1);
            at(DUI_PIN_A_PAGE, pg, 3, 1);
        }
        int c, r;
        TEST_ASSERT_FALSE(find(DUI_PIN_A_CANCEL, 0, &c, &r)); /* ⌫ held, a dial pushed, or idle */
    }
}

static void test_layout_roller(void)
{
    for (int k = 0; k < 2; k++) {
        open_pad(k ? PID_M6 : PID_MINI, true, 0);
        at(DUI_PIN_A_DOWN, 0, 0, 0);
        at(DUI_PIN_A_UP, 0, 2, 0);
        at(DUI_PIN_A_DEL, 0, 0, 1);
        at(DUI_PIN_A_ADD, 0, 1, 1);
        at(DUI_PIN_A_OK, 0, 2, 1);
        dui_tile_t t;
        TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 1, 0, &t)); /* the digit to add, the dots under it */
        TEST_ASSERT_EQUAL(DUI_TILE_KEY, t.kind);
        TEST_ASSERT_TRUE(t.flags & DUI_F_DOTS);
        TEST_ASSERT_EQUAL_STRING("0", t.text);
    }
}

/* The digits typed show as dots only: on the keys, the strip or info bar words, the roller's key. */
static void test_dots_never_digits(void)
{
    static const uint16_t PIDS[] = {PID_M32, PID_PXL, PID_MK2, PID_PLUS, PID_NEO, PID_MINI};
    for (size_t m = 0; m < sizeof PIDS / sizeof PIDS[0]; m++) {
        open_pad(PIDS[m], true, 0);
        if (P.style == DUI_PIN_ROLLER) {
            for (int k = 0; k < 4; k++) { /* 4, 7, 1, 1: the roller up to each, then Add */
                const int want = "4711"[k] - '0';
                while (P.roller != want) press(DUI_PIN_A_UP);
                press(DUI_PIN_A_ADD);
            }
            while (P.roller != 0) press(DUI_PIN_A_UP); /* the shown digit back to 0 */
        } else {
            type("4711");
        }
        TEST_ASSERT_EQUAL(4, P.n);
        int dots = 0;
        for (int pg = 0; pg < L.pages; pg++)
            for (int r = 0; r < D.rows; r++)
                for (int c = 0; c < D.cols; c++) {
                    dui_tile_t t;
                    if (!dui_layout_key(&L, pg, c, r, &t)) continue;
                    if (t.kind == DUI_TILE_DOTS || (t.flags & DUI_F_DOTS)) {
                        TEST_ASSERT_EQUAL(4, t.num);
                        dots++;
                    }
                    const bool label = t.kind == DUI_TILE_KEY && t.action >= DUI_PIN_A_DIGIT && t.action <= DUI_PIN_A_DIGIT + 9;
                    if (t.text && !label && !(t.flags & DUI_F_DOTS)) TEST_ASSERT_NULL_MESSAGE(strpbrk(t.text, "0123456789"), t.text);
                    if (t.flags & DUI_F_DOTS) TEST_ASSERT_EQUAL_STRING("0", t.text); /* the roller's digit, not what was typed */
                    if (t.caption) TEST_ASSERT_NULL(strpbrk(t.caption, "0123456789"));
                }
        if (D.bar == DUI_BAR_NONE) TEST_ASSERT_TRUE(dots >= 1);
        TEST_ASSERT_EQUAL_STRING("Enter PIN", message());
    }
}

/* ---- entry ---- */

static void test_entry_delete_ok(void)
{
    open_pad(PID_M32, true, 0);
    type("12");
    TEST_ASSERT_EQUAL(2, P.n);
    press(DUI_PIN_A_DEL);
    TEST_ASSERT_EQUAL(1, P.n);
    type("23");
    dui_pin_out_t o = press(DUI_PIN_A_OK); /* 3 digits: too short */
    TEST_ASSERT_FALSE(o.check);
    TEST_ASSERT_EQUAL(DUI_SOUND_LOCKED, o.sound);
    TEST_ASSERT_EQUAL(DUI_PIN_ENTRY, P.phase);
    type("456789"); /* past 8: ignored */
    TEST_ASSERT_EQUAL(DUI_PIN_MAX, P.n);
    for (int i = 0; i < 4; i++) press(DUI_PIN_A_DEL);
    TEST_ASSERT_EQUAL(4, P.n);
    o = press(DUI_PIN_A_OK);
    TEST_ASSERT_TRUE(o.check);
    TEST_ASSERT_EQUAL(DUI_PIN_CHECKING, P.phase);
    TEST_ASSERT_EQUAL_STRING("Checking", message());
    char pin[DUI_PIN_MAX + 1];
    dui_pin_take(&P, pin);
    TEST_ASSERT_EQUAL_STRING("1234", pin);
    TEST_ASSERT_EQUAL(0, P.n);
    for (size_t i = 0; i < sizeof P.d; i++) TEST_ASSERT_EQUAL(0, P.d[i]); /* wiped */
    /* while it checks the keys do nothing */
    type("9");
    TEST_ASSERT_EQUAL(0, P.n);
}

static void test_cancel_and_idle(void)
{
    open_pad(PID_M32, true, 0);
    type("12");
    dui_pin_out_t o = press(DUI_PIN_A_CANCEL);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(o.unlocked);
    TEST_ASSERT_FALSE(P.open);
    for (size_t i = 0; i < sizeof P.d; i++) TEST_ASSERT_EQUAL(0, P.d[i]);
    /* 30 s without a press */
    open_pad(PID_MK2, true, 0);
    type("5");
    o = tick_to(NOW + DUI_PIN_IDLE_MS - 300); /* the last press: 100 ms ago */
    TEST_ASSERT_FALSE(o.closed);
    o = tick_to(NOW + 400);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(P.open);
    /* never while a check runs */
    open_pad(PID_MK2, true, 0);
    type("1234");
    press(DUI_PIN_A_OK);
    o = tick_to(NOW + DUI_PIN_IDLE_MS + 5000);
    TEST_ASSERT_FALSE(o.closed);
    TEST_ASSERT_TRUE(P.open);
    /* ⌫ held cancels where there is no ✕ (PAGES, ROLLER) */
    static const uint16_t SMALL[] = {PID_PLUS, PID_NEO, PID_MINI};
    for (size_t k = 0; k < 3; k++) {
        open_pad(SMALL[k], true, 0);
        int c, r;
        TEST_ASSERT_TRUE(find(DUI_PIN_A_DEL, P.pg, &c, &r));
        dui_pin_key(&P, &L, c, r, true, NOW, &o);
        o = tick_to(NOW + DUI_PIN_CANCEL_HOLD_MS + 50);
        TEST_ASSERT_TRUE(o.closed);
        dui_pin_key(&P, &L, c, r, false, NOW, &o); /* its release does nothing more */
        TEST_ASSERT_FALSE(o.closed);
    }
    /* a dial pushed cancels (+); the push is a press: the screen's idle clock runs from it, never from earlier */
    open_pad(PID_PLUS, true, 0);
    dui_pin_dial_press(&P, NOW + 20000, &o);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_EQUAL(NOW + 20000, o.last_press);
}

static void test_wrong_locked_nopin(void)
{
    open_pad(PID_M32, true, 0);
    type("1111");
    press(DUI_PIN_A_OK);
    char pin[DUI_PIN_MAX + 1];
    dui_pin_take(&P, pin);
    dui_pin_out_t o;
    dui_pin_result(&P, DUI_PIN_R_WRONG, 0, NOW, &o);
    TEST_ASSERT_EQUAL(DUI_SOUND_LOCKED, o.sound);
    TEST_ASSERT_TRUE(o.redraw);
    TEST_ASSERT_FALSE(o.closed);
    TEST_ASSERT_EQUAL(DUI_PIN_WRONG, P.phase);
    TEST_ASSERT_EQUAL_STRING("Wrong PIN", message());
    dui_layout_t lw;
    dui_pin_layout(&D, &P, NOW, &lw);
    dui_tile_t t;
    dui_layout_key(&lw, 0, 1, 0, &t); /* the status row red */
    TEST_ASSERT_EQUAL(DUI_TINT_BAD, t.tint);
    tick_to(NOW + DUI_PIN_WRONG_MS + 50);
    TEST_ASSERT_EQUAL(DUI_PIN_ENTRY, P.phase);
    TEST_ASSERT_EQUAL(0, P.n);
    /* locked out: the countdown */
    type("2222");
    press(DUI_PIN_A_OK);
    dui_pin_take(&P, pin);
    dui_pin_result(&P, DUI_PIN_R_LOCKED, 37, NOW, &o);
    TEST_ASSERT_EQUAL(DUI_SOUND_LOCKED, o.sound);
    TEST_ASSERT_EQUAL(DUI_PIN_LOCKED, P.phase);
    TEST_ASSERT_EQUAL_STRING("Locked \xC2\xB7 wait 37 s", message());
    o = tick_to(NOW + 1000);
    TEST_ASSERT_TRUE(o.redraw);
    TEST_ASSERT_EQUAL_STRING("Locked \xC2\xB7 wait 36 s", message());
    o = press(DUI_PIN_A_DIGIT + 5); /* the keys do nothing meanwhile */
    TEST_ASSERT_EQUAL(DUI_SOUND_LOCKED, o.sound);
    TEST_ASSERT_EQUAL(0, P.n);
    tick_to(NOW + 37000);
    TEST_ASSERT_EQUAL(DUI_PIN_ENTRY, P.phase);
    TEST_ASSERT_EQUAL_STRING("Enter PIN", message());
    /* opened while locked out: the countdown at once */
    o = open_pad(PID_MK2, true, 90);
    TEST_ASSERT_EQUAL(DUI_PIN_LOCKED, P.phase);
    TEST_ASSERT_EQUAL_STRING("Locked \xC2\xB7 wait 90 s", message());
    /* no PIN set (none, or a text password of before): said so; any key closes it */
    open_pad(PID_M32, false, 0);
    TEST_ASSERT_EQUAL(DUI_PIN_NOPIN, P.phase);
    TEST_ASSERT_EQUAL_STRING("Set a PIN on the web page", message());
    o = press(DUI_PIN_A_DIGIT + 3);
    TEST_ASSERT_TRUE(o.closed);
}

static void test_right_pin_closes(void)
{
    open_pad(PID_NEO, true, 0);
    type("4711");
    dui_pin_out_t o = press(DUI_PIN_A_OK);
    TEST_ASSERT_TRUE(o.check);
    char pin[DUI_PIN_MAX + 1];
    dui_pin_take(&P, pin);
    TEST_ASSERT_EQUAL_STRING("4711", pin);
    dui_pin_result(&P, DUI_PIN_R_OK, 0, NOW, &o);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_TRUE(o.unlocked);
    TEST_ASSERT_EQUAL(DUI_SOUND_CONFIRM, o.sound);
    TEST_ASSERT_FALSE(P.open);
    TEST_ASSERT_EQUAL(0x0042, P.from_action); /* the key that opened it: not fired, the caller knows */
    /* a stale answer after a cancel does nothing */
    dui_pin_result(&P, DUI_PIN_R_OK, 0, NOW, &o);
    TEST_ASSERT_FALSE(o.closed || o.unlocked);
}

/* A check the worker can't take now: "Busy, try again" (amber), the digits kept for
 * another OK, no locked sound. */
static void test_busy_keeps_digits(void)
{
    open_pad(PID_M32, true, 0);
    type("4711");
    dui_pin_out_t o = press(DUI_PIN_A_OK);
    TEST_ASSERT_TRUE(o.check);
    char pin[DUI_PIN_MAX + 1];
    dui_pin_take(&P, pin);
    dui_pin_busy(&P, pin, NOW, &o);
    TEST_ASSERT_EQUAL(DUI_PIN_BUSY, P.phase);
    TEST_ASSERT_EQUAL_STRING("Busy, try again", message());
    TEST_ASSERT_EQUAL(DUI_SOUND_NONE, o.sound);
    TEST_ASSERT_TRUE(o.redraw);
    TEST_ASSERT_EQUAL(4, P.n);
    dui_layout_t lb;
    dui_pin_layout(&D, &P, NOW, &lb);
    dui_tile_t t;
    dui_layout_key(&lb, 0, 1, 0, &t);
    TEST_ASSERT_EQUAL(DUI_TINT_WAIT, t.tint);
    tick_to(NOW + DUI_PIN_WRONG_MS + 50);
    TEST_ASSERT_EQUAL(DUI_PIN_ENTRY, P.phase);
    TEST_ASSERT_EQUAL(4, P.n);
    o = press(DUI_PIN_A_OK);
    TEST_ASSERT_TRUE(o.check);
    dui_pin_take(&P, pin);
    TEST_ASSERT_EQUAL_STRING("4711", pin);
}

/* A check that gets no answer in DUI_PIN_CHECK_MS ends as busy (the digits gone with it), so the pad
 * and the on-demand idle close aren't held up; a late answer then does nothing. */
static void test_checking_times_out(void)
{
    TEST_ASSERT_EQUAL(15000, DUI_PIN_CHECK_MS);
    open_pad(PID_MK2, true, 0);
    type("1234");
    press(DUI_PIN_A_OK);
    char pin[DUI_PIN_MAX + 1];
    dui_pin_take(&P, pin);
    dui_pin_out_t o = tick_to(NOW + DUI_PIN_CHECK_MS - 300); /* OK was let go 100 ms ago */
    TEST_ASSERT_EQUAL(DUI_PIN_CHECKING, P.phase);
    o = tick_to(NOW + 400);
    TEST_ASSERT_TRUE(o.redraw);
    TEST_ASSERT_EQUAL(DUI_PIN_BUSY, P.phase);
    dui_pin_result(&P, DUI_PIN_R_OK, 0, NOW, &o); /* too late */
    TEST_ASSERT_FALSE(o.unlocked);
    TEST_ASSERT_TRUE(P.open);
    o = tick_to(NOW + DUI_PIN_IDLE_MS + 100); /* the idle cancel runs again */
    TEST_ASSERT_TRUE(o.closed);
}

/* A key held across a change of hands (the caller drops it) doesn't jam the pad; the idle close tells
 * the caller when the last press was, so the screen under it doesn't get an extra 30 s. */
static void test_cancel_held_and_idle_last_press(void)
{
    open_pad(PID_M32, true, 0);
    int c, r;
    TEST_ASSERT_TRUE(find(DUI_PIN_A_DIGIT + 5, 0, &c, &r));
    dui_pin_out_t o;
    dui_pin_key(&P, &L, c, r, true, NOW, &o);
    dui_pin_cancel(&P); /* its release will never come */
    NOW += 100;
    press(DUI_PIN_A_DIGIT + 7);
    TEST_ASSERT_EQUAL(1, P.n);
    const int64_t last = NOW - 100; /* press() leaves NOW 100 ms past the release */
    o = tick_to(NOW + DUI_PIN_IDLE_MS + 100);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_TRUE(o.last_press >= last - 100 && o.last_press <= last + 100);
}

static void test_roller(void)
{
    open_pad(PID_MINI, true, 0);
    press(DUI_PIN_A_DOWN);
    TEST_ASSERT_EQUAL(9, P.roller);
    press(DUI_PIN_A_UP);
    press(DUI_PIN_A_UP);
    TEST_ASSERT_EQUAL(1, P.roller);
    press(DUI_PIN_A_ADD);
    press(DUI_PIN_A_ADD);
    TEST_ASSERT_EQUAL(2, P.n);
    dui_tile_t t;
    dui_layout_key(&L, 0, 1, 0, &t);
    TEST_ASSERT_EQUAL_STRING("1", t.text);
    TEST_ASSERT_EQUAL(2, t.num);
    press(DUI_PIN_A_DEL);
    TEST_ASSERT_EQUAL(1, P.n);
    /* a wrong PIN shows on the digit's key: the Mini has no status row */
    press(DUI_PIN_A_ADD);
    press(DUI_PIN_A_ADD);
    press(DUI_PIN_A_ADD);
    dui_pin_out_t o = press(DUI_PIN_A_OK);
    TEST_ASSERT_TRUE(o.check);
    TEST_ASSERT_EQUAL(0, P.roller); /* the last digit doesn't stay on show */
    char pin[DUI_PIN_MAX + 1];
    dui_pin_take(&P, pin);
    TEST_ASSERT_EQUAL_STRING("1111", pin);
    dui_pin_result(&P, DUI_PIN_R_WRONG, 0, NOW, &o);
    dui_pin_layout(&D, &P, NOW, &L);
    dui_layout_key(&L, 0, 1, 0, &t);
    TEST_ASSERT_EQUAL(DUI_TINT_BAD, t.tint);
    /* locked out: the specified words on the one key (2 lines) */
    open_pad(PID_MINI, true, 37);
    dui_layout_key(&L, 0, 1, 0, &t);
    TEST_ASSERT_EQUAL_STRING("Locked \xC2\xB7 wait 37 s", t.text);
}

static void test_pages_turn(void)
{
    open_pad(PID_PLUS, true, 0);
    TEST_ASSERT_EQUAL(0, P.pg);
    press(DUI_PIN_A_PAGE);
    TEST_ASSERT_EQUAL(1, P.pg);
    dui_pin_out_t o;
    dui_pin_dial_turn(&P, 1, NOW, &o);
    TEST_ASSERT_EQUAL(0, P.pg);
    TEST_ASSERT_TRUE(o.redraw);
    dui_pin_touch(&P, -1, NOW, &o);
    TEST_ASSERT_EQUAL(1, P.pg);
    type("06"); /* 0 on page 2, 6 on page 2 */
    TEST_ASSERT_EQUAL(2, P.n);
}

/* ---- the unlock ---- */

static void test_unlock_expiry(void)
{
    dui_unlock_t u = {0};
    TEST_ASSERT_FALSE(dui_unlock_step(&u, 0, true, true)); /* not on */
    dui_unlock_start(&u, 1000);
    TEST_ASSERT_TRUE(u.on);
    TEST_ASSERT_FALSE(dui_unlock_step(&u, 1000 + 4 * 60 * 1000, false, false));
    TEST_ASSERT_TRUE(dui_unlock_step(&u, 2000, true, false)); /* the maintenance screen closed */
    TEST_ASSERT_FALSE(u.on);
    dui_unlock_start(&u, 5000);
    TEST_ASSERT_TRUE(dui_unlock_step(&u, 6000, false, true)); /* the 60 s idle return home */
    dui_unlock_start(&u, 5000);
    TEST_ASSERT_FALSE(dui_unlock_step(&u, 5000 + DUI_UNLOCK_MS - 1, false, false));
    TEST_ASSERT_TRUE(dui_unlock_step(&u, 5000 + DUI_UNLOCK_MS, false, false)); /* 5 min at most */
    TEST_ASSERT_FALSE(u.on);
}

/* Each deck has its own pad and unlock: unlocking one leaves the other locked. */
static void test_unlock_per_deck(void)
{
    dui_unlock_t a = {0}, b = {0};
    dui_pin_t pa, pb;
    dui_pin_out_t o;
    deck(PID_M32);
    memset(&pa, 0, sizeof pa);
    memset(&pb, 0, sizeof pb);
    dui_pin_open(&pa, &D, 1, true, 0, 0, &o);
    dui_pin_open(&pb, &D, 1, true, 0, 0, &o);
    pa.phase = DUI_PIN_CHECKING; /* a PIN went in on deck a, OK pressed */
    dui_pin_result(&pa, DUI_PIN_R_OK, 0, 10, &o);
    if (o.unlocked) dui_unlock_start(&a, 10);
    TEST_ASSERT_TRUE(a.on);
    TEST_ASSERT_FALSE(b.on);
    TEST_ASSERT_TRUE(pb.open); /* the other deck's pad is still open, untouched */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_style_per_model);
    RUN_TEST(test_layout_phone);
    RUN_TEST(test_layout_plus_xl);
    RUN_TEST(test_layout_rows);
    RUN_TEST(test_layout_pages);
    RUN_TEST(test_layout_roller);
    RUN_TEST(test_dots_never_digits);
    RUN_TEST(test_entry_delete_ok);
    RUN_TEST(test_cancel_and_idle);
    RUN_TEST(test_wrong_locked_nopin);
    RUN_TEST(test_right_pin_closes);
    RUN_TEST(test_roller);
    RUN_TEST(test_busy_keeps_digits);
    RUN_TEST(test_checking_times_out);
    RUN_TEST(test_cancel_held_and_idle_last_press);
    RUN_TEST(test_pages_turn);
    RUN_TEST(test_unlock_expiry);
    RUN_TEST(test_unlock_per_deck);
    return UNITY_END();
}
