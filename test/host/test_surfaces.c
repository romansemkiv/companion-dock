#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "deckmodel/model.h"
#include "surfaces/surfaces.h"

void setUp(void) {}
void tearDown(void) {}

static const deck_model_t *M(uint16_t pid)
{
    const deck_model_t *m = deck_model_find(0x0FD9, pid);
    TEST_ASSERT_NOT_NULL(m);
    return m;
}

#define PID_ORIG_V1 0x0060
#define PID_XL 0x006C
#define PID_NEO 0x009A
#define PID_PLUS 0x0084
#define PID_PLUS_XL 0x00C6
#define PID_PEDAL 0x0086

static int deck1, deck2, decks[8];

/* ---- registry ---- */

void test_registry_attach_detach(void)
{
    static surf_registry_t r;
    memset(&r, 0, sizeof r);
    int s = surfreg_attach(&r, "streamdeck:A", &deck1, M(PID_XL));
    TEST_ASSERT_TRUE(s >= 0);
    TEST_ASSERT_EQUAL(s, surfreg_find(&r, "streamdeck:A"));
    TEST_ASSERT_EQUAL_PTR(&deck1, r.slot[s].deck);
    TEST_ASSERT_EQUAL(32, r.slot[s].nctl);
    TEST_ASSERT_FALSE(r.slot[s].added);

    TEST_ASSERT_EQUAL(s, surfreg_detach(&r, &deck1));
    TEST_ASSERT_EQUAL(-1, surfreg_find(&r, "streamdeck:A"));
    TEST_ASSERT_EQUAL(-1, surfreg_detach(&r, &deck1));
}

void test_registry_full_returns_minus1(void)
{
    static surf_registry_t r;
    memset(&r, 0, sizeof r);
    char id[48];
    for (int i = 0; i < SURF_MAX_DECKS; i++) {
        snprintf(id, sizeof id, "streamdeck:S%d", i);
        TEST_ASSERT_TRUE(surfreg_attach(&r, id, &decks[i], M(PID_XL)) >= 0);
    }
    TEST_ASSERT_EQUAL(-1, surfreg_attach(&r, "streamdeck:S6", &decks[6], M(PID_XL)));
    /* a freed slot can be used again */
    int s = surfreg_detach(&r, &decks[2]);
    TEST_ASSERT_TRUE(s >= 0);
    TEST_ASSERT_EQUAL(s, surfreg_attach(&r, "streamdeck:S6", &decks[6], M(PID_XL)));
}

/* the deck limit caps the registry: at 2 the 3rd deck (the Pedal counts) is refused, at 4 the 3rd and 4th are
 * taken and the 5th refused; a replugged deck keeps its slot whatever the cap; a freed slot takes the next deck. */
void test_registry_deck_limit(void)
{
    static surf_registry_t r;
    char id[48];
    for (int cap = 2; cap <= 4; cap += 2) {
        memset(&r, 0, sizeof r);
        r.cap = (uint8_t)cap;
        for (int i = 0; i < cap; i++) {
            snprintf(id, sizeof id, "streamdeck:L%d", i);
            const int slot = surfreg_attach(&r, id, &decks[i], M(i == 1 ? PID_PEDAL : PID_XL));
            TEST_ASSERT_TRUE(slot >= 0 && slot < cap);
        }
        TEST_ASSERT_EQUAL(-1, surfreg_attach(&r, "streamdeck:OVER", &decks[cap], M(PID_XL))); /* ignored */
        TEST_ASSERT_EQUAL(-1, surfreg_find(&r, "streamdeck:OVER"));
        TEST_ASSERT_EQUAL(0, surfreg_attach(&r, "streamdeck:L0", &decks[7], M(PID_XL))); /* replugged: its slot */
        const int s = surfreg_detach(&r, &decks[7]);
        TEST_ASSERT_EQUAL(0, s);
        TEST_ASSERT_EQUAL(0, surfreg_attach(&r, "streamdeck:OVER", &decks[cap], M(PID_XL))); /* room again */
    }
    memset(&r, 0, sizeof r); /* no cap set (0): the registry's capacity, SURF_MAX_DECKS */
    for (int i = 0; i < SURF_MAX_DECKS; i++) {
        snprintf(id, sizeof id, "streamdeck:N%d", i);
        TEST_ASSERT_TRUE(surfreg_attach(&r, id, &decks[i], M(PID_XL)) >= 0);
    }
}

void test_registry_same_serial_reuses_slot(void)
{
    static surf_registry_t r;
    memset(&r, 0, sizeof r);
    int a = surfreg_attach(&r, "streamdeck:A", &deck1, M(PID_XL));
    r.slot[a].added = true;
    int b = surfreg_attach(&r, "streamdeck:A", &deck2, M(PID_XL));
    TEST_ASSERT_EQUAL(a, b);
    TEST_ASSERT_EQUAL_PTR(&deck2, r.slot[a].deck);
    TEST_ASSERT_TRUE(r.slot[a].added); /* one surface, no second ADD-DEVICE */
    int pending[SURF_MAX_DECKS];
    TEST_ASSERT_EQUAL(0, surf_pending_adds(&r, pending, SURF_MAX_DECKS));
    /* the stale handle's detach finds nothing */
    TEST_ASSERT_EQUAL(-1, surfreg_detach(&r, &deck1));
    TEST_ASSERT_EQUAL(a, surfreg_find(&r, "streamdeck:A"));
}

void test_ready_readds_all_attached(void)
{
    static surf_registry_t r;
    memset(&r, 0, sizeof r);
    int s[3];
    s[0] = surfreg_attach(&r, "streamdeck:A", &decks[0], M(PID_XL));
    s[1] = surfreg_attach(&r, "streamdeck:B", &decks[1], M(PID_PLUS));
    s[2] = surfreg_attach(&r, "streamdeck:C", &decks[2], M(PID_PEDAL));
    for (int i = 0; i < 3; i++) r.slot[s[i]].added = true;
    int pending[SURF_MAX_DECKS];
    TEST_ASSERT_EQUAL(0, surf_pending_adds(&r, pending, SURF_MAX_DECKS));

    surfreg_mark_all_unadded(&r);
    int n = surf_pending_adds(&r, pending, SURF_MAX_DECKS);
    TEST_ASSERT_EQUAL(3, n);
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL(s[i], pending[i]);
    TEST_ASSERT_EQUAL(2, surf_pending_adds(&r, pending, 2));
}

/* ---- KEY-STATE routing ---- */

static surf_registry_t *xl_registry(void)
{
    static surf_registry_t r;
    memset(&r, 0, sizeof r);
    TEST_ASSERT_TRUE(surfreg_attach(&r, "streamdeck:XL1", &deck1, M(PID_XL)) >= 0);
    return &r;
}

void test_keystate_unknown_device_dropped(void)
{
    surf_registry_t *r = xl_registry();
    sat_key_state_t ks = {.device_id = "streamdeck:GONE", .control_id = "0/0", .bitmap_b64 = "AAAA",
                          .bitmap_b64_len = 4};
    int slot = 99;
    const deck_ctl_t *ctl = NULL;
    size_t exp = 99;
    TEST_ASSERT_EQUAL(-1, surf_route_keystate(r, &ks, &slot, &ctl, &exp));

    /* unplugged: the same device id after detach */
    surfreg_detach(r, &deck1);
    ks.device_id = "streamdeck:XL1";
    TEST_ASSERT_EQUAL(-1, surf_route_keystate(r, &ks, &slot, &ctl, &exp));
    ks.device_id = NULL;
    TEST_ASSERT_EQUAL(-1, surf_route_keystate(r, &ks, &slot, &ctl, &exp));
}

void test_keystate_unknown_control_dropped(void)
{
    surf_registry_t *r = xl_registry();
    sat_key_state_t ks = {.device_id = "streamdeck:XL1", .control_id = "9/9"};
    int slot;
    const deck_ctl_t *ctl;
    size_t exp;
    TEST_ASSERT_EQUAL(-1, surf_route_keystate(r, &ks, &slot, &ctl, &exp));
    ks.control_id = "4/0"; /* one row past the XL's 4 rows */
    TEST_ASSERT_EQUAL(-1, surf_route_keystate(r, &ks, &slot, &ctl, &exp));
    ks.control_id = NULL;
    TEST_ASSERT_EQUAL(-1, surf_route_keystate(r, &ks, &slot, &ctl, &exp));
    ks.control_id = "3/7";
    TEST_ASSERT_EQUAL(0, surf_route_keystate(r, &ks, &slot, &ctl, &exp));
    TEST_ASSERT_EQUAL(31, ctl->hw);
}

void test_bitmap_wrong_size_dropped(void)
{
    surf_registry_t *r = xl_registry();
    sat_key_state_t ks = {.device_id = "streamdeck:XL1", .control_id = "1/2"};
    int slot = -1;
    const deck_ctl_t *ctl = NULL;
    size_t exp = 0;
    TEST_ASSERT_EQUAL(0, surf_route_keystate(r, &ks, &slot, &ctl, &exp));
    TEST_ASSERT_EQUAL(surfreg_find(r, "streamdeck:XL1"), slot);
    TEST_ASSERT_EQUAL(CTL_KEY_LCD, ctl->kind);
    TEST_ASSERT_EQUAL(27648, exp);
    TEST_ASSERT_FALSE(surf_bitmap_ok(exp, 100));
    TEST_ASSERT_FALSE(surf_bitmap_ok(exp, 27647));
    TEST_ASSERT_FALSE(surf_bitmap_ok(exp, 27649));
    TEST_ASSERT_FALSE(surf_bitmap_ok(0, 0));
    TEST_ASSERT_TRUE(surf_bitmap_ok(exp, 27648));
}

/* KEY-STATE's BITMAP before it is queued: raw RGB in base64 of exactly the key's size, or a PNG data URL (the size
 * is checked when it is decoded) of at most 1.5 x the raw size + 4 KB; webp, or anything else, is dropped */
void test_bitmap_text_size(void)
{
    const size_t exp = 27648; /* an XL key, 96 x 96 RGB */
    static char v[60000];
    memset(v, 'A', sizeof v);
    TEST_ASSERT_TRUE(surf_bitmap_text_ok(v, 36864, exp));
    TEST_ASSERT_FALSE(surf_bitmap_text_ok(v, 36860, exp));
    TEST_ASSERT_FALSE(surf_bitmap_text_ok(v, 36868, exp));
    TEST_ASSERT_FALSE(surf_bitmap_text_ok(v, 0, exp));
    TEST_ASSERT_FALSE(surf_bitmap_text_ok(NULL, 36864, exp));
    const char *png = "data:image/png;base64,";
    memcpy(v, png, strlen(png));
    TEST_ASSERT_TRUE(surf_bitmap_text_ok(v, strlen(png) + 600, exp));
    TEST_ASSERT_TRUE(surf_bitmap_text_ok(v, strlen(png) + 4, exp));
    TEST_ASSERT_FALSE(surf_bitmap_text_ok(v, strlen(png), exp)); /* empty */
    const size_t cap = (exp + exp / 2 + 4096 + 2) / 3 * 4; /* 1.5 x raw + 4 KB, in base64 */
    TEST_ASSERT_TRUE(surf_bitmap_text_ok(v, strlen(png) + cap, exp));
    TEST_ASSERT_FALSE(surf_bitmap_text_ok(v, strlen(png) + cap + 4, exp));
    TEST_ASSERT_FALSE(surf_bitmap_text_ok("data:image/webp;base64,UklGRg==", 31, exp));
    TEST_ASSERT_FALSE(surf_bitmap_text_ok("data:image/png;base64,AAAA", 26, 0));
}

/* A deck Companion let go of (dock mode), or Companion lost (Satellite): its last images stay 2 s (a quick
 * reconnect shows nothing), then dimmed with "Reconnecting" until 30 s, then the box's screens */
void test_loss_phase(void)
{
    const int64_t s = 1000000;
    TEST_ASSERT_EQUAL(SURF_LOSS_NONE, surf_loss_phase(0, false, 50 * s));
    TEST_ASSERT_EQUAL(SURF_LOSS_GRACE, surf_loss_phase(10 * s, false, 10 * s));
    TEST_ASSERT_EQUAL(SURF_LOSS_GRACE, surf_loss_phase(10 * s, false, 12 * s - 1));
    TEST_ASSERT_EQUAL(SURF_LOSS_DIM_DUE, surf_loss_phase(10 * s, false, 12 * s));
    TEST_ASSERT_EQUAL(SURF_LOSS_DIMMED, surf_loss_phase(10 * s, true, 12 * s));
    TEST_ASSERT_EQUAL(SURF_LOSS_DIMMED, surf_loss_phase(10 * s, true, 40 * s - 1));
    TEST_ASSERT_EQUAL(SURF_LOSS_OVER, surf_loss_phase(10 * s, true, 40 * s));
    TEST_ASSERT_EQUAL(SURF_LOSS_OVER, surf_loss_phase(10 * s, false, 40 * s)); /* never dimmed (a message): over too */
}

void test_keystate_expected_bytes_per_kind(void)
{
    static surf_registry_t r;
    memset(&r, 0, sizeof r);
    surfreg_attach(&r, "streamdeck:NEO", &decks[0], M(PID_NEO));
    surfreg_attach(&r, "streamdeck:PLUS", &decks[1], M(PID_PLUS));
    surfreg_attach(&r, "streamdeck:PEDAL", &decks[2], M(PID_PEDAL));
    int slot;
    const deck_ctl_t *ctl;
    size_t exp;
    sat_key_state_t ks = {.device_id = "streamdeck:NEO", .control_id = "2/0"};
    TEST_ASSERT_EQUAL(0, surf_route_keystate(&r, &ks, &slot, &ctl, &exp));
    TEST_ASSERT_EQUAL(CTL_KEY_RGB, ctl->kind);
    TEST_ASSERT_EQUAL(0, exp); /* colour-only */
    ks.control_id = "2/1";
    TEST_ASSERT_EQUAL(0, surf_route_keystate(&r, &ks, &slot, &ctl, &exp));
    TEST_ASSERT_EQUAL(248 * 58 * 3, exp);

    ks.device_id = "streamdeck:PLUS";
    ks.control_id = "2/3";
    TEST_ASSERT_EQUAL(0, surf_route_keystate(&r, &ks, &slot, &ctl, &exp));
    TEST_ASSERT_EQUAL(CTL_LCD_CELL, ctl->kind);
    TEST_ASSERT_EQUAL(200 * 100 * 3, exp);
    ks.control_id = "3/1"; /* encoder: nothing to draw */
    TEST_ASSERT_EQUAL(-1, surf_route_keystate(&r, &ks, &slot, &ctl, &exp));

    ks.device_id = "streamdeck:PEDAL";
    ks.control_id = "0/1"; /* pedals never get images */
    TEST_ASSERT_EQUAL(-1, surf_route_keystate(&r, &ks, &slot, &ctl, &exp));
}

/* ---- coalescer ---- */

void test_coalesce_replaces_pending(void)
{
    static coalesce_t c;
    memset(&c, 0, sizeof c);
    int a, b, ctl = -1;
    TEST_ASSERT_NULL(coalesce_put(&c, 2, 3, &a));
    TEST_ASSERT_EQUAL_PTR(&a, coalesce_put(&c, 2, 3, &b));
    TEST_ASSERT_EQUAL_PTR(&b, coalesce_take(&c, 2, &ctl));
    TEST_ASSERT_EQUAL(3, ctl);
    TEST_ASSERT_NULL(coalesce_take(&c, 2, &ctl));
}

void test_coalesce_round_robin(void)
{
    static coalesce_t c;
    memset(&c, 0, sizeof c);
    int a, b, ctl = -1;
    coalesce_put(&c, 0, 5, &b);
    coalesce_put(&c, 0, 1, &a);
    TEST_ASSERT_EQUAL_PTR(&a, coalesce_take(&c, 0, &ctl));
    TEST_ASSERT_EQUAL(1, ctl);
    TEST_ASSERT_EQUAL_PTR(&b, coalesce_take(&c, 0, &ctl));
    TEST_ASSERT_EQUAL(5, ctl);
    TEST_ASSERT_NULL(coalesce_take(&c, 0, &ctl));
    /* continues after the last control taken, wrapping around */
    coalesce_put(&c, 0, 2, &a);
    coalesce_put(&c, 0, 7, &b);
    TEST_ASSERT_EQUAL_PTR(&b, coalesce_take(&c, 0, &ctl));
    TEST_ASSERT_EQUAL_PTR(&a, coalesce_take(&c, 0, &ctl));
}

static int freed;
static void count_free(void *p)
{
    (void)p;
    freed++;
}

void test_coalesce_drop_slot_and_bounds(void)
{
    static coalesce_t c;
    memset(&c, 0, sizeof c);
    int a, b, x, ctl;
    coalesce_put(&c, 1, 0, &a);
    coalesce_put(&c, 1, 63, &b);
    coalesce_put(&c, 4, 9, &x);
    freed = 0;
    coalesce_drop_slot(&c, 1, count_free);
    TEST_ASSERT_EQUAL(2, freed);
    TEST_ASSERT_NULL(coalesce_take(&c, 1, &ctl));
    TEST_ASSERT_EQUAL_PTR(&x, coalesce_take(&c, 4, &ctl));
    TEST_ASSERT_EQUAL_PTR(&a, coalesce_put(&c, SURF_MAX_DECKS, 0, &a));
    TEST_ASSERT_EQUAL_PTR(&a, coalesce_put(&c, 0, SURF_MAX_CTLS, &a));
    TEST_ASSERT_EQUAL_PTR(&a, coalesce_put(&c, -1, 0, &a));
    TEST_ASSERT_NULL(coalesce_take(&c, SURF_MAX_DECKS, &ctl));
}

/* ---- input map ---- */

static int translate(uint16_t pid, deck_event_t ev, surf_action_t *a)
{
    memset(a, 0, sizeof(surf_action_t) * 4);
    return inputmap_translate(M(pid), &ev, a, 4);
}

void test_inputmap_xl_key(void)
{
    surf_action_t a[4];
    TEST_ASSERT_EQUAL(1, translate(PID_XL, (deck_event_t){.kind = DECK_EV_KEY, .index = 9, .pressed = true}, a));
    TEST_ASSERT_EQUAL(ACT_PRESS, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("1/1", a[0].control_id);
    TEST_ASSERT_EQUAL(1, translate(PID_XL, (deck_event_t){.kind = DECK_EV_KEY, .index = 9, .pressed = false}, a));
    TEST_ASSERT_EQUAL(ACT_RELEASE, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("1/1", a[0].control_id);
}

/* the control id is formatted by hand (no snprintf on the deck task): two-digit rows and columns too */
void test_inputmap_id_two_digits(void)
{
    const deck_model_t wide = {.cols = 12, .rows = 11};
    surf_action_t a[2];
    const deck_event_t last = {.kind = DECK_EV_KEY, .index = 12 * 11 - 1, .pressed = true};
    TEST_ASSERT_EQUAL(1, inputmap_translate(&wide, &last, a, 2));
    TEST_ASSERT_EQUAL_STRING("10/11", a[0].control_id);
    const deck_event_t mid = {.kind = DECK_EV_KEY, .index = 12 * 9 + 10, .pressed = true};
    TEST_ASSERT_EQUAL(1, inputmap_translate(&wide, &mid, a, 2));
    TEST_ASSERT_EQUAL_STRING("9/10", a[0].control_id);
}

void test_inputmap_orig_v1_rtl(void)
{
    surf_action_t a[4];
    TEST_ASSERT_EQUAL(1, translate(PID_ORIG_V1, (deck_event_t){.kind = DECK_EV_KEY, .index = 4, .pressed = true}, a));
    TEST_ASSERT_EQUAL(ACT_PRESS, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("0/0", a[0].control_id);
}

void test_inputmap_plus_encoder_rotate(void)
{
    surf_action_t a[4];
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS, (deck_event_t){.kind = DECK_EV_ENC_ROTATE, .index = 2, .delta = -1}, a));
    TEST_ASSERT_EQUAL(ACT_ROTATE, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("3/2", a[0].control_id);
    TEST_ASSERT_EQUAL(-1, a[0].delta);
}

void test_inputmap_plus_encoder_press(void)
{
    surf_action_t a[4];
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS_XL, (deck_event_t){.kind = DECK_EV_ENC_PRESS, .index = 5, .pressed = true}, a));
    TEST_ASSERT_EQUAL(ACT_PRESS, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("5/8", a[0].control_id);
    /* no such encoder / no encoders at all */
    TEST_ASSERT_EQUAL(0, translate(PID_PLUS, (deck_event_t){.kind = DECK_EV_ENC_PRESS, .index = 4, .pressed = true}, a));
    TEST_ASSERT_EQUAL(0, translate(PID_XL, (deck_event_t){.kind = DECK_EV_ENC_ROTATE, .index = 0, .delta = 1}, a));
}

void test_inputmap_plus_tap_cell(void)
{
    surf_action_t a[4];
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS, (deck_event_t){.kind = DECK_EV_TOUCH_TAP, .x = 450, .y = 40}, a));
    TEST_ASSERT_EQUAL(ACT_TAP, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("2/2", a[0].control_id);
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS, (deck_event_t){.kind = DECK_EV_TOUCH_LONG, .x = 10, .y = 40}, a));
    TEST_ASSERT_EQUAL(ACT_TAP, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("2/0", a[0].control_id);
    /* off the right edge clamps to the last cell */
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS, (deck_event_t){.kind = DECK_EV_TOUCH_TAP, .x = 800, .y = 40}, a));
    TEST_ASSERT_EQUAL_STRING("2/3", a[0].control_id);
}

void test_inputmap_plus_xl_tap_cell(void)
{
    surf_action_t a[4];
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS_XL, (deck_event_t){.kind = DECK_EV_TOUCH_TAP, .x = 1150, .y = 50}, a));
    TEST_ASSERT_EQUAL(ACT_TAP, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("4/8", a[0].control_id);
}

void test_inputmap_swipe_vertical_up(void)
{
    surf_action_t a[4];
    deck_event_t ev = {.kind = DECK_EV_TOUCH_SWIPE, .x = 450, .y = 90, .x2 = 455, .y2 = 10};
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS, ev, a));
    TEST_ASSERT_EQUAL(ACT_ROTATE, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("2/2", a[0].control_id);
    TEST_ASSERT_EQUAL(1, a[0].delta);
    ev = (deck_event_t){.kind = DECK_EV_TOUCH_SWIPE, .x = 455, .y = 10, .x2 = 450, .y2 = 90};
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS, ev, a));
    TEST_ASSERT_EQUAL(-1, a[0].delta);
    /* vertical but crossing into another cell: nothing */
    ev = (deck_event_t){.kind = DECK_EV_TOUCH_SWIPE, .x = 395, .y = 90, .x2 = 405, .y2 = 10};
    TEST_ASSERT_EQUAL(0, translate(PID_PLUS, ev, a));
}

void test_inputmap_swipe_horizontal_left(void)
{
    surf_action_t a[4];
    deck_event_t ev = {.kind = DECK_EV_TOUCH_SWIPE, .x = 700, .y = 50, .x2 = 100, .y2 = 55};
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS, ev, a));
    TEST_ASSERT_EQUAL(ACT_PAGE, a[0].kind);
    TEST_ASSERT_TRUE(a[0].forward);
    ev = (deck_event_t){.kind = DECK_EV_TOUCH_SWIPE, .x = 100, .y = 55, .x2 = 700, .y2 = 50};
    TEST_ASSERT_EQUAL(1, translate(PID_PLUS, ev, a));
    TEST_ASSERT_EQUAL(ACT_PAGE, a[0].kind);
    TEST_ASSERT_FALSE(a[0].forward);
}

void test_inputmap_swipe_ambiguous(void)
{
    surf_action_t a[4];
    /* atan(58/100) = 30.1 degrees: neither vertical (>= 50) nor horizontal (<= 22.5) */
    deck_event_t ev = {.kind = DECK_EV_TOUCH_SWIPE, .x = 300, .y = 20, .x2 = 400, .y2 = 78};
    TEST_ASSERT_EQUAL(0, translate(PID_PLUS, ev, a));
}

void test_inputmap_neo_touch_key(void)
{
    surf_action_t a[4];
    TEST_ASSERT_EQUAL(1, translate(PID_NEO, (deck_event_t){.kind = DECK_EV_KEY, .index = 8, .pressed = true}, a));
    TEST_ASSERT_EQUAL(ACT_PRESS, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("2/0", a[0].control_id);
    TEST_ASSERT_EQUAL(1, translate(PID_NEO, (deck_event_t){.kind = DECK_EV_KEY, .index = 9, .pressed = false}, a));
    TEST_ASSERT_EQUAL(ACT_RELEASE, a[0].kind);
    TEST_ASSERT_EQUAL_STRING("2/3", a[0].control_id);
}

/* Every key/pedal/touch-key HID index maps to the same control ID the model table produces. */
void test_inputmap_keys_match_model_controls(void)
{
    static const uint16_t pids[] = {0x0060, 0x0063, 0x0090, 0x00B3, 0x00B8, 0x006D, 0x0080, 0x00A5,
                                    0x00B9, 0x00E4, 0x006C, 0x008F, 0x00BA, 0x009A, 0x0084, 0x00C6,
                                    0x0086};
    for (size_t p = 0; p < sizeof pids / sizeof pids[0]; p++) {
        const deck_model_t *m = M(pids[p]);
        deck_ctl_t l[SURF_MAX_CTLS];
        int n = deck_model_controls(m, l, SURF_MAX_CTLS);
        TEST_ASSERT_TRUE(n > 0);
        int keys = 0;
        for (int i = 0; i < n; i++) {
            if (l[i].kind != CTL_KEY_LCD && l[i].kind != CTL_KEY_RGB && l[i].kind != CTL_KEY_NONE) continue;
            char want[8];
            deck_ctl_id(&l[i], want);
            surf_action_t a[2];
            deck_event_t ev = {.kind = DECK_EV_KEY, .index = l[i].hw, .pressed = true};
            TEST_ASSERT_EQUAL_MESSAGE(1, inputmap_translate(m, &ev, a, 2), m->name);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(want, a[0].control_id, m->name);
            keys++;
        }
        surf_action_t a[2];
        deck_event_t ev = {.kind = DECK_EV_KEY, .index = (uint8_t)keys, .pressed = true};
        TEST_ASSERT_EQUAL_MESSAGE(0, inputmap_translate(m, &ev, a, 2), m->name);
    }
}

/* The key click (components/feedback) on every key-down: a key, pedal or touch key pressed, an encoder
 * pushed, the LCD strip touched. Not on releases, turns or swipes. Model-independent: dock mode sees
 * only the parsed report. */
void test_inputmap_key_down(void)
{
    TEST_ASSERT_TRUE(inputmap_key_down(&(deck_event_t){.kind = DECK_EV_KEY, .index = 3, .pressed = true}));
    TEST_ASSERT_FALSE(inputmap_key_down(&(deck_event_t){.kind = DECK_EV_KEY, .index = 3, .pressed = false}));
    TEST_ASSERT_TRUE(inputmap_key_down(&(deck_event_t){.kind = DECK_EV_ENC_PRESS, .index = 1, .pressed = true}));
    TEST_ASSERT_FALSE(inputmap_key_down(&(deck_event_t){.kind = DECK_EV_ENC_PRESS, .index = 1, .pressed = false}));
    TEST_ASSERT_FALSE(inputmap_key_down(&(deck_event_t){.kind = DECK_EV_ENC_ROTATE, .index = 1, .delta = 2}));
    TEST_ASSERT_TRUE(inputmap_key_down(&(deck_event_t){.kind = DECK_EV_TOUCH_TAP, .x = 100, .y = 50}));
    TEST_ASSERT_TRUE(inputmap_key_down(&(deck_event_t){.kind = DECK_EV_TOUCH_LONG, .x = 100, .y = 50}));
    TEST_ASSERT_FALSE(inputmap_key_down(&(deck_event_t){.kind = DECK_EV_TOUCH_SWIPE, .x = 10, .x2 = 300}));
    TEST_ASSERT_FALSE(inputmap_key_down(NULL));
}

/* ---- deck message ---- */

void test_message_valid_accepts(void)
{
    char big[SURF_MSG_MAX + 1];
    memset(big, '~', SURF_MSG_MAX);
    big[SURF_MSG_MAX] = '\0';
    TEST_ASSERT_TRUE(surf_message_valid("Hello", 0));
    TEST_ASSERT_TRUE(surf_message_valid(" ", 1));
    TEST_ASSERT_TRUE(surf_message_valid("Line 1\nLine 2\n", 3600));
    TEST_ASSERT_TRUE(surf_message_valid(big, 60));
}

void test_message_valid_rejects(void)
{
    char big[SURF_MSG_MAX + 2];
    memset(big, 'a', SURF_MSG_MAX + 1);
    big[SURF_MSG_MAX + 1] = '\0';
    TEST_ASSERT_FALSE(surf_message_valid(NULL, 0));
    TEST_ASSERT_FALSE(surf_message_valid("", 0));
    TEST_ASSERT_FALSE(surf_message_valid(big, 0));          /* 161 bytes */
    TEST_ASSERT_FALSE(surf_message_valid("tab\there", 0));
    TEST_ASSERT_FALSE(surf_message_valid("cr\r\n", 0));
    TEST_ASSERT_FALSE(surf_message_valid("del\x7f", 0));
    TEST_ASSERT_FALSE(surf_message_valid("caf\xc3\xa9", 0)); /* UTF-8 */
    TEST_ASSERT_FALSE(surf_message_valid("ok", 3601));
}

void test_message_idle_at_start(void)
{
    surf_message_t m = {0};
    TEST_ASSERT_EQUAL(SURF_MSG_IDLE, surf_message_step(&m, 0, NULL));
    TEST_ASSERT_FALSE(surf_message_clear(&m)); /* nothing to clear: no redraw either */
    TEST_ASSERT_EQUAL(SURF_MSG_IDLE, surf_message_step(&m, 1000, NULL));
}

void test_message_show_clear_before_first_tick(void)
{
    surf_message_t m = {0};
    surf_message_show(&m, 0);
    TEST_ASSERT_TRUE(surf_message_clear(&m));
    TEST_ASSERT_EQUAL(SURF_MSG_END, surf_message_step(&m, 10, NULL));
    TEST_ASSERT_EQUAL(SURF_MSG_IDLE, surf_message_step(&m, 20, NULL));
}

void test_message_show_expire_before_first_tick(void)
{
    surf_message_t m = {0};
    bool expired = false;
    surf_message_show(&m, 1000);
    TEST_ASSERT_EQUAL(SURF_MSG_END, surf_message_step(&m, 2000, &expired));
    TEST_ASSERT_TRUE(expired);
    TEST_ASSERT_FALSE(m.on);
    TEST_ASSERT_EQUAL(SURF_MSG_IDLE, surf_message_step(&m, 3000, &expired));
    TEST_ASSERT_FALSE(expired);
}

void test_message_show_tick_clear(void)
{
    surf_message_t m = {0};
    surf_message_show(&m, 0);
    TEST_ASSERT_EQUAL(SURF_MSG_DRAW, surf_message_step(&m, 10, NULL));
    TEST_ASSERT_EQUAL(SURF_MSG_DRAW, surf_message_step(&m, 1000000000, NULL)); /* no expiry */
    TEST_ASSERT_TRUE(surf_message_clear(&m));
    TEST_ASSERT_FALSE(surf_message_clear(&m));
    TEST_ASSERT_EQUAL(SURF_MSG_END, surf_message_step(&m, 20, NULL));
    TEST_ASSERT_EQUAL(SURF_MSG_IDLE, surf_message_step(&m, 30, NULL));
}

void test_message_show_tick_expire(void)
{
    surf_message_t m = {0};
    bool expired = true;
    surf_message_show(&m, 5000);
    TEST_ASSERT_EQUAL(SURF_MSG_DRAW, surf_message_step(&m, 4999, &expired));
    TEST_ASSERT_FALSE(expired);
    TEST_ASSERT_EQUAL(SURF_MSG_END, surf_message_step(&m, 5000, &expired));
    TEST_ASSERT_TRUE(expired);
    TEST_ASSERT_EQUAL(SURF_MSG_IDLE, surf_message_step(&m, 6000, NULL));
}

void test_message_replace_mid_way(void)
{
    surf_message_t m = {0};
    surf_message_show(&m, 5000);
    TEST_ASSERT_EQUAL(SURF_MSG_DRAW, surf_message_step(&m, 10, NULL));
    const uint32_t first = m.seq;
    surf_message_show(&m, 0); /* new text and timer: the old expiry no longer applies */
    TEST_ASSERT_EQUAL(SURF_MSG_DRAW, surf_message_step(&m, 6000, NULL));
    TEST_ASSERT_NOT_EQUAL(first, m.seq); /* the decks redraw for the new message */

    /* replaced and then cleared before the next tick: still ends */
    surf_message_show(&m, 0);
    surf_message_clear(&m);
    TEST_ASSERT_EQUAL(SURF_MSG_END, surf_message_step(&m, 7000, NULL));

    /* cleared and shown again before the next tick: draws the new one, no end needed */
    surf_message_show(&m, 0);
    TEST_ASSERT_EQUAL(SURF_MSG_DRAW, surf_message_step(&m, 8000, NULL));
    const uint32_t drawn = m.seq;
    surf_message_clear(&m);
    surf_message_show(&m, 0);
    TEST_ASSERT_EQUAL(SURF_MSG_DRAW, surf_message_step(&m, 9000, NULL));
    TEST_ASSERT_NOT_EQUAL(drawn, m.seq);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registry_attach_detach);
    RUN_TEST(test_registry_full_returns_minus1);
    RUN_TEST(test_registry_deck_limit);
    RUN_TEST(test_registry_same_serial_reuses_slot);
    RUN_TEST(test_ready_readds_all_attached);
    RUN_TEST(test_keystate_unknown_device_dropped);
    RUN_TEST(test_keystate_unknown_control_dropped);
    RUN_TEST(test_bitmap_wrong_size_dropped);
    RUN_TEST(test_bitmap_text_size);
    RUN_TEST(test_loss_phase);
    RUN_TEST(test_keystate_expected_bytes_per_kind);
    RUN_TEST(test_coalesce_replaces_pending);
    RUN_TEST(test_coalesce_round_robin);
    RUN_TEST(test_coalesce_drop_slot_and_bounds);
    RUN_TEST(test_inputmap_xl_key);
    RUN_TEST(test_inputmap_orig_v1_rtl);
    RUN_TEST(test_inputmap_plus_encoder_rotate);
    RUN_TEST(test_inputmap_plus_encoder_press);
    RUN_TEST(test_inputmap_plus_tap_cell);
    RUN_TEST(test_inputmap_plus_xl_tap_cell);
    RUN_TEST(test_inputmap_swipe_vertical_up);
    RUN_TEST(test_inputmap_swipe_horizontal_left);
    RUN_TEST(test_inputmap_swipe_ambiguous);
    RUN_TEST(test_inputmap_neo_touch_key);
    RUN_TEST(test_inputmap_keys_match_model_controls);
    RUN_TEST(test_inputmap_key_down);
    RUN_TEST(test_inputmap_id_two_digits);
    RUN_TEST(test_message_valid_accepts);
    RUN_TEST(test_message_valid_rejects);
    RUN_TEST(test_message_idle_at_start);
    RUN_TEST(test_message_show_clear_before_first_tick);
    RUN_TEST(test_message_show_expire_before_first_tick);
    RUN_TEST(test_message_show_tick_clear);
    RUN_TEST(test_message_show_tick_expire);
    RUN_TEST(test_message_replace_mid_way);
    return UNITY_END();
}
