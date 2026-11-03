/* The deck's static-address editor (components/deckui/ipedit.c): the octet entry (the
 * auto-advance, the 255 rule, ⌫ across octets, Next across fields), the mask prefix and its dotted form,
 * the checks, the summary's Save (its hold, its lock), the cancel and the idle cancel, and the layout per
 * model. Its host mode (the Companion's IP): one field, the checks without a subnet, the summary with the
 * port and Save, the cancel, and the layout on every model. */
#include <stdio.h>
#include <string.h>

#include "deckmodel/model.h"
#include "deckui/deckui.h"
#include "deckui/ipedit.h"
#include "deckui/nav.h"
#include "deckui/pin.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function" /* fit_check.h's helpers this test doesn't use */
#include "fit_check.h"
#pragma GCC diagnostic pop
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

enum { PID_M32 = 0x00BA, PID_PXL = 0x00C6, PID_MK2 = 0x0080, PID_PLUS = 0x0084, PID_NEO = 0x009A, PID_MINI = 0x0063 };

static dui_deck_t D;
static dui_ipe_t E;
static dui_layout_t L;
static int64_t NOW;

static void deck(uint16_t pid) { TEST_ASSERT_TRUE(dui_deck_of(deck_model_find(DECK_VID_ELGATO, pid), &D)); }

static void open_ed(uint16_t pid, const dui_ipset_t *pre)
{
    deck(pid);
    NOW = 5000;
    dui_ipe_out_t o;
    dui_ipe_open(&E, &D, pre, NOW, &o);
    TEST_ASSERT_TRUE(o.redraw);
    dui_ipe_layout(&D, &E, NOW, &L);
}

static void typed(const char *digits)
{
    for (const char *p = digits; *p; p++) dui_ipe_digit(&E, *p - '0');
}

static void values(dui_ipset_t *v) { dui_ipe_values(&E, v); }

/* Keys as the person presses them: digits, 'N' Next, '<' delete, ' ' nothing (readability). */
static void keys(const char *seq)
{
    for (const char *p = seq; *p; p++) {
        if (*p >= '0' && *p <= '9') dui_ipe_digit(&E, *p - '0');
        else if (*p == 'N') dui_ipe_next(&E);
        else if (*p == '<') dui_ipe_del(&E);
    }
}

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

static dui_ipe_out_t press(uint16_t action, int64_t hold_ms)
{
    dui_ipe_layout(&D, &E, NOW, &L);
    int c, r;
    char msg[32];
    snprintf(msg, sizeof msg, "action 0x%x", action);
    TEST_ASSERT_TRUE_MESSAGE(find(action, E.pg, &c, &r), msg);
    dui_ipe_out_t o, acc = {0};
    dui_ipe_key(&E, &L, c, r, true, NOW, &o);
    for (int64_t t = 0; t < hold_ms; t += 50) {
        NOW += 50;
        dui_ipe_tick(&E, NOW, &o);
        acc.save |= o.save;
        acc.closed |= o.closed;
        if (o.sound) acc.sound = o.sound;
    }
    dui_ipe_key(&E, &L, c, r, false, NOW, &o);
    acc.save |= o.save;
    acc.closed |= o.closed;
    if (o.sound) acc.sound = o.sound;
    NOW += 20;
    return acc;
}

/* ---- the octet entry ---- */

static void test_octets_auto_advance(void)
{
    open_ed(PID_M32, NULL);
    typed("192"); /* 3 digits: on to the next octet */
    TEST_ASSERT_EQUAL(1, E.oct);
    typed("168");
    typed("8");
    TEST_ASSERT_EQUAL(2, E.oct); /* one digit: still on it */
    typed("50");  /* "85" is fine, then "850" would pass 255: the 0 starts the next octet */
    TEST_ASSERT_EQUAL(3, E.oct);
    dui_ipset_t v;
    values(&v);
    TEST_ASSERT_EQUAL_STRING("192.168.85.0", v.ip);
    /* 25 then 6: 256 would pass 255, so 6 starts the next octet */
    open_ed(PID_M32, NULL);
    typed("25");
    typed("6");
    TEST_ASSERT_EQUAL(1, E.oct);
    TEST_ASSERT_EQUAL(25, E.q[0].v[0]);
    TEST_ASSERT_EQUAL(6, E.q[0].v[1]);
    typed("255"); /* 6, 62, then 625 would pass 255: the 5 starts the next octet, then 55 */
    TEST_ASSERT_EQUAL(62, E.q[0].v[1]);
    TEST_ASSERT_EQUAL(2, E.oct);
    TEST_ASSERT_EQUAL(55, E.q[0].v[2]);
    /* no advance after a 0 typed first: only 3 digits or the 255 rule move on */
    open_ed(PID_M32, NULL);
    keys("10 0");
    TEST_ASSERT_EQUAL(1, E.oct); /* "100": 3 digits, on */
    TEST_ASSERT_EQUAL(100, E.q[0].v[0]);
    keys("N 0");
    TEST_ASSERT_EQUAL(1, E.oct); /* a 0 typed: still on it */
    TEST_ASSERT_EQUAL(0, E.q[0].v[1]);
    keys("N 0 N 7");
    values(&v);
    TEST_ASSERT_EQUAL_STRING("100.0.0.7", v.ip);
    TEST_ASSERT_EQUAL(3, E.oct);
    /* the last octet full (3 digits typed), or a digit past 255 there: refused */
    keys("<"); /* the 7 off: the last octet empty */
    keys("72");
    TEST_ASSERT_EQUAL(72, E.q[0].v[3]);
    TEST_ASSERT_FALSE(dui_ipe_digit(&E, 5)); /* 725 > 255, nowhere to go */
    TEST_ASSERT_EQUAL(72, E.q[0].v[3]);
    keys("<<");
    keys("100");
    TEST_ASSERT_FALSE(dui_ipe_digit(&E, 1)); /* 3 digits typed: full */
    TEST_ASSERT_EQUAL(100, E.q[0].v[3]);
}

/* Next right after an auto-advance only confirms the move: "172 Next 16 Next 100 Next 5" over the
 * lease 192.168.8.77 gives 172.16.100.5, never 172.168.16.100. */
static void test_next_after_advance_confirms(void)
{
    const dui_ipset_t lease = {"192.168.8.77", "255.255.255.0", "192.168.8.1", ""};
    open_ed(PID_M32, &lease);
    keys("172 N 16 N 100 N 5");
    dui_ipset_t v;
    values(&v);
    TEST_ASSERT_EQUAL_STRING("172.16.100.5", v.ip);
    TEST_ASSERT_EQUAL(3, E.oct);
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    /* the same without the confirming Nexts */
    open_ed(PID_M32, &lease);
    keys("172 16 N 100 5");
    values(&v);
    TEST_ASSERT_EQUAL_STRING("172.16.100.5", v.ip);
    /* a second Next after the confirm moves on, as usual */
    open_ed(PID_M32, &lease);
    keys("172 N N");
    TEST_ASSERT_EQUAL(2, E.oct);
    TEST_ASSERT_EQUAL(168, E.q[0].v[1]); /* the prefill kept: the person chose to keep it */
}

/* ⌫ right after an auto-advance goes back into the previous octet and deletes its last digit typed; the
 * prefilled octet it moved to is untouched. */
static void test_del_after_advance(void)
{
    const dui_ipset_t lease = {"192.168.8.77", "255.255.255.0", "", ""};
    open_ed(PID_M32, &lease);
    keys("193");
    TEST_ASSERT_EQUAL(1, E.oct);
    keys("<");
    TEST_ASSERT_EQUAL(0, E.oct);
    TEST_ASSERT_EQUAL(19, E.q[0].v[0]);
    TEST_ASSERT_EQUAL(168, E.q[0].v[1]); /* untouched */
    keys("2");
    TEST_ASSERT_EQUAL(192, E.q[0].v[0]);
    TEST_ASSERT_EQUAL(1, E.oct); /* 3 digits again: on */
    /* typing over a prefill: the first digit replaces it */
    keys("1");
    TEST_ASSERT_EQUAL(1, E.q[0].v[1]);
    keys("0");
    TEST_ASSERT_EQUAL(10, E.q[0].v[1]);
}

static void test_prefilled_digit_replaces(void)
{
    const dui_ipset_t pre = {"192.168.8.50", "255.255.255.0", "192.168.8.1", "8.8.8.8"};
    open_ed(PID_M32, &pre);
    TEST_ASSERT_EQUAL(0x0F, E.q[0].set);
    TEST_ASSERT_EQUAL(24, E.prefix);
    TEST_ASSERT_TRUE(E.fresh);
    typed("1"); /* the first digit replaces 192 */
    TEST_ASSERT_EQUAL(1, E.q[0].v[0]);
    typed("0");
    TEST_ASSERT_EQUAL(10, E.q[0].v[0]);
    TEST_ASSERT_EQUAL(0, E.oct);
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E)); /* Next: the next octet, as it was */
    TEST_ASSERT_EQUAL(1, E.oct);
    TEST_ASSERT_EQUAL(168, E.q[0].v[1]);
    dui_ipset_t v;
    values(&v);
    TEST_ASSERT_EQUAL_STRING("10.168.8.50", v.ip);
    TEST_ASSERT_EQUAL_STRING("192.168.8.1", v.gw);
    TEST_ASSERT_EQUAL_STRING("8.8.8.8", v.dns);
}

static void test_del_across_octets(void)
{
    open_ed(PID_M32, NULL);
    typed("192168");
    TEST_ASSERT_EQUAL(2, E.oct);
    dui_ipe_del(&E); /* octet 2 empty: back to octet 1, its last digit off */
    TEST_ASSERT_EQUAL(1, E.oct);
    TEST_ASSERT_EQUAL(16, E.q[0].v[1]);
    dui_ipe_del(&E);
    dui_ipe_del(&E);
    TEST_ASSERT_FALSE(E.q[0].set & 2); /* 16 -> 1 -> empty */
    TEST_ASSERT_EQUAL(1, E.oct);
    dui_ipe_del(&E); /* empty: back to octet 0 */
    TEST_ASSERT_EQUAL(0, E.oct);
    TEST_ASSERT_EQUAL(19, E.q[0].v[0]);
    typed("2"); /* typing goes on from 19 */
    TEST_ASSERT_EQUAL(192, E.q[0].v[0]);
    TEST_ASSERT_EQUAL(1, E.oct);
    /* a prefilled octet: ⌫ takes its last digit */
    const dui_ipset_t pre = {"192.168.8.50", "255.255.255.0", "", ""};
    open_ed(PID_M32, &pre);
    dui_ipe_del(&E);
    TEST_ASSERT_EQUAL(19, E.q[0].v[0]);
    TEST_ASSERT_FALSE(E.fresh);
    typed("5");
    TEST_ASSERT_EQUAL(195, E.q[0].v[0]);
}

static void test_next_across_fields(void)
{
    const dui_ipset_t pre = {"192.168.8.50", "255.255.255.0", "192.168.8.1", ""};
    open_ed(PID_M32, &pre);
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E));
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E)); /* the last octet: the next field */
    TEST_ASSERT_EQUAL(DUI_IPE_MASK, E.field);
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E));
    TEST_ASSERT_EQUAL(DUI_IPE_GW, E.field);
    TEST_ASSERT_EQUAL(0, E.oct);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E));
    TEST_ASSERT_EQUAL(DUI_IPE_DNS, E.field);
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E)); /* an empty DNS: Next skips it */
    TEST_ASSERT_EQUAL(DUI_IPE_SUM, E.field);
    dui_ipe_del(&E); /* the summary's back: the DNS (its first octet: it is empty) */
    TEST_ASSERT_EQUAL(DUI_IPE_DNS, E.field);
    TEST_ASSERT_EQUAL(0, E.oct);
    dui_ipe_del(&E); /* DNS's first octet, empty: back to the Gateway's last */
    TEST_ASSERT_EQUAL(DUI_IPE_GW, E.field);
    TEST_ASSERT_EQUAL(3, E.oct);
    E.oct = 0;
    E.q[1].set = 0;
    dui_ipe_del(&E); /* the Gateway empty at its first octet: back to the Mask */
    TEST_ASSERT_EQUAL(DUI_IPE_MASK, E.field);
    dui_ipe_del(&E); /* the Mask: back to the IP's last octet */
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    TEST_ASSERT_EQUAL(3, E.oct);
}

/* ---- the mask ---- */

static void test_mask_prefix(void)
{
    uint8_t m[4];
    dui_mask_of_prefix(24, m);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){255, 255, 255, 0}), m, 4);
    dui_mask_of_prefix(20, m);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){255, 255, 240, 0}), m, 4);
    dui_mask_of_prefix(1, m);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){128, 0, 0, 0}), m, 4);
    dui_mask_of_prefix(32, m);
    TEST_ASSERT_EQUAL(32, dui_prefix_of_mask(m));
    dui_mask_of_prefix(0, m);
    TEST_ASSERT_EQUAL(0, dui_prefix_of_mask(m));
    for (int p = 0; p <= 32; p++) {
        dui_mask_of_prefix(p, m);
        TEST_ASSERT_EQUAL(p, dui_prefix_of_mask(m));
    }
    TEST_ASSERT_EQUAL(-1, dui_prefix_of_mask((uint8_t[]){255, 0, 255, 0}));
    /* the editor: − / + in 1..30; a mask that isn't a prefix opens as /24 */
    const dui_ipset_t pre = {"10.1.2.3", "255.0.255.0", "", ""};
    open_ed(PID_M32, &pre);
    TEST_ASSERT_EQUAL(24, E.prefix);
    E.field = DUI_IPE_MASK;
    dui_ipe_step(&E, -100);
    TEST_ASSERT_EQUAL(1, E.prefix);
    dui_ipe_step(&E, 100);
    TEST_ASSERT_EQUAL(30, E.prefix);
    dui_ipe_step(&E, -10);
    dui_ipset_t v;
    values(&v);
    TEST_ASSERT_EQUAL_STRING("255.255.240.0", v.mask);
    char msg[24];
    dui_ipe_message(&E, NOW, msg, sizeof msg); /* the words name the prefix */
    TEST_ASSERT_EQUAL_STRING("Mask /20", msg);
    /* on the 32-key decks the prefix sits between − and +, where the steps change it */
    dui_ipe_layout(&D, &E, NOW, &L);
    int c0, r0, c1, r1;
    TEST_ASSERT_TRUE(find(DUI_IPE_A_DOWN, 0, &c0, &r0));
    TEST_ASSERT_TRUE(find(DUI_IPE_A_UP, 0, &c1, &r1));
    TEST_ASSERT_TRUE(r0 == r1 && c1 == c0 + 2);
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, c0 + 1, r0, &t));
    TEST_ASSERT_EQUAL(DUI_TILE_CAP, t.kind);
    TEST_ASSERT_EQUAL_STRING("/20", t.text);
    for (int c = 6; c < D.cols; c++) /* and no longer in the status row */
        if (dui_layout_key(&L, 0, c, 0, &t)) TEST_ASSERT_NOT_EQUAL(DUI_TILE_CAP, t.kind);
}

/* ---- the checks ---- */

static void test_validation(void)
{
    open_ed(PID_M32, NULL);
    keys("192 168 0 N 1"); /* 192.168.0.1 */
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_check(&E, DUI_IPE_IP));
    E.prefix = 24;
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_check(&E, DUI_IPE_MASK));
    E.q[0].v[3] = 0; /* the network address */
    TEST_ASSERT_EQUAL(DUI_IPE_E_NETWORK, dui_ipe_check(&E, DUI_IPE_MASK));
    E.q[0].v[3] = 255; /* the broadcast */
    TEST_ASSERT_EQUAL(DUI_IPE_E_BROADCAST, dui_ipe_check(&E, DUI_IPE_MASK));
    E.prefix = 23; /* in a /23, .0.255 is a host */
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_check(&E, DUI_IPE_MASK));
    E.q[0].set = 0x07; /* not complete */
    TEST_ASSERT_EQUAL(DUI_IPE_E_IP_PART, dui_ipe_check(&E, DUI_IPE_IP));
    E.q[0].set = 0x0F;
    E.q[0].v[3] = 10;
    E.prefix = 24;
    /* the gateway: empty, or complete and in the subnet */
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_check(&E, DUI_IPE_GW));
    E.q[1] = (dui_quad_t){{192, 168, 0, 1}, 0x0F};
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_check(&E, DUI_IPE_GW));
    E.q[1].v[2] = 1;
    TEST_ASSERT_EQUAL(DUI_IPE_E_GW_SUBNET, dui_ipe_check(&E, DUI_IPE_GW));
    E.q[1].set = 0x03;
    TEST_ASSERT_EQUAL(DUI_IPE_E_GW_PART, dui_ipe_check(&E, DUI_IPE_GW));
    /* the gateway is neither the IP nor the subnet's network or broadcast address */
    E.q[1] = (dui_quad_t){{192, 168, 0, 10}, 0x0F}; /* the IP itself */
    TEST_ASSERT_EQUAL(DUI_IPE_E_GW_HOST, dui_ipe_check(&E, DUI_IPE_GW));
    E.q[1].v[3] = 0;
    TEST_ASSERT_EQUAL(DUI_IPE_E_GW_HOST, dui_ipe_check(&E, DUI_IPE_GW));
    E.q[1].v[3] = 255;
    TEST_ASSERT_EQUAL(DUI_IPE_E_GW_HOST, dui_ipe_check(&E, DUI_IPE_GW));
    E.q[1].set = 0;
    /* ... and the IP a host's: not 0/8, 127/8, multicast or reserved */
    static const uint8_t BAD[] = {0, 127, 224, 239, 240, 255};
    for (size_t k = 0; k < sizeof BAD; k++) {
        E.q[0].v[0] = BAD[k];
        TEST_ASSERT_EQUAL(DUI_IPE_E_RESERVED, dui_ipe_check(&E, DUI_IPE_IP));
        TEST_ASSERT_EQUAL(DUI_IPE_E_RESERVED, dui_ipe_check(&E, DUI_IPE_MASK));
    }
    E.q[0].v[0] = 223;
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_check(&E, DUI_IPE_IP));
    E.q[0].v[0] = 192;
    /* DNS: empty or complete (any address) */
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_check(&E, DUI_IPE_DNS));
    E.q[2] = (dui_quad_t){{1, 1, 1, 1}, 0x0F};
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_check(&E, DUI_IPE_DNS));
    E.q[2].set = 0x01;
    TEST_ASSERT_EQUAL(DUI_IPE_E_DNS_PART, dui_ipe_check(&E, DUI_IPE_DNS));
    /* an error keeps the field, shows in the status with the locked sound, ends with a press */
    open_ed(PID_M32, NULL);
    keys("192 168 0 N 1");
    E.q[0].v[3] = 0;
    E.oct = 3;
    E.fresh = true;
    dui_ipe_out_t o = press(DUI_IPE_A_NEXT, 0);
    TEST_ASSERT_EQUAL(DUI_IPE_MASK, E.field); /* the IP alone is complete: on to the Mask */
    o = press(DUI_IPE_A_NEXT, 0);
    TEST_ASSERT_EQUAL(DUI_IPE_MASK, E.field); /* .0 in a /24: kept on the Mask */
    TEST_ASSERT_EQUAL(DUI_SOUND_LOCKED, o.sound);
    char msg[40];
    dui_ipe_message(&E, NOW, msg, sizeof msg);
    TEST_ASSERT_EQUAL_STRING("IP is the network address", msg);
    dui_ipe_layout(&D, &E, NOW, &L); /* the status row says it, red */
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 1, 0, &t));
    TEST_ASSERT_EQUAL(DUI_TINT_BAD, t.tint);
    NOW += DUI_IPE_ERR_MS;
    dui_ipe_tick(&E, NOW, &o);
    dui_ipe_message(&E, NOW, msg, sizeof msg);
    TEST_ASSERT_EQUAL_STRING("Mask /24", msg);
    /* the gateway out of the subnet */
    E.prefix = 24;
    E.q[0].v[3] = 9;
    press(DUI_IPE_A_NEXT, 0);
    TEST_ASSERT_EQUAL(DUI_IPE_GW, E.field);
    E.q[1] = (dui_quad_t){{10, 0, 0, 1}, 0x0F};
    E.oct = 3;
    o = press(DUI_IPE_A_NEXT, 0);
    TEST_ASSERT_EQUAL(DUI_IPE_GW, E.field);
    dui_ipe_message(&E, NOW, msg, sizeof msg);
    TEST_ASSERT_EQUAL_STRING("Gateway not in subnet", msg);
}

/* ---- the summary's Save: its hold and its lock ---- */

static void to_summary(void)
{
    const dui_ipset_t pre = {"192.168.8.50", "255.255.255.0", "192.168.8.1", "8.8.8.8"};
    open_ed(PID_M32, &pre);
    for (int i = 0; i < 20 && E.field != DUI_IPE_SUM; i++) TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E));
    TEST_ASSERT_EQUAL(DUI_IPE_SUM, E.field);
}

static void test_save_hold_and_lock(void)
{
    to_summary();
    dui_ipe_layout(&D, &E, NOW, &L);
    int c, r;
    TEST_ASSERT_TRUE(find(DUI_IPE_A_SAVE, 0, &c, &r));
    dui_tile_t t;
    dui_layout_key(&L, 0, c, r, &t);
    TEST_ASSERT_EQUAL(DUI_HOLD_ETH_MS, t.hold_ms);
    TEST_ASSERT_EQUAL_STRING("net resets", t.hint); /* "Hold 1.5 s" / "net resets" */
    TEST_ASSERT_FALSE(t.flags & DUI_F_LOCKED);
    TEST_ASSERT_TRUE(find(DUI_IPE_A_CANCEL, 0, &c, &r));
    TEST_ASSERT_TRUE(find(DUI_ACT_BACK, 0, &c, &r));
    /* let go early: no save, the hint */
    dui_ipe_out_t o = press(DUI_IPE_A_SAVE, DUI_HOLD_ETH_MS - 200);
    TEST_ASSERT_FALSE(o.save);
    TEST_ASSERT_TRUE(E.open);
    TEST_ASSERT_TRUE(E.hint_until > NOW);
    find(DUI_IPE_A_SAVE, 0, &c, &r);
    dui_layout_key(&L, 0, c, r, &t);
    dui_ipe_decorate(&E, c, r, NOW, &t);
    TEST_ASSERT_TRUE(t.flags & DUI_F_HINT);
    /* held 1.5 s: saved, closed, the confirm sound; what it saves */
    o = press(DUI_IPE_A_SAVE, DUI_HOLD_ETH_MS + 50);
    TEST_ASSERT_TRUE(o.save);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_EQUAL(DUI_SOUND_CONFIRM, o.sound);
    TEST_ASSERT_FALSE(E.open);
    dui_ipset_t v;
    values(&v);
    TEST_ASSERT_EQUAL_STRING("192.168.8.50", v.ip);
    TEST_ASSERT_EQUAL_STRING("255.255.255.0", v.mask);
    TEST_ASSERT_TRUE(dui_ipset_valid(&v)); /* what Eth.SetConfig takes */
    /* the deck locked meanwhile: Save shows a lock and never saves, however long it is held */
    to_summary();
    E.locked = true;
    dui_ipe_layout(&D, &E, NOW, &L);
    find(DUI_IPE_A_SAVE, 0, &c, &r);
    dui_layout_key(&L, 0, c, r, &t);
    TEST_ASSERT_TRUE(t.flags & DUI_F_LOCKED);
    o = press(DUI_IPE_A_SAVE, 3000);
    TEST_ASSERT_FALSE(o.save);
    TEST_ASSERT_TRUE(E.open);
    dui_ipe_layout(&D, &E, NOW, &L); /* a locked Save answers with the locked sound on its press */
    find(DUI_IPE_A_SAVE, 0, &c, &r);
    dui_ipe_key(&E, &L, c, r, true, NOW, &o);
    TEST_ASSERT_EQUAL(DUI_SOUND_LOCKED, o.sound);
    dui_ipe_key(&E, &L, c, r, false, NOW, &o);
    /* Save is a locked action: the worker refuses it without the PIN */
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_ETH_SAVE));
    TEST_ASSERT_FALSE(dui_action_allowed(DUI_A_ETH_SAVE, true, false));
    TEST_ASSERT_TRUE(dui_action_allowed(DUI_A_ETH_SAVE, true, true));
    /* an empty gateway saves as 0.0.0.0, an empty DNS as none */
    to_summary();
    E.q[1].set = E.q[2].set = 0;
    values(&v);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", v.gw);
    TEST_ASSERT_EQUAL_STRING("", v.dns);
    TEST_ASSERT_TRUE(dui_ipset_valid(&v));
}

/* ---- cancel ---- */

static void test_cancel_and_idle(void)
{
    open_ed(PID_M32, NULL);
    dui_ipe_out_t o = press(DUI_IPE_A_CANCEL, 0);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(o.save);
    TEST_ASSERT_FALSE(E.open);
    /* ⌫ held: cancel (the decks without ✕ too) */
    open_ed(PID_PLUS, NULL);
    o = press(DUI_IPE_A_DEL, DUI_PIN_CANCEL_HOLD_MS + 50);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(o.save);
    /* 60 s without a press: it cancels itself; the last press is what the screen's idle counts from */
    open_ed(PID_M32, NULL);
    press(DUI_IPE_A_DIGIT + 1, 0);
    const int64_t last = NOW - 20;
    dui_ipe_tick(&E, last + DUI_IPE_IDLE_MS - 1, &o);
    TEST_ASSERT_FALSE(o.closed);
    dui_ipe_tick(&E, last + DUI_IPE_IDLE_MS, &o);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(o.save);
    TEST_ASSERT_EQUAL(last, o.last_press);
    /* a held key never idles out; dui_ipe_cancel drops it */
    open_ed(PID_M32, NULL);
    dui_ipe_layout(&D, &E, NOW, &L);
    int c, r;
    find(DUI_IPE_A_DIGIT + 5, 0, &c, &r);
    dui_ipe_key(&E, &L, c, r, true, NOW, &o);
    dui_ipe_tick(&E, NOW + DUI_IPE_IDLE_MS + 10, &o);
    TEST_ASSERT_FALSE(o.closed);
    dui_ipe_cancel(&E);
    TEST_ASSERT_EQUAL(-1, E.held.col);
}

/* ---- the roller's repeat, the dial ---- */

static void test_roller_and_dial(void)
{
    open_ed(PID_MINI, NULL);
    at(DUI_IPE_A_DOWN, 0, 0, 0);
    at(DUI_IPE_A_UP, 0, 2, 0);
    at(DUI_IPE_A_NEXT, 0, 0, 1);
    at(DUI_IPE_A_DEL, 0, 1, 1);
    at(DUI_IPE_A_CANCEL, 0, 2, 1);
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 1, 0, &t));
    TEST_ASSERT_EQUAL(DUI_TILE_OCTETS, t.kind);
    TEST_ASSERT_EQUAL_STRING("IP 1/4", t.caption);
    press(DUI_IPE_A_UP, 0); /* empty: + gives 0 */
    TEST_ASSERT_EQUAL(0, E.q[0].v[0]);
    press(DUI_IPE_A_DOWN, 0); /* wraps to 255 */
    TEST_ASSERT_EQUAL(255, E.q[0].v[0]);
    /* held: repeats, faster after a while */
    E.q[0].v[0] = 0;
    press(DUI_IPE_A_UP, DUI_IPE_REPEAT_MS + 3 * DUI_IPE_EVERY_MS);
    const int slow = E.q[0].v[0];
    TEST_ASSERT_TRUE(slow >= 3 && slow <= 6);
    E.q[0].v[0] = 0;
    press(DUI_IPE_A_UP, DUI_IPE_FAST_MS + 4 * DUI_IPE_EVERY_MS);
    TEST_ASSERT_TRUE(E.q[0].v[0] > slow + 10); /* 5 at a time after 1.5 s */
    /* the +: a dial turn changes the current octet, a push is Next */
    open_ed(PID_PLUS, NULL);
    dui_ipe_out_t o;
    dui_ipe_dial_turn(&E, 3, NOW, &o);
    TEST_ASSERT_EQUAL(2, E.q[0].v[0]); /* from empty: 0, 1, 2 */
    dui_ipe_dial_press(&E, NOW, &o);
    TEST_ASSERT_EQUAL(1, E.oct);
    E.field = DUI_IPE_MASK;
    dui_ipe_dial_turn(&E, -4, NOW, &o);
    TEST_ASSERT_EQUAL(20, E.prefix);
}

/* ---- the layout per model ---- */

static void test_layouts(void)
{
    open_ed(PID_M32, NULL); /* the 4 octets on 4 keys of the top row; the phone pad, ⌫ / 0 / Next */
    at(DUI_IPE_A_CANCEL, 0, 0, 0);
    for (int i = 0; i < 4; i++) {
        dui_tile_t t;
        TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 2 + i, 0, &t));
        TEST_ASSERT_EQUAL(DUI_TILE_OCTETS, t.kind);
        TEST_ASSERT_EQUAL(1, t.num);
        TEST_ASSERT_EQUAL(i == 0 ? 0 : 0xFF, t.cur); /* the current one outlined */
    }
    at(DUI_IPE_A_DIGIT + 1, 0, 2, 1);
    at(DUI_IPE_A_DIGIT + 9, 0, 4, 3);
    at(DUI_IPE_A_DEL, 0, 5, 1);
    at(DUI_IPE_A_DIGIT + 0, 0, 5, 2);
    at(DUI_IPE_A_NEXT, 0, 5, 3);
    E.field = DUI_IPE_MASK; /* − and + in place of the digits */
    dui_ipe_layout(&D, &E, NOW, &L);
    int c, r;
    TEST_ASSERT_FALSE(find(DUI_IPE_A_DIGIT + 5, 0, &c, &r));
    TEST_ASSERT_TRUE(find(DUI_IPE_A_DOWN, 0, &c, &r));
    TEST_ASSERT_TRUE(find(DUI_IPE_A_UP, 0, &c, &r));

    open_ed(PID_MK2, NULL); /* ✕ ⌫, the value on 2 keys, Next; 1-5, 6-0 */
    at(DUI_IPE_A_CANCEL, 0, 0, 0);
    at(DUI_IPE_A_DEL, 0, 1, 0);
    at(DUI_IPE_A_NEXT, 0, 4, 0);
    typed("1921688");
    dui_ipe_layout(&D, &E, NOW, &L);
    dui_tile_t a, b;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 2, 0, &a));
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 3, 0, &b));
    TEST_ASSERT_EQUAL(DUI_TILE_OCTETS, a.kind);
    TEST_ASSERT_EQUAL_STRING("192.168.", a.text); /* two-octet pairs: the trailing dot on the first */
    TEST_ASSERT_EQUAL_STRING("8.", b.text);
    TEST_ASSERT_EQUAL_STRING("IP", a.caption);
    TEST_ASSERT_EQUAL(0xFF, a.cur);
    TEST_ASSERT_EQUAL(0, b.cur); /* octet 2, the first on the second key */
    at(DUI_IPE_A_DIGIT + 1, 0, 0, 1);
    at(DUI_IPE_A_DIGIT + 0, 0, 4, 2);

    open_ed(PID_PLUS, NULL); /* the digits on two pages, ⌫, Next, the page key */
    at(DUI_IPE_A_DIGIT + 1, 0, 0, 0);
    at(DUI_IPE_A_DIGIT + 6, 1, 0, 0);
    at(DUI_IPE_A_DEL, 0, 1, 1);
    at(DUI_IPE_A_NEXT, 0, 2, 1);
    at(DUI_IPE_A_PAGE, 0, 3, 1);
    TEST_ASSERT_EQUAL(2, L.pages);
    open_ed(PID_NEO, NULL);
    dui_ipe_out_t o;
    dui_ipe_touch(&E, 1, NOW, &o); /* the touch keys turn the page */
    TEST_ASSERT_EQUAL(1, E.pg % 2);
    open_ed(PID_PXL, NULL); /* ✕ and the phone pad; the value on the strip */
    at(DUI_IPE_A_CANCEL, 0, 0, 0);
    at(DUI_IPE_A_NEXT, 0, 6, 3);

    /* every model, every field and the summary: laid out without overflow */
    const uint16_t pids[] = {PID_M32, 0x006C, PID_PXL, PID_MK2, 0x0060, PID_PLUS, PID_NEO, PID_MINI, 0x00B8};
    for (size_t k = 0; k < sizeof pids / sizeof pids[0]; k++)
        for (int f = DUI_IPE_IP; f <= DUI_IPE_SUM; f++) {
            const dui_ipset_t pre = {"192.168.8.50", "255.255.255.0", "192.168.8.1", "8.8.8.8"};
            open_ed(pids[k], &pre);
            E.field = (uint8_t)f;
            dui_ipe_layout(&D, &E, NOW, &L);
            char msg[32];
            snprintf(msg, sizeof msg, "pid 0x%04x field %d", pids[k], f);
            TEST_ASSERT_FALSE_MESSAGE(L.overflow, msg);
            if (f != DUI_IPE_SUM) TEST_ASSERT_TRUE_MESSAGE(find(DUI_IPE_A_NEXT, 0, &c, &r), msg);
            else TEST_ASSERT_TRUE_MESSAGE(find(DUI_IPE_A_SAVE, 0, &c, &r) || find(DUI_IPE_A_SAVE, 1, &c, &r) ||
                                              find(DUI_IPE_A_SAVE, 2, &c, &r) || find(DUI_IPE_A_SAVE, 3, &c, &r),
                                          msg);
        }
}


/* ---- Host mode: the Companion's IP ---- */

static void open_host(uint16_t pid, const char *ip, uint16_t port)
{
    deck(pid);
    NOW = 5000;
    dui_ipe_out_t o;
    dui_ipe_open_host(&E, &D, ip, port, NOW, &o);
    TEST_ASSERT_TRUE(o.redraw);
    TEST_ASSERT_TRUE(E.open);
    TEST_ASSERT_EQUAL(DUI_IPE_HOST, E.kind);
    dui_ipe_layout(&D, &E, NOW, &L);
}

static const char *msg_now(void)
{
    static char m[48];
    dui_ipe_message(&E, NOW, m, sizeof m);
    return m;
}

/* The summary's tile captioned cap (its text), NULL none. */
static const char *sum_text(const char *cap)
{
    for (int i = 0; i < E.sum.ntiles; i++)
        if (E.sum.tile[i].caption && !strcmp(E.sum.tile[i].caption, cap)) return E.sum.tile[i].text;
    return NULL;
}

/* One field, "Companion IP": Next through its 4 octets, then the summary (no Mask, Gateway or DNS); ⌫ and Back on the
 * summary go back to the IP's last octet; the prefill: a dotted quad, else empty. */
static void test_host_one_field(void)
{
    open_host(PID_M32, "192.168.8.155", 16622);
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    TEST_ASSERT_EQUAL(0x0F, E.q[0].set);
    TEST_ASSERT_EQUAL(155, E.q[0].v[3]);
    TEST_ASSERT_EQUAL_STRING("Companion IP", msg_now());
    dui_tile_t t; /* the 32-key decks: the field's name on the status row, then the 4 octets */
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 1, 0, &t));
    TEST_ASSERT_EQUAL(DUI_TILE_WORD, t.kind);
    TEST_ASSERT_EQUAL_STRING("Companion IP", t.text);
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 2, 0, &t));
    TEST_ASSERT_EQUAL(DUI_TILE_OCTETS, t.kind);
    TEST_ASSERT_EQUAL_STRING("192", t.text);
    keys("NNN");
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    TEST_ASSERT_EQUAL(3, E.oct);
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E));
    TEST_ASSERT_EQUAL(DUI_IPE_SUM, E.field); /* straight to the summary */
    TEST_ASSERT_EQUAL_STRING("Save?", msg_now());
    dui_ipe_del(&E); /* ⌫ on the summary: the IP's last octet, as typed */
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    TEST_ASSERT_EQUAL(3, E.oct);
    TEST_ASSERT_EQUAL(0x0F, E.q[0].set);
    TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E));
    TEST_ASSERT_EQUAL(DUI_IPE_SUM, E.field);
    press(DUI_ACT_BACK, 0); /* the summary's Back: the same */
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    TEST_ASSERT_EQUAL(3, E.oct);
    TEST_ASSERT_TRUE(E.open);
    /* ⌫ at the IP's first octet, empty: nowhere further back (no field before it) */
    open_host(PID_M32, NULL, 0);
    keys("<<");
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    TEST_ASSERT_EQUAL(0, E.oct);
    /* the prefill: nothing, an empty string, a name, 0.0.0.0, a bad quad: empty */
    const char *none[] = {NULL, "", "companion.local", "0.0.0.0", "192.168.8", "300.1.1.1"};
    for (size_t i = 0; i < sizeof none / sizeof none[0]; i++) {
        open_host(PID_M32, none[i], 0);
        TEST_ASSERT_EQUAL_MESSAGE(0, E.q[0].set, none[i] ? none[i] : "NULL");
    }
    /* typed as on Ethernet's editor (the auto-advance and Next), then saved as typed */
    open_host(PID_M32, NULL, 0);
    keys("10N0N0N5N");
    TEST_ASSERT_EQUAL(DUI_IPE_SUM, E.field);
    dui_ipset_t v;
    values(&v);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", v.ip);
    /* the digit typed over a prefill replaces it */
    open_host(PID_M32, "192.168.8.155", 0);
    keys("172N");
    TEST_ASSERT_EQUAL(172, E.q[0].v[0]);
    TEST_ASSERT_EQUAL(168, E.q[0].v[1]);
}

/* Complete and a host's (not 0/8, 127/8, 224/3), with the existing errors and their words; no subnet (no mask): a
 * ".0" or ".255" address is fine. An error keeps the field, with the locked sound. */
static void test_host_checks(void)
{
    open_host(PID_M32, NULL, 0);
    keys("192168"); /* 192.168. typed: on the third octet, by an auto-advance */
    keys("N");      /* confirms the move */
    TEST_ASSERT_EQUAL(2, E.oct);
    keys("N"); /* the empty third octet: Next goes on to the fourth */
    TEST_ASSERT_EQUAL(3, E.oct);
    TEST_ASSERT_EQUAL(DUI_IPE_E_IP_PART, dui_ipe_next(&E));
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    static const struct {
        const char *ip;
        int err;
    } C[] = {{"127.0.0.1", DUI_IPE_E_RESERVED},  {"0.1.2.3", DUI_IPE_E_RESERVED}, {"224.0.0.1", DUI_IPE_E_RESERVED},
             {"255.255.255.255", DUI_IPE_E_RESERVED}, {"10.0.0.0", DUI_IPE_OK}, {"192.168.1.255", DUI_IPE_OK},
             {"223.255.255.254", DUI_IPE_OK},     {"1.0.0.1", DUI_IPE_OK}};
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        open_host(PID_M32, C[i].ip, 0);
        keys("NNN");
        TEST_ASSERT_EQUAL_MESSAGE(C[i].err, dui_ipe_next(&E), C[i].ip);
        TEST_ASSERT_EQUAL_MESSAGE(C[i].err ? DUI_IPE_IP : DUI_IPE_SUM, E.field, C[i].ip);
    }
    /* by its key: the error in the status, the locked sound, the field kept */
    open_host(PID_M32, "127.0.0.1", 0);
    keys("NNN");
    dui_ipe_out_t o = press(DUI_IPE_A_NEXT, 0);
    TEST_ASSERT_EQUAL(DUI_SOUND_LOCKED, o.sound);
    TEST_ASSERT_EQUAL(DUI_IPE_E_RESERVED, E.err);
    TEST_ASSERT_EQUAL_STRING("Not a host address", msg_now());
    TEST_ASSERT_EQUAL(DUI_IPE_IP, E.field);
    open_host(PID_M32, NULL, 0);
    keys("10N");
    TEST_ASSERT_TRUE(find(DUI_IPE_A_NEXT, 0, &(int){0}, &(int){0}));
    keys("NN");
    o = press(DUI_IPE_A_NEXT, 0);
    TEST_ASSERT_EQUAL(DUI_IPE_E_IP_PART, E.err);
    TEST_ASSERT_EQUAL_STRING("Fill all 4 octets", msg_now());
}

/* The summary: the IP, the port (read-only: companionPort, else 16622), Save (a 1.5 s hold, "reconnects", locked
 * while the deck is), Back and Cancel; Save held saves the IP (the arg of DUI_A_COMP_SAVE). */
static void test_host_summary_and_save(void)
{
    open_host(PID_M32, "192.168.8.155", 0);
    keys("NNNN");
    dui_ipe_layout(&D, &E, NOW, &L);
    TEST_ASSERT_TRUE(E.sum.sub);
    TEST_ASSERT_EQUAL_STRING("Companion", E.sum.status.title[0]); /* "Companion IP" */
    TEST_ASSERT_EQUAL_STRING("IP", E.sum.status.title[1]);
    TEST_ASSERT_EQUAL_STRING("192.168.8.155", sum_text("IP"));
    TEST_ASSERT_EQUAL_STRING("16622", sum_text("Port")); /* none set: the default */
    TEST_ASSERT_NULL(sum_text("Gateway"));
    int c, r;
    TEST_ASSERT_TRUE(find(DUI_IPE_A_SAVE, 0, &c, &r));
    dui_tile_t t;
    dui_layout_key(&L, 0, c, r, &t);
    TEST_ASSERT_EQUAL(DUI_HOLD_COMP_MS, t.hold_ms);
    TEST_ASSERT_EQUAL(1500, DUI_HOLD_COMP_MS);
    TEST_ASSERT_EQUAL_STRING("reconnects", t.hint); /* "Hold 1.5 s" / "reconnects" */
    char h[48];
    dui_hint_text(&t, h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("Hold 1.5 s \xC2\xB7 reconnects", h);
    TEST_ASSERT_EQUAL_STRING("Save", t.text);
    TEST_ASSERT_FALSE(t.flags & DUI_F_LOCKED);
    TEST_ASSERT_TRUE(find(DUI_IPE_A_CANCEL, 0, &c, &r));
    TEST_ASSERT_TRUE(find(DUI_ACT_BACK, 0, &c, &r));
    /* let go early: the hint, no save */
    dui_ipe_out_t o = press(DUI_IPE_A_SAVE, DUI_HOLD_COMP_MS - 200);
    TEST_ASSERT_FALSE(o.save);
    TEST_ASSERT_TRUE(E.open);
    TEST_ASSERT_TRUE(E.hint_until > NOW);
    /* held: saved, closed, the confirm sound; the IP */
    o = press(DUI_IPE_A_SAVE, DUI_HOLD_COMP_MS + 50);
    TEST_ASSERT_TRUE(o.save);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_EQUAL(DUI_SOUND_CONFIRM, o.sound);
    TEST_ASSERT_FALSE(E.open);
    dui_ipset_t v;
    values(&v);
    TEST_ASSERT_EQUAL_STRING("192.168.8.155", v.ip);
    TEST_ASSERT_TRUE(dui_ipv4_parse(v.ip, NULL)); /* what the worker takes */
    /* companionPort set: that one */
    open_host(PID_M32, "10.0.0.5", 16000);
    keys("NNNN");
    dui_ipe_layout(&D, &E, NOW, &L);
    TEST_ASSERT_EQUAL_STRING("16000", sum_text("Port"));
    /* the deck locked meanwhile: a lock, never saved */
    E.locked = true;
    dui_ipe_layout(&D, &E, NOW, &L);
    find(DUI_IPE_A_SAVE, 0, &c, &r);
    dui_layout_key(&L, 0, c, r, &t);
    TEST_ASSERT_TRUE(t.flags & DUI_F_LOCKED);
    o = press(DUI_IPE_A_SAVE, 3000);
    TEST_ASSERT_FALSE(o.save);
    TEST_ASSERT_TRUE(E.open);
    /* a locked action: the worker refuses it without the PIN */
    TEST_ASSERT_TRUE(dui_action_locked(DUI_A_COMP_SAVE));
    TEST_ASSERT_FALSE(dui_action_allowed(DUI_A_COMP_SAVE, true, false));
    TEST_ASSERT_TRUE(dui_action_allowed(DUI_A_COMP_SAVE, true, true));
    /* Cancel on the summary: closed, nothing saved */
    E.locked = false;
    o = press(DUI_IPE_A_CANCEL, 0);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(o.save);
}

/* ✕, ⌫ held and 60 s idle cancel it, as Ethernet's. */
static void test_host_cancel_and_idle(void)
{
    open_host(PID_M32, "192.168.8.155", 0);
    dui_ipe_out_t o = press(DUI_IPE_A_CANCEL, 0);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(o.save);
    open_host(PID_PLUS, "192.168.8.155", 0);
    o = press(DUI_IPE_A_DEL, DUI_PIN_CANCEL_HOLD_MS + 50);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(o.save);
    open_host(PID_M32, NULL, 0);
    dui_ipe_tick(&E, NOW + DUI_IPE_IDLE_MS - 1, &o);
    TEST_ASSERT_FALSE(o.closed);
    dui_ipe_tick(&E, NOW + DUI_IPE_IDLE_MS, &o);
    TEST_ASSERT_TRUE(o.closed);
    TEST_ASSERT_FALSE(o.save);
}

/* Every model: the field and the summary laid out (no overflow, every key's ink inside); the 15-key decks caption the
 * pair "IP", the roller "IP 1/4"; the strip / info bar decks show "Companion IP" there, a bar of its own (not Ethernet's
 * with the same octets). */
static void test_host_layouts(void)
{
    const uint16_t pids[] = {PID_M32, 0x006C, PID_PXL, PID_MK2, 0x0060, 0x00B9, PID_PLUS, PID_NEO, PID_MINI, 0x00B8};
    for (size_t k = 0; k < sizeof pids / sizeof pids[0]; k++)
        for (int sum = 0; sum < 2; sum++) {
            open_host(pids[k], "192.168.8.155", 16622);
            if (sum) keys("NNNN");
            dui_ipe_layout(&D, &E, NOW, &L);
            char msg[40];
            snprintf(msg, sizeof msg, "host pid 0x%04x %s", pids[k], sum ? "summary" : "ip");
            TEST_ASSERT_FALSE_MESSAGE(L.overflow, msg);
            fit_layout(&D, &L, msg);
            int c, r;
            bool save = false;
            for (int p = 0; p < L.pages; p++) save |= find(DUI_IPE_A_SAVE, p, &c, &r);
            TEST_ASSERT_TRUE_MESSAGE(sum ? save : find(DUI_IPE_A_NEXT, 0, &c, &r), msg);
        }
    open_host(PID_MK2, "192.168.8.155", 0);
    dui_tile_t t;
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 2, 0, &t));
    TEST_ASSERT_EQUAL_STRING("IP", t.caption);
    TEST_ASSERT_EQUAL_STRING("192.168.", t.text);
    open_host(PID_MINI, "192.168.8.155", 0);
    TEST_ASSERT_TRUE(dui_layout_key(&L, 0, 1, 0, &t));
    TEST_ASSERT_EQUAL_STRING("IP 1/4", t.caption);
    open_host(PID_PLUS, "192.168.8.155", 0);
    TEST_ASSERT_EQUAL_STRING("Companion IP", msg_now());
    const uint32_t hh = dui_ipe_bar_hash(&E, NOW);
    const dui_ipset_t pre = {"192.168.8.155", "255.255.255.0", "", ""};
    open_ed(PID_PLUS, &pre);
    TEST_ASSERT_EQUAL_STRING("IP address", msg_now()); /* Ethernet's, unchanged */
    TEST_ASSERT_NOT_EQUAL(hh, dui_ipe_bar_hash(&E, NOW));
    TEST_ASSERT_EQUAL(DUI_IPE_ETH, E.kind);
}

/* paging the summary back from its first page wraps to its last, on a 3-page summary too (the Neo with wide
 * addresses: two-octet pairs, a gateway and a DNS), and on the host mode's summary (2 pages on the Mini); forward wraps
 * to the first. */
static void test_summary_pages_wrap(void)
{
    const dui_ipset_t pre = {"222.222.222.222", "255.255.255.128", "222.222.222.200", "222.222.222.201"};
    open_ed(PID_NEO, &pre);
    for (int i = 0; i < 20 && E.field != DUI_IPE_SUM; i++) TEST_ASSERT_EQUAL(DUI_IPE_OK, dui_ipe_next(&E));
    TEST_ASSERT_EQUAL(DUI_IPE_SUM, E.field);
    dui_ipe_layout(&D, &E, NOW, &L);
    TEST_ASSERT_EQUAL(3, L.pages); /* the premise */
    dui_ipe_out_t o;
    dui_ipe_touch(&E, -1, NOW, &o); /* back from the first: the last */
    dui_ipe_layout(&D, &E, NOW, &L);
    TEST_ASSERT_EQUAL(2, E.pg % L.pages);
    dui_ipe_touch(&E, -1, NOW, &o);
    TEST_ASSERT_EQUAL(1, E.pg % L.pages);
    dui_ipe_touch(&E, 1, NOW, &o);
    dui_ipe_touch(&E, 1, NOW, &o); /* forward from the last: the first */
    TEST_ASSERT_EQUAL(0, E.pg % L.pages);
    dui_ipe_dial_turn(&E, -1, NOW, &o); /* a dial (the +) the same way */
    TEST_ASSERT_EQUAL(2, E.pg % L.pages);
    open_host(PID_MINI, "192.168.100.100", 16622); /* the host mode's summary */
    keys("NNNN");
    TEST_ASSERT_EQUAL(DUI_IPE_SUM, E.field);
    dui_ipe_layout(&D, &E, NOW, &L);
    TEST_ASSERT_EQUAL(2, L.pages);
    dui_ipe_touch(&E, -1, NOW, &o);
    dui_ipe_layout(&D, &E, NOW, &L);
    TEST_ASSERT_EQUAL(L.pages - 1, E.pg % L.pages);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_octets_auto_advance);
    RUN_TEST(test_prefilled_digit_replaces);
    RUN_TEST(test_next_after_advance_confirms);
    RUN_TEST(test_del_after_advance);
    RUN_TEST(test_del_across_octets);
    RUN_TEST(test_next_across_fields);
    RUN_TEST(test_mask_prefix);
    RUN_TEST(test_validation);
    RUN_TEST(test_save_hold_and_lock);
    RUN_TEST(test_cancel_and_idle);
    RUN_TEST(test_roller_and_dial);
    RUN_TEST(test_layouts);
    RUN_TEST(test_host_one_field);
    RUN_TEST(test_host_checks);
    RUN_TEST(test_host_summary_and_save);
    RUN_TEST(test_host_cancel_and_idle);
    RUN_TEST(test_host_layouts);
    RUN_TEST(test_summary_pages_wrap);
    return UNITY_END();
}
