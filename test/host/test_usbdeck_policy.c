#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "usbdeck_policy.h"

void setUp(void) {}
void tearDown(void) {}

#define S(x) ((int64_t)((x) * 1000000.0))
#define MINS(x) ((int64_t)(x) * 60 * 1000000LL)

static const upol_record_t *rec(const upol_t *p, const char *key)
{
    for (int i = 0; i < UPOL_RECORDS; i++)
        if (strcmp(p->rec[i].key, key) == 0) return &p->rec[i];
    return NULL;
}

/* ---- timing ---- */

static void test_interval_hs_fs(void)
{
    TEST_ASSERT_EQUAL_UINT32(125, upol_interval_us(true, 1));
    TEST_ASSERT_EQUAL_UINT32(1000, upol_interval_us(true, 4));
    TEST_ASSERT_EQUAL_UINT32(125u << 15, upol_interval_us(true, 16));
    TEST_ASSERT_EQUAL_UINT32(125u << 15, upol_interval_us(true, 200)); /* clamped, no UB shift */
    TEST_ASSERT_EQUAL_UINT32(125, upol_interval_us(true, 0));
    TEST_ASSERT_EQUAL_UINT32(1000, upol_interval_us(false, 1));
    TEST_ASSERT_EQUAL_UINT32(10000, upol_interval_us(false, 10));
    TEST_ASSERT_EQUAL_UINT32(1000, upol_interval_us(false, 0));
}

static void test_chunk_pkts_bounded(void)
{
    TEST_ASSERT_EQUAL_UINT16(32, upol_chunk_pkts(125));  /* HS: whole Module 32 report */
    TEST_ASSERT_EQUAL_UINT16(8, upol_chunk_pkts(1000));  /* FS: 8 ms per gated transfer */
    TEST_ASSERT_EQUAL_UINT16(1, upol_chunk_pkts(32000)); /* slow endpoint: one packet */
    TEST_ASSERT_EQUAL_UINT16(32, upol_chunk_pkts(0));
}

static void test_out_timeout_covers_healthy_stall(void)
{
    /* Healthy Module 32 stalls peak at ~223 ms: never late. */
    TEST_ASSERT_EQUAL_UINT32(500, upol_out_timeout_ms(2, 125));
    TEST_ASSERT_TRUE(upol_out_timeout_ms(2, 125) > 2 * 223);
    TEST_ASSERT_EQUAL_UINT32(516, upol_out_timeout_ms(8, 1000)); /* plus 2x the FS chunk's bus time */
}

static void test_backoff_doubles_and_caps(void)
{
    uint32_t b = upol_backoff_next_ms(0);
    TEST_ASSERT_EQUAL_UINT32(1000, b);
    b = upol_backoff_next_ms(b);
    TEST_ASSERT_EQUAL_UINT32(2000, b);
    b = upol_backoff_next_ms(upol_backoff_next_ms(b));
    TEST_ASSERT_EQUAL_UINT32(8000, b);
    TEST_ASSERT_EQUAL_UINT32(8000, upol_backoff_next_ms(b));
}

static void test_sweep_window(void)
{
    int64_t d = -1;
    TEST_ASSERT_TRUE(upol_sweep_due(S(10), S(10) - 10000, &d)); /* exactly one window */
    TEST_ASSERT_FALSE(upol_sweep_due(S(10), S(10) - 4000, &d));
    TEST_ASSERT_EQUAL_INT64(6000, d); /* the timer fires when the window ends */
    TEST_ASSERT_TRUE(upol_sweep_due(S(10), 0, &d));
}

/* ---- the OUT/IN gate ---- */

/* With the RX FIFO big enough for a 512-byte HS IN packet, an idle IN stays queued (NAKs are retried
 * inside the transfer) and completes only with data. One that completes while an OUT holds the gate
 * parks (the gate re-arms it after the OUT, upol_sweep_due); otherwise it is resubmitted at once. */
static void test_in_completion_parks_only_while_held(void)
{
    TEST_ASSERT_EQUAL_INT(UPOL_IN_RESUBMIT, upol_in_next(true, false, false));
    TEST_ASSERT_EQUAL_INT(UPOL_IN_PARK, upol_in_next(true, false, true));
    TEST_ASSERT_EQUAL_INT(UPOL_IN_STOP, upol_in_next(false, false, false)); /* error: the owner restarts it */
    TEST_ASSERT_EQUAL_INT(UPOL_IN_STOP, upol_in_next(false, false, true));
    TEST_ASSERT_EQUAL_INT(UPOL_IN_STOP, upol_in_next(true, true, false));   /* closing */
    TEST_ASSERT_EQUAL_INT(UPOL_IN_STOP, upol_in_next(true, true, true));
}

/* Close of a deck that left: its queued IN gets UPOL_IN_GONE_SETTLE_US to retire by itself (behind a
 * hub it errors out, and a halt racing that error never returns), then is halted (root port: the
 * port is invalid, so the halt doesn't wait), again every UPOL_IN_REHALT_US, until UPOL_IN_CLOSE_US. */
static void test_close_halts_a_gone_decks_in_after_it_settles(void)
{
    const int64_t settle = UPOL_IN_GONE_SETTLE_US;
    TEST_ASSERT_EQUAL_INT(UPOL_CLOSE_WAIT, upol_in_close(true, 0, -1));
    TEST_ASSERT_EQUAL_INT(UPOL_CLOSE_WAIT, upol_in_close(true, settle - 1, -1));
    TEST_ASSERT_EQUAL_INT(UPOL_CLOSE_HALT, upol_in_close(true, settle, -1));
    TEST_ASSERT_EQUAL_INT(UPOL_CLOSE_WAIT, upol_in_close(true, settle + 10000, settle));
    TEST_ASSERT_EQUAL_INT(UPOL_CLOSE_HALT, upol_in_close(true, settle + UPOL_IN_REHALT_US, settle));
    TEST_ASSERT_EQUAL_INT(UPOL_CLOSE_GIVE_UP, upol_in_close(true, UPOL_IN_CLOSE_US, settle));
}

/* A present deck's queued IN is never halted: IDF 5.5 can't halt a periodic channel whose device keeps
 * NAKing (its halt waits forever); the close gives up and leaks the interface instead. */
static void test_close_never_halts_a_present_decks_queued_in(void)
{
    for (int64_t t = 0; t < UPOL_IN_CLOSE_US; t += 10000)
        TEST_ASSERT_EQUAL_INT(UPOL_CLOSE_WAIT, upol_in_close(false, t, -1));
    TEST_ASSERT_EQUAL_INT(UPOL_CLOSE_GIVE_UP, upol_in_close(false, UPOL_IN_CLOSE_US, -1));
}

/* the hold check and the submit can't be one atomic step, so an IN submit decided just
 * before the hold may still be on its way into the HCD. out_begin waits for those (a few us), and only
 * for those: an idle queued IN never holds an OUT back. */
static void test_out_waits_for_a_submit_in_flight(void)
{
    TEST_ASSERT_FALSE(upol_out_may_start(0, 1));
    TEST_ASSERT_FALSE(upol_out_may_start(2, 1));
    TEST_ASSERT_FALSE(upol_out_may_start(0, 3));
}

static void test_queued_in_never_blocks_an_out(void)
{
    TEST_ASSERT_TRUE(upol_out_may_start(0, 0));
    TEST_ASSERT_TRUE(upol_out_may_start(1, 0));
    TEST_ASSERT_TRUE(upol_out_may_start(6, 0));
}

/* ---- key (I-1) ---- */

static void test_key_serial_or_stable_path(void)
{
    char a[UPOL_KEY_MAX], b[UPOL_KEY_MAX];
    upol_key(a, sizeof a, "AB1CD2345678EF", 0x0fd9, 0x00ba, "0");
    TEST_ASSERT_EQUAL_STRING("AB1CD2345678EF", a);
    /* No serial: VID/PID + port path. The USB address isn't an input, so a deck that comes back
     * with a new address after every reset keeps its key. */
    upol_key(a, sizeof a, NULL, 0x0fd9, 0x00ba, "0.3");
    upol_key(b, sizeof b, "", 0x0fd9, 0x00ba, "0.3");
    TEST_ASSERT_EQUAL_STRING("?0fd9:00ba@0.3", a);
    TEST_ASSERT_EQUAL_STRING(a, b);
    upol_key(b, sizeof b, NULL, 0x0fd9, 0x00ba, "0.4"); /* another port: another deck */
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
    upol_key(b, sizeof b, NULL, 0x0fd9, 0x00ba, "0.1.2.3.4.5.6.7.8.9.10.11.12"); /* bounded */
    TEST_ASSERT_TRUE(strlen(b) < UPOL_KEY_MAX);
}

/* I-1: a deck without a readable serial whose reset doesn't cure it ends up quarantined, though it
 * re-enumerates with a new USB address each time. */
static void test_unreadable_serial_deck_quarantined(void)
{
    upol_t p;
    upol_init(&p);
    char key[UPOL_KEY_MAX];
    upol_key(key, sizeof key, NULL, 0x0fd9, 0x00ba, "0"); /* attach as address 5 */
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, key, S(1)));
    TEST_ASSERT_FALSE(upol_note_reset(&p, key, UPOL_STRIKE_OUT, S(3)));
    upol_note_detach(&p, key, true, S(3.01));
    upol_key(key, sizeof key, NULL, 0x0fd9, 0x00ba, "0"); /* back as address 6: same key */
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, key, S(4)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, key, S(6))); /* stalls again: quarantining reset */
    TEST_ASSERT_TRUE(upol_note_reset(&p, key, UPOL_STRIKE_OUT, S(6)));
    upol_note_detach(&p, key, true, S(6.01));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, key, S(7)));
}

/* ---- the reset floor and the quarantine's bypass of it ---- */

static void test_reset_floor(void)
{
    upol_t p;
    upol_init(&p);
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "A", 0));
    upol_note_reset(&p, "A", UPOL_STRIKE_OUT, S(100));
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "B", S(100)));
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "B", S(109.9)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "B", S(110)));
    upol_note_reset(&p, "", UPOL_STRIKE_OUT, S(200)); /* unknown deck still counts for the floor */
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "B", S(205)));
}

/* Our reset -> re-attach -> stall again at once: the reset didn't cure it. That reset quarantines
 * the deck and may bypass the floor, 1 s after the previous one at the earliest. */
static void test_restall_on_probation_quarantines_past_floor(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_attach(&p, "X", S(0));
    TEST_ASSERT_FALSE(upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(100)));
    upol_note_detach(&p, "X", true, S(100.01));
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", S(101)));
    TEST_ASSERT_TRUE(upol_would_quarantine(&p, "X", S(103)));
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "X", S(100.5))); /* < 1 s gap */
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "X", S(101)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "X", S(103))); /* the old test refused this one */
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "Y", S(103))); /* another deck's first stall: floor */
    TEST_ASSERT_TRUE(upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(103))); /* report once */
    upol_note_detach(&p, "X", true, S(103.01));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", S(104)));
    /* Already quarantined: an IN stall goes through the floor and isn't a new quarantine. */
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "X", S(106)));
    TEST_ASSERT_FALSE(upol_note_reset(&p, "X", UPOL_STRIKE_IN, S(114)));
    TEST_ASSERT_EQUAL(UPOL_Q_ALL, upol_note_attach(&p, "X", S(115))); /* Q_WRITES -> Q_ALL */
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "Y", S(115)));
}

/* I-2 (a): enumeration elsewhere on the bus never holds a due reset back. A device that
 * re-enumerates every 1.5 s, forever, once the port has come up after our power-on. */
static void test_flapping_device_doesnt_delay_reset(void)
{
    upol_t p;
    upol_init(&p);
    const int64_t power_on = S(50);
    upol_note_reset(&p, "Z", UPOL_STRIKE_OUT, S(49.5)); /* the reset that power-on ended */
    /* upol_reset_allowed takes no enumeration input: the floor alone decides, however often a
     * device enumerates (the settle check after our power-on is test_root_settled_after_power_on). */
    for (int64_t t = S(50.4); t < S(60) + 25 * 60 * S(1); t += S(1.5)) {
        const int64_t now = t + S(0.7); /* the stuck deck asks between two NEW_DEVs */
        TEST_ASSERT_EQUAL(now - S(49.5) >= UPOL_RESET_MIN_INTERVAL_US, upol_reset_allowed(&p, "X", now));
    }
    /* The quarantining reset of a deck on probation goes 1 s after the previous one, flapping or not. */
    const int64_t t0 = S(2000);
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, t0);
    upol_note_detach(&p, "X", true, t0 + S(0.01));
    upol_note_attach(&p, "X", t0 + S(1)); /* power-on at t0 + 0.5 s, X's NEW_DEV at t0 + 0.9 s */
    TEST_ASSERT_TRUE(upol_root_settled(t0 + S(1.2), t0 + S(0.5), t0 + S(0.9)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "X", t0 + S(1.2)));
    /* Even with no NEW_DEV at all after our power-on, a reset waits at most UPOL_ROOT_SETTLE_US. */
    TEST_ASSERT_TRUE(UPOL_ROOT_SETTLE_US <= 5 * 1000000LL);
    TEST_ASSERT_TRUE(upol_root_settled(power_on + UPOL_ROOT_SETTLE_US, power_on, 0));
}

/* I-2 (b): IDF aborts (hub.c root_port_handle_events: dev_tree_node_dev_gone(NULL, 0) not found)
 * on a root-port power-off before the root device's connection was handled after a power-on. So
 * after our power-on: no power-off until a device finished enumerating (which needs that
 * connection handled), or UPOL_ROOT_SETTLE_US later. */
static void test_root_settled_after_power_on(void)
{
    const int64_t on = S(100);
    TEST_ASSERT_FALSE(upol_root_settled(on, on, 0));
    TEST_ASSERT_FALSE(upol_root_settled(S(100.3), on, 0));
    TEST_ASSERT_FALSE(upol_root_settled(S(100.3), on, S(99))); /* enumerated before: doesn't count */
    TEST_ASSERT_TRUE(upol_root_settled(S(100.4), on, S(100.38))); /* the root device's NEW_DEV */
    TEST_ASSERT_TRUE(upol_root_settled(S(120), on, S(100.38)));
    TEST_ASSERT_FALSE(upol_root_settled(on + UPOL_ROOT_SETTLE_US - 1, on, 0));
    TEST_ASSERT_TRUE(upol_root_settled(on + UPOL_ROOT_SETTLE_US, on, 0)); /* bounded */
    TEST_ASSERT_TRUE(upol_root_settled(S(7), 0, 0)); /* never powered by us yet: nothing to wait for */
}

static void test_no_bypass_for_first_old_or_empty(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_reset(&p, "Z", UPOL_STRIKE_OUT, S(0)); /* some reset, sets the floor */
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(100));
    upol_note_attach(&p, "X", S(101));
    /* Probation over: an independent later stall is a plain reset again, through the floor. */
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", S(101) + UPOL_PROBATION_US));
    upol_note_reset(&p, "W", UPOL_STRIKE_OUT, S(101) + UPOL_PROBATION_US - S(5));
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "X", S(101) + UPOL_PROBATION_US + S(1)));
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "", S(101) + UPOL_PROBATION_US + S(1)));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "", S(102)));
}

/* The r1_late case. Stalls cured by the reset, the next one after a replug or long after
 * the re-attach: no quarantine. */
static void test_cured_stalls_not_quarantined(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_attach(&p, "M32", S(308.6));
    TEST_ASSERT_FALSE(upol_note_reset(&p, "M32", UPOL_STRIKE_OUT, S(314.15)));
    upol_note_detach(&p, "M32", true, S(314.16));
    upol_note_attach(&p, "M32", S(315.03)); /* probation until 345 s */
    upol_note_detach(&p, "M32", false, S(328.2)); /* a power cycle that wasn't our reset */
    upol_note_attach(&p, "M32", S(328.58));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "M32", S(335.23)));
    TEST_ASSERT_FALSE(upol_note_reset(&p, "M32", UPOL_STRIKE_OUT, S(335.23)));
    upol_note_detach(&p, "M32", true, S(335.24));
    upol_note_attach(&p, "M32", S(336.11));
    /* Stalls again 40 s after that re-attach: a new stall, not an uncured one. */
    TEST_ASSERT_FALSE(upol_note_reset(&p, "M32", UPOL_STRIKE_OUT, S(376.2)));
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "M32", S(377)));
}

/* ...but the 4th reset within 10 min quarantines whatever the timing. */
static void test_fourth_reset_in_window_quarantines(void)
{
    upol_t p;
    upol_init(&p);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(upol_note_reset(&p, "F", UPOL_STRIKE_OUT, S(100 * i)));
        upol_note_detach(&p, "F", true, S(100 * i + 0.01));
        upol_note_attach(&p, "F", S(100 * i + 1));
    }
    TEST_ASSERT_TRUE(upol_would_quarantine(&p, "F", S(250)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "F", S(250))); /* past the floor anyway */
    TEST_ASSERT_TRUE(upol_note_reset(&p, "F", UPOL_STRIKE_OUT, S(250)));
    /* Spread over more than the window: never 4 live ones. */
    upol_init(&p);
    for (int i = 0; i < 8; i++) TEST_ASSERT_FALSE(upol_note_reset(&p, "G", UPOL_STRIKE_OUT, S(250 * i)));
}

/* ---- replug and quarantine lifetime (n1) ---- */

static void test_replug_soon_after_reset_lifts(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(10));
    upol_note_attach(&p, "X", S(11));
    TEST_ASSERT_TRUE(upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(13)));
    upol_note_detach(&p, "X", true, S(13.01)); /* ours: kept */
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", S(14)));
    upol_note_detach(&p, "X", false, S(18)); /* user replugs 5 s after the reset: lifted */
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", S(19)));
    TEST_ASSERT_FALSE(upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(40))); /* afresh */
}

/* Quarantines `key`'s deck: a reset at t, its re-attach, a stall during the probation at t + 2 s
 * (the quarantining reset), its re-attach at t + 3 s. */
static void quarantine(upol_t *p, const char *key, int64_t t)
{
    upol_note_reset(p, key, UPOL_STRIKE_OUT, t);
    upol_note_detach(p, key, true, t + S(0.01));
    upol_note_attach(p, key, t + S(1));
    TEST_ASSERT_TRUE(upol_note_reset(p, key, UPOL_STRIKE_OUT, t + S(2)));
    upol_note_detach(p, key, true, t + S(2.01));
    TEST_ASSERT_TRUE(upol_note_attach(p, key, t + S(3)));
}

/* The lifted deck's first report stalls at `t`: the quarantining reset, then its re-attach. Returns
 * the new quarantine's end. */
static int64_t fail_retry(upol_t *p, const char *key, int64_t t)
{
    TEST_ASSERT_TRUE(upol_would_quarantine(p, key, t));
    TEST_ASSERT_TRUE(upol_reset_allowed(p, key, t));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_reset(p, key, UPOL_STRIKE_OUT, t));
    upol_note_detach(p, key, true, t + S(0.01));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(p, key, t + S(1)));
    return upol_quarantine_end_us(p, key);
}

/* ---- A quarantine retries by itself ---- */

static void test_quarantine_len_doubles_and_caps(void)
{
    TEST_ASSERT_EQUAL_INT64(MINS(10), UPOL_QUARANTINE_MIN_US);
    TEST_ASSERT_EQUAL_INT64(MINS(60), UPOL_QUARANTINE_MAX_US);
    TEST_ASSERT_EQUAL_INT64(MINS(10), upol_quarantine_len_us(0));
    TEST_ASSERT_EQUAL_INT64(MINS(10), upol_quarantine_len_us(1));
    TEST_ASSERT_EQUAL_INT64(MINS(20), upol_quarantine_len_us(2));
    TEST_ASSERT_EQUAL_INT64(MINS(40), upol_quarantine_len_us(3));
    TEST_ASSERT_EQUAL_INT64(MINS(60), upol_quarantine_len_us(4));
    TEST_ASSERT_EQUAL_INT64(MINS(60), upol_quarantine_len_us(5));
    TEST_ASSERT_EQUAL_INT64(MINS(60), upol_quarantine_len_us(64)); /* no overflowing shift */
    TEST_ASSERT_EQUAL_INT64(MINS(60), upol_quarantine_len_us(255));
}

/* With the deck still plugged in, the quarantine ends after 10 min: one retry, on probation. */
static void test_quarantine_lifts_while_attached(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "X", S(100)); /* quarantined at 102 s */
    const int64_t end = upol_quarantine_end_us(&p, "X");
    TEST_ASSERT_EQUAL_INT64(S(102) + MINS(10), end);
    TEST_ASSERT_FALSE(upol_note_lift(&p, "X", end - 1));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
    TEST_ASSERT_EQUAL_INT64(0, upol_quarantine_end_us(&p, "X"));
    int decks, resets, q;
    upol_summary(&p, &decks, &resets, &q);
    TEST_ASSERT_EQUAL(0, q);
    /* Asking again doesn't start the probation again. */
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end + S(10)));
    TEST_ASSERT_TRUE(upol_would_quarantine(&p, "X", end + UPOL_PROBATION_US - 1));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", end + UPOL_PROBATION_US));
    /* The 1 s spacing holds for that retry: its quarantining reset may go 1 s after another deck's reset. */
    upol_note_reset(&p, "Y", UPOL_STRIKE_OUT, end + S(2));
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "X", end + S(2.5)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "X", end + S(3)));
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "Z", end + S(3))); /* a deck not on probation: floor */
}

/* The lift keeps the record's resets and its streak, and arms the probation. */
static void test_timed_lift_keeps_history_and_arms_probation(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "X", S(100));
    const upol_record_t *r = rec(&p, "X");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(2, r->nresets);
    TEST_ASSERT_EQUAL(1, r->nquar);
    const int64_t end = upol_quarantine_end_us(&p, "X");
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", end - 1)); /* quarantined: nothing to test */
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
    TEST_ASSERT_EQUAL(2, r->nresets);
    TEST_ASSERT_EQUAL_INT64(S(100), r->reset_us[0]);
    TEST_ASSERT_EQUAL_INT64(S(102), r->reset_us[1]);
    TEST_ASSERT_EQUAL(1, r->nquar);
    TEST_ASSERT_FALSE(r->quarantined);
    TEST_ASSERT_EQUAL_INT64(end + UPOL_PROBATION_US, r->probation_until_us);
    TEST_ASSERT_TRUE(upol_would_quarantine(&p, "X", end + S(1)));
}

/* Each retry that stalls during its probation quarantines again, twice as long: 20, 40, 60, 60 min.
 * Time spent quarantined isn't quiet time: a deck that fails every retry stays at the cap. */
static void test_failed_retry_doubles(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "X", S(100));
    int64_t end = upol_quarantine_end_us(&p, "X");
    const int64_t len[] = {MINS(20), MINS(40), MINS(60), MINS(60), MINS(60)};
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
        const int64_t stall = end + S(2); /* the flush report after the lift */
        const int64_t next = fail_retry(&p, "X", stall);
        TEST_ASSERT_EQUAL_INT64(stall + len[i], next);
        TEST_ASSERT_FALSE(upol_note_lift(&p, "X", next - 1));
        end = next;
    }
    TEST_ASSERT_EQUAL(6, rec(&p, "X")->nquar);
}

/* A retry that runs 30 s without a stall passed: a later stall is a plain reset through the floor
 * and leaves the streak alone; 4 resets within 10 min still quarantine, at the next length. */
static void test_passed_retry(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "X", S(100));
    const int64_t end = upol_quarantine_end_us(&p, "X");
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", end + UPOL_PROBATION_US));
    upol_note_reset(&p, "Y", UPOL_STRIKE_OUT, end + S(40)); /* another deck's reset */
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "X", end + S(45)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "X", end + S(50)));
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_reset(&p, "X", UPOL_STRIKE_OUT, end + S(50)));
    TEST_ASSERT_EQUAL(1, rec(&p, "X")->nquar);
    for (int i = 1; i <= 3; i++) { /* cured stalls, each well after the probation of the last */
        upol_note_detach(&p, "X", true, end + S(50 + 100 * (i - 1)) + S(0.01));
        upol_note_attach(&p, "X", end + S(51 + 100 * (i - 1)));
        const upol_quarantine_t q = upol_note_reset(&p, "X", UPOL_STRIKE_OUT, end + S(50 + 100 * i));
        TEST_ASSERT_EQUAL(i < 3 ? UPOL_Q_NONE : UPOL_Q_WRITES, q); /* the 4th within 10 min */
    }
    TEST_ASSERT_EQUAL_INT64(end + S(350) + MINS(20), upol_quarantine_end_us(&p, "X"));
}

/* the streak goes back to 10 min after an hour with no new strike. The quiet hour counts from
 * the later of the last strike and the lift (a quarantined deck can't strike). */
static void test_streak_resets_after_quiet_hour(void)
{
    upol_t p;
    /* Just under an hour after the lift: still the streak, 20 min. */
    upol_init(&p);
    quarantine(&p, "X", S(100));
    int64_t lift = upol_quarantine_end_us(&p, "X");
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", lift));
    int64_t t = lift + MINS(60) - 1;
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_reset(&p, "X", UPOL_STRIKE_OUT, t)); /* plain, cured... */
    upol_note_detach(&p, "X", true, t + S(0.01));
    upol_note_attach(&p, "X", t + S(1));
    TEST_ASSERT_EQUAL_INT64(t + S(2) + MINS(20), fail_retry(&p, "X", t + S(2))); /* ...or not */
    /* An hour after the lift: the streak is forgotten at that first strike, 10 min. */
    upol_init(&p);
    quarantine(&p, "X", S(100));
    lift = upol_quarantine_end_us(&p, "X");
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", lift));
    t = lift + MINS(60);
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_reset(&p, "X", UPOL_STRIKE_OUT, t));
    TEST_ASSERT_EQUAL(0, rec(&p, "X")->nquar);
    upol_note_detach(&p, "X", true, t + S(0.01));
    upol_note_attach(&p, "X", t + S(1));
    TEST_ASSERT_EQUAL_INT64(t + S(2) + MINS(10), fail_retry(&p, "X", t + S(2)));
    /* A strike restarts the quiet hour: one 30 min after the lift keeps the streak 89 min after it. */
    upol_init(&p);
    quarantine(&p, "X", S(100));
    lift = upol_quarantine_end_us(&p, "X");
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", lift));
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_reset(&p, "X", UPOL_STRIKE_OUT, lift + MINS(30)));
    upol_note_detach(&p, "X", true, lift + MINS(30) + S(0.01));
    upol_note_attach(&p, "X", lift + MINS(30) + S(1));
    t = lift + MINS(89);
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_reset(&p, "X", UPOL_STRIKE_OUT, t));
    upol_note_detach(&p, "X", true, t + S(0.01));
    upol_note_attach(&p, "X", t + S(1));
    TEST_ASSERT_EQUAL_INT64(t + S(2) + MINS(20), fail_retry(&p, "X", t + S(2)));
}

/* A deck our reset took off during its quarantine that comes back after the end: that attach is the
 * retry (lifted, on probation, resets and streak kept), but only when upol_note_lift would lift it:
 * not before the end, and not while another retry is running (a reset within 10 s, another deck on
 * probation, or another deck's re-attach after our reset pending). */
static void test_attach_after_expiry_is_the_retry(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "X", S(100));
    const int64_t end = upol_quarantine_end_us(&p, "X");
    upol_note_reset(&p, "Y", UPOL_STRIKE_OUT, end - S(60)); /* Y's reset takes X off too */
    upol_note_detach(&p, "X", true, end - S(59.99));
    upol_note_detach(&p, "Y", true, end - S(59.99));
    upol_note_attach(&p, "Y", end - S(59)); /* Y's probation ends at end - 29 s */
    /* Nothing else running, so only the end holds it: still quarantined 1 us before. */
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", end - 1));
    upol_note_detach(&p, "X", true, end - S(0.5)); /* (one more bounce, for the test) */
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", end + S(1)));
    TEST_ASSERT_EQUAL_INT64(0, upol_quarantine_end_us(&p, "X"));
    TEST_ASSERT_TRUE(upol_would_quarantine(&p, "X", end + S(1) + UPOL_PROBATION_US - 1));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", end + S(1) + UPOL_PROBATION_US));
    TEST_ASSERT_EQUAL(1, rec(&p, "X")->nquar);
    TEST_ASSERT_EQUAL(2, rec(&p, "X")->nresets);
    TEST_ASSERT_EQUAL_INT64(end + S(3) + MINS(20), fail_retry(&p, "X", end + S(3)));

    /* Within 10 s of a reset: still quarantined; the deck task's lift follows once the port is
     * quiet and the culprit's probation is over. */
    upol_init(&p);
    quarantine(&p, "X", S(100));
    const int64_t end2 = upol_quarantine_end_us(&p, "X");
    upol_note_reset(&p, "Y", UPOL_STRIKE_OUT, end2 + S(5));
    upol_note_detach(&p, "X", true, end2 + S(5.01));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", end2 + S(7)));
    upol_note_attach(&p, "Y", end2 + S(8)); /* Y's probation: until end2 + 38 s */
    TEST_ASSERT_FALSE(upol_note_lift(&p, "X", end2 + S(37)));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end2 + S(38)));

    /* Port quiet for 30 s, but another deck on probation: still quarantined, until it ends. */
    upol_init(&p);
    quarantine(&p, "X", S(100));
    const int64_t end3 = upol_quarantine_end_us(&p, "X");
    upol_note_reset(&p, "Y", UPOL_STRIKE_OUT, end3 - S(30));
    upol_note_detach(&p, "X", true, end3 - S(29.99));
    upol_note_attach(&p, "Y", end3 - S(5)); /* Y's probation: until end3 + 25 s */
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", end3 + S(1)));
    upol_note_detach(&p, "X", true, end3 + S(2)); /* (bounce) */
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", end3 + S(25) - 1));
    upol_note_detach(&p, "X", true, end3 + S(25) - 1);
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", end3 + S(25)));

    /* Port quiet for 16 s, but the culprit's re-attach still pending: still quarantined. */
    upol_init(&p);
    quarantine(&p, "X", S(100));
    const int64_t end4 = upol_quarantine_end_us(&p, "X");
    upol_note_reset(&p, "Y", UPOL_STRIKE_OUT, end4 - S(15)); /* Y not back yet: until end4 + 15 s */
    upol_note_detach(&p, "X", true, end4 - S(14.99));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", end4 + S(1)));
    TEST_ASSERT_FALSE(upol_note_lift(&p, "X", end4 + S(15) - 1));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end4 + S(15)));
}

/* nquar saturates at 255; the length stays at the cap. */
static void test_nquar_saturates(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "X", S(100));
    int64_t end = upol_quarantine_end_us(&p, "X");
    for (int i = 0; i < 260; i++) {
        TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
        const int64_t stall = end + S(2);
        end = fail_retry(&p, "X", stall);
        if (i >= 2) TEST_ASSERT_EQUAL_INT64(stall + MINS(60), end); /* nquar 4 and up */
    }
    TEST_ASSERT_EQUAL(255, rec(&p, "X")->nquar);
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
    TEST_ASSERT_EQUAL_INT64(end + S(2) + MINS(60), fail_retry(&p, "X", end + S(2)));
    TEST_ASSERT_EQUAL(255, rec(&p, "X")->nquar);
}

/* The quiet hour only runs while the deck is not quarantined: a strike that comes an hour after the
 * last one but while the deck is still quarantined (past its end, the lift held back) keeps the
 * streak. Here an IN strike on a Q_WRITES deck at the 60-min cap. */
static void test_streak_kept_while_quarantined(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "X", S(100));
    int64_t end = upol_quarantine_end_us(&p, "X");
    for (int i = 0; i < 3; i++) { /* 20, 40, then 60 min */
        TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
        end = fail_retry(&p, "X", end + S(2));
    }
    TEST_ASSERT_EQUAL(4, rec(&p, "X")->nquar);
    const int64_t strike = end + S(60); /* 61 min after the last strike, still quarantined */
    TEST_ASSERT_TRUE(strike - rec(&p, "X")->streak_us >= UPOL_STREAK_RESET_US);
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_reset(&p, "X", UPOL_STRIKE_IN, strike)); /* not a new one */
    TEST_ASSERT_EQUAL(4, rec(&p, "X")->nquar);
    upol_note_detach(&p, "X", true, strike + S(0.01));
    TEST_ASSERT_EQUAL(UPOL_Q_ALL, upol_note_attach(&p, "X", strike + S(1)));
    const int64_t lift = strike + UPOL_LIFT_QUIET_US;
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", lift));
    TEST_ASSERT_EQUAL(UPOL_Q_ALL, upol_note_reset(&p, "X", UPOL_STRIKE_OUT, lift + S(2))); /* retry fails */
    TEST_ASSERT_EQUAL(5, rec(&p, "X")->nquar);
    TEST_ASSERT_EQUAL_INT64(lift + S(2) + MINS(60), upol_quarantine_end_us(&p, "X"));
}

/* One retry at a time, box-wide: none while another deck is on probation, about to start one (the
 * re-attach after our reset is pending), or within UPOL_LIFT_QUIET_US of a bus reset. */
static void test_one_lift_at_a_time(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "A", S(100));
    quarantine(&p, "B", S(110));
    const int64_t t = upol_quarantine_end_us(&p, "B") + S(5); /* both due */
    TEST_ASSERT_TRUE(t >= upol_quarantine_end_us(&p, "A"));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "A", t));
    TEST_ASSERT_FALSE(upol_note_lift(&p, "B", t));
    TEST_ASSERT_FALSE(upol_note_lift(&p, "B", t + UPOL_PROBATION_US - 1));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "B", t + UPOL_PROBATION_US)); /* A passed: B's turn */
    /* A bus reset holds lifts back for UPOL_LIFT_QUIET_US, and its culprit's re-attach for as long
     * as it is pending (UPOL_REATTACH_US at most), then its probation. */
    TEST_ASSERT_EQUAL_INT64(S(10), UPOL_LIFT_QUIET_US);
    upol_init(&p);
    quarantine(&p, "C", S(100));
    const int64_t r = upol_quarantine_end_us(&p, "C") + S(100);
    upol_note_reset(&p, "D", UPOL_STRIKE_OUT, r);
    upol_note_detach(&p, "D", true, r + S(0.01));
    TEST_ASSERT_FALSE(upol_note_lift(&p, "C", r + UPOL_LIFT_QUIET_US - 1));
    TEST_ASSERT_FALSE(upol_note_lift(&p, "C", r + UPOL_REATTACH_US - 1)); /* D not back yet */
    TEST_ASSERT_TRUE(upol_note_lift(&p, "C", r + UPOL_REATTACH_US));     /* D never came back */
    upol_init(&p);
    quarantine(&p, "C", S(100));
    upol_note_reset(&p, "D", UPOL_STRIKE_OUT, r);
    upol_note_detach(&p, "D", true, r + S(0.01));
    upol_note_attach(&p, "D", r + S(12)); /* D on probation until r + 42 s */
    TEST_ASSERT_FALSE(upol_note_lift(&p, "C", r + S(41)));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "C", r + S(42)));
    /* Two decks quarantined before a restart both end at boot + 10 min: one retries, then the other. */
    upol_init(&p);
    quarantine(&p, "A", S(5000));
    quarantine(&p, "B", S(5100));
    upol_after_restart(&p);
    TEST_ASSERT_EQUAL_INT64(MINS(10), upol_quarantine_end_us(&p, "A"));
    TEST_ASSERT_EQUAL_INT64(MINS(10), upol_quarantine_end_us(&p, "B"));
    upol_note_attach(&p, "A", S(6.5));
    upol_note_attach(&p, "B", S(6.7));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "B", MINS(10)));
    TEST_ASSERT_FALSE(upol_note_lift(&p, "A", MINS(10)));
    /* B fails: its quarantining reset holds A back for UPOL_LIFT_QUIET_US more. */
    fail_retry(&p, "B", MINS(10) + S(2));
    TEST_ASSERT_FALSE(upol_note_lift(&p, "A", MINS(10) + S(2) + UPOL_LIFT_QUIET_US - 1));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "A", MINS(10) + S(2) + UPOL_LIFT_QUIET_US));
}

/* After a restart the period starts again at boot: never shorter than what was left, longer by at
 * most one period. The streak survives. */
static void test_restart_rebases_quarantine_end(void)
{
    for (int k = 0; k <= 4; k++) { /* restart k/4 of the way into a 40-min quarantine */
        upol_t p;
        upol_init(&p);
        quarantine(&p, "X", S(100));
        int64_t end = upol_quarantine_end_us(&p, "X");
        TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
        end = fail_retry(&p, "X", end + S(2));
        TEST_ASSERT_TRUE(upol_note_lift(&p, "X", end));
        end = fail_retry(&p, "X", end + S(2)); /* the 3rd: 40 min */
        const int64_t start = end - MINS(40), restart_at = start + k * MINS(10);
        const int64_t left = end - restart_at;
        upol_after_restart(&p); /* the clock starts again at 0 */
        const int64_t rebased = upol_quarantine_end_us(&p, "X");
        TEST_ASSERT_EQUAL_INT64(MINS(40), rebased);
        TEST_ASSERT_TRUE(rebased >= left);
        TEST_ASSERT_TRUE(rebased <= left + MINS(40));
        TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", S(6.7)));
        TEST_ASSERT_FALSE(upol_note_lift(&p, "X", rebased - 1));
        TEST_ASSERT_TRUE(upol_note_lift(&p, "X", rebased));
        TEST_ASSERT_EQUAL_INT64(rebased + S(2) + MINS(60), fail_retry(&p, "X", rebased + S(2)));
    }
}

/* A replug (or the deck dropping off by itself) lifts at once, without probation; a serial key
 * keeps its streak (p2), a port-path key is dropped with its record (p3). */
static void test_foreign_detach_keeps_streak(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "X", S(100));
    upol_note_detach(&p, "X", false, S(200));
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", S(201)));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", S(203)));
    TEST_ASSERT_EQUAL(1, rec(&p, "X")->nquar);
    quarantine(&p, "X", S(300));
    TEST_ASSERT_EQUAL_INT64(S(302) + MINS(20), upol_quarantine_end_us(&p, "X"));
    char k[UPOL_KEY_MAX];
    upol_key(k, sizeof k, NULL, 0x0fd9, 0x00ba, "0.2");
    quarantine(&p, k, S(400));
    upol_note_detach(&p, k, false, S(500));
    TEST_ASSERT_NULL(rec(&p, k));
    quarantine(&p, k, S(600));
    TEST_ASSERT_EQUAL_INT64(S(602) + MINS(10), upol_quarantine_end_us(&p, k));
}

/* A record evicted while its deck is quarantined holds nothing back any more. */
static void test_lift_without_record(void)
{
    upol_t p;
    upol_init(&p);
    TEST_ASSERT_EQUAL_INT64(0, upol_quarantine_end_us(&p, "X"));
    TEST_ASSERT_TRUE(upol_note_lift(&p, "X", S(1)));
}

static void test_records_evict_lru_but_keep_quarantined(void)
{
    upol_t p;
    upol_init(&p);
    quarantine(&p, "Q", S(0));
    char k[8];
    for (int i = 0; i < 3 * UPOL_RECORDS; i++) { /* many other decks, one reset each */
        k[0] = 'a' + (char)(i % 26);
        k[1] = 'a' + (char)(i / 26);
        k[2] = 0;
        upol_note_reset(&p, k, UPOL_STRIKE_OUT, S(100 + 20 * i));
    }
    TEST_ASSERT_EQUAL_INT64(S(2) + MINS(10), upol_quarantine_end_us(&p, "Q"));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "Q", S(590))); /* before its end */
}

static void test_all_quarantined_evicts_oldest(void)
{
    upol_t p;
    upol_init(&p);
    char k[4] = "q0";
    for (int i = 0; i < UPOL_RECORDS; i++) {
        k[1] = (char)('0' + i);
        quarantine(&p, k, S(10 * i));
    }
    upol_note_attach(&p, "q0", S(200)); /* q0 used recently: q1 is now the oldest */
    upol_note_reset(&p, "new", UPOL_STRIKE_OUT, S(300));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "q0", S(301)));
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "q1", S(301)));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "q2", S(301)));
}

/* p1: the re-attach that starts a probation must follow our reset closely. A deck that didn't
 * come back after our reset and is replugged hours later starts afresh. */
static void test_late_attach_after_reset_no_probation(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_attach(&p, "X", S(10));
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(100)); /* X doesn't come back after power-on */
    upol_note_detach(&p, "X", true, S(100.01));
    const int64_t later = S(100) + 2 * 3600 * S(1);
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", later));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", later + S(2)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "X", later + S(2))); /* a plain reset: the floor is long past */
    TEST_ASSERT_FALSE(upol_note_reset(&p, "X", UPOL_STRIKE_OUT, later + S(2)));
    /* Just inside the bound, it still is the re-attach after our reset. */
    upol_init(&p);
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(100));
    upol_note_attach(&p, "X", S(100) + UPOL_REATTACH_US - 1);
    TEST_ASSERT_TRUE(upol_would_quarantine(&p, "X", S(100) + UPOL_REATTACH_US + S(1)));
    upol_init(&p);
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(100));
    upol_note_attach(&p, "X", S(100) + UPOL_REATTACH_US);
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", S(100) + UPOL_REATTACH_US + S(1)));
}

/* p2: a replug (or the deck dropping off by itself) lifts the quarantine but not the reset count,
 * so the 4-in-10-min backstop holds across quarantine episodes. */
static void test_lift_keeps_reset_history(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(10));
    upol_note_detach(&p, "X", true, S(10.01));
    upol_note_attach(&p, "X", S(11));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(13))); /* uncured */
    upol_note_detach(&p, "X", true, S(13.01));
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", S(14)));
    upol_note_detach(&p, "X", false, S(60)); /* brown-out, or a replug */
    int decks, resets, q;
    upol_summary(&p, &decks, &resets, &q);
    TEST_ASSERT_EQUAL(1, decks);
    TEST_ASSERT_EQUAL(2, resets);
    TEST_ASSERT_EQUAL(0, q);
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", S(61))); /* lifted */
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", S(63))); /* no probation after a replug */
    TEST_ASSERT_FALSE(upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(100))); /* 3rd */
    upol_note_detach(&p, "X", true, S(100.01));
    upol_note_attach(&p, "X", S(101));
    TEST_ASSERT_TRUE(upol_would_quarantine(&p, "X", S(200))); /* the 4th within 10 min */
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(200)));
}

/* p3: a port-path key names a socket, not a deck: a detach we didn't cause (a swap at that socket)
 * drops the record, so the next deck there inherits nothing. Serial keys keep theirs (p2). */
static void test_path_key_dropped_on_foreign_detach(void)
{
    upol_t p;
    upol_init(&p);
    char k[UPOL_KEY_MAX];
    upol_key(k, sizeof k, NULL, 0x0fd9, 0x00ba, "0.2");
    for (int i = 0; i < 3; i++) {
        upol_note_reset(&p, k, UPOL_STRIKE_OUT, S(100 * i));
        upol_note_detach(&p, k, true, S(100 * i + 0.01)); /* ours: kept */
        upol_note_attach(&p, k, S(100 * i + 1));
    }
    int decks, resets, q;
    upol_summary(&p, &decks, &resets, &q);
    TEST_ASSERT_EQUAL(3, resets);
    upol_note_detach(&p, k, false, S(250)); /* unplugged: another deck may come next */
    upol_summary(&p, &decks, &resets, &q);
    TEST_ASSERT_EQUAL(0, decks);
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, k, S(260)));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, k, S(300)));
    TEST_ASSERT_FALSE(upol_note_reset(&p, k, UPOL_STRIKE_OUT, S(300)));
    /* Another PID at the same socket never shared the record anyway. */
    char k2[UPOL_KEY_MAX];
    upol_key(k2, sizeof k2, NULL, 0x0fd9, 0x0084, "0.2");
    TEST_ASSERT_TRUE(strcmp(k, k2) != 0);
}

/* One text per quarantine level and time left, the same when the reset quarantines the
 * deck and at the re-attach after it; it fits the problem buffer whole, even with the longest
 * serial, and says that the deck is retried by itself (no more "until replugged"). */
static void test_quarantine_message_fits_and_matches(void)
{
    char serial[32], a[USBDECK_PROBLEM_MAX + 32], b[USBDECK_PROBLEM_MAX + 32];
    memset(serial, 'S', sizeof serial - 1); /* usbdeck_info_t.serial: 31 characters at most */
    serial[sizeof serial - 1] = 0;
    const int64_t longest = UPOL_QUARANTINE_MAX_US;
    TEST_ASSERT_TRUE(upol_quarantine_msg(a, sizeof a, serial, UPOL_Q_WRITES, longest) < USBDECK_PROBLEM_MAX);
    TEST_ASSERT_TRUE(upol_quarantine_msg(b, sizeof b, serial, UPOL_Q_ALL, longest) < USBDECK_PROBLEM_MAX);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
    TEST_ASSERT_NOT_NULL(strstr(a, serial));
    TEST_ASSERT_NOT_NULL(strstr(a, "retrying in 60 min"));
    TEST_ASSERT_NOT_NULL(strstr(b, "retrying in 60 min"));
    TEST_ASSERT_NULL(strstr(a, "replug"));
    TEST_ASSERT_NULL(strstr(b, "replug"));
    char prefix[USBDECK_PROBLEM_MAX]; /* surfaces drops the deck's texts by it once it is restored */
    snprintf(prefix, sizeof prefix, USBDECK_QUARANTINE_PREFIX, serial);
    TEST_ASSERT_EQUAL(0, strncmp(a, prefix, strlen(prefix)));
    TEST_ASSERT_EQUAL(0, strncmp(b, prefix, strlen(prefix)));
    TEST_ASSERT_EQUAL(strlen(prefix), usbdeck_quarantine_key_len(a)); /* one entry per deck in surfaces */
    TEST_ASSERT_EQUAL(strlen(prefix), usbdeck_quarantine_key_len(b));
    TEST_ASSERT_EQUAL(0, upol_quarantine_msg(a, sizeof a, serial, UPOL_Q_NONE, longest));
    TEST_ASSERT_EQUAL_STRING("", a);
    /* Whole minutes, rounded up, 1..60: overdue (held back by another deck's retry) is "1 min". */
    upol_quarantine_msg(a, sizeof a, "X", UPOL_Q_WRITES, MINS(10));
    TEST_ASSERT_EQUAL_STRING("Stream Deck X keeps stalling USB: images off, retrying in 10 min", a);
    upol_quarantine_msg(a, sizeof a, "X", UPOL_Q_ALL, MINS(10) - S(7));
    TEST_ASSERT_EQUAL_STRING("Stream Deck X keeps stalling USB: off, retrying in 10 min", a);
    upol_quarantine_msg(a, sizeof a, "X", UPOL_Q_WRITES, MINS(9) + 1);
    TEST_ASSERT_NOT_NULL(strstr(a, "retrying in 10 min"));
    upol_quarantine_msg(a, sizeof a, "X", UPOL_Q_WRITES, S(1));
    TEST_ASSERT_NOT_NULL(strstr(a, "retrying in 1 min"));
    upol_quarantine_msg(a, sizeof a, "X", UPOL_Q_WRITES, -S(5));
    TEST_ASSERT_NOT_NULL(strstr(a, "retrying in 1 min"));
    upol_quarantine_msg(a, sizeof a, "X", UPOL_Q_WRITES, 3 * MINS(60));
    TEST_ASSERT_NOT_NULL(strstr(a, "retrying in 60 min"));
    /* The level and the time left reported by the quarantining reset are what the re-attach after
     * it reports: an earlier IN strike in the window makes an OUT-quarantined deck Q_ALL in both
     * places, and the re-attach a few seconds later still rounds up to the whole period. */
    upol_t p;
    upol_init(&p);
    upol_note_reset(&p, "X", UPOL_STRIKE_IN, S(10));
    upol_note_attach(&p, "X", S(11));
    const upol_quarantine_t at_reset = upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(13));
    TEST_ASSERT_EQUAL(UPOL_Q_ALL, at_reset);
    upol_quarantine_msg(a, sizeof a, serial, at_reset, upol_quarantine_end_us(&p, "X") - S(13));
    const upol_quarantine_t at_attach = upol_note_attach(&p, "X", S(19));
    TEST_ASSERT_EQUAL(at_reset, at_attach);
    upol_quarantine_msg(b, sizeof b, serial, at_attach, upol_quarantine_end_us(&p, "X") - S(19));
    TEST_ASSERT_EQUAL_STRING(a, b);
}

/* ---- restart ---- */

/* A restart caused by a reset that didn't bring USB back must not wipe the record. */
static void test_record_survives_restart(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_attach(&p, "X", S(4000));
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(5000)); /* reset; USB doesn't come back */
    upol_after_restart(&p);                             /* clock restarts at 0 */
    TEST_ASSERT_FALSE(upol_reset_allowed(&p, "Y", S(5))); /* the floor counts from boot */
    /* The re-attach after that reset is this boot's attach: probation starts now. */
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", S(7)));
    TEST_ASSERT_TRUE(upol_reset_allowed(&p, "X", S(9)));   /* bypass: >= 1 s after "boot" */
    TEST_ASSERT_TRUE(upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(9)));
    upol_after_restart(&p);
    TEST_ASSERT_EQUAL(UPOL_Q_WRITES, upol_note_attach(&p, "X", S(20)));
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "X", UPOL_QUARANTINE_MIN_US)); /* from boot */
    /* A pending re-attach kept across a restart counts from boot, bounded like any other. */
    upol_init(&p);
    upol_note_reset(&p, "Y", UPOL_STRIKE_OUT, S(5000));
    upol_after_restart(&p);
    upol_note_attach(&p, "Y", UPOL_REATTACH_US + S(1)); /* the deck only came back much later */
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "Y", UPOL_REATTACH_US + S(3)));
}

/* A manual restart long after a cured reset: counts kept, no probation. */
static void test_restart_ends_running_probation(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(100));
    upol_note_attach(&p, "X", S(101));
    upol_after_restart(&p);
    upol_note_attach(&p, "X", S(7));
    TEST_ASSERT_FALSE(upol_would_quarantine(&p, "X", S(9)));
    int decks, resets, q;
    upol_summary(&p, &decks, &resets, &q);
    TEST_ASSERT_EQUAL(1, decks);
    TEST_ASSERT_EQUAL(1, resets);
    TEST_ASSERT_EQUAL(0, q);
}

static void test_after_restart_repairs_garbage(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(1));
    memset(p.rec[1].key, 'A', sizeof p.rec[1].key); /* no NUL */
    p.rec[1].nresets = 255;
    upol_after_restart(&p);
    TEST_ASSERT_EQUAL(UPOL_KEY_MAX - 1, strlen(p.rec[1].key));
    TEST_ASSERT_EQUAL(UPOL_RESETS_QUARANTINE, p.rec[1].nresets);
}

static void test_sum_detects_change(void)
{
    upol_t p;
    upol_init(&p);
    upol_note_reset(&p, "X", UPOL_STRIKE_OUT, S(1));
    const uint32_t s = upol_sum(&p);
    TEST_ASSERT_EQUAL_UINT32(s, upol_sum(&p));
    p.rec[0].nresets ^= 1;
    TEST_ASSERT_TRUE(upol_sum(&p) != s);
}

static void test_empty_key_ignored(void)
{
    upol_t p;
    upol_init(&p);
    TEST_ASSERT_FALSE(upol_note_reset(&p, "", UPOL_STRIKE_OUT, S(0)));
    TEST_ASSERT_FALSE(upol_note_reset(&p, NULL, UPOL_STRIKE_OUT, S(20)));
    TEST_ASSERT_EQUAL(UPOL_Q_NONE, upol_note_attach(&p, "", S(30)));
    upol_note_detach(&p, NULL, false, S(40));
}

/* ---- the status light's deck trouble ----
 * A deck's input or image writes count as failing now while the problem was reported for the streak
 * and the last failure is less than UPOL_FAILING_HOLD_S old: a deck that recovered stops counting
 * then, without waiting for a key press or an image. Seconds of a 32-bit clock (wraps after 136
 * years). */
static void test_failing_window(void)
{
    TEST_ASSERT_FALSE(upol_failing(false, 100, 100)); /* never reported */
    TEST_ASSERT_TRUE(upol_failing(true, 100, 100));
    TEST_ASSERT_TRUE(upol_failing(true, 100, 100 + UPOL_FAILING_HOLD_S - 1));
    TEST_ASSERT_FALSE(upol_failing(true, 100, 100 + UPOL_FAILING_HOLD_S));
    TEST_ASSERT_FALSE(upol_failing(true, 100, 100000));
    TEST_ASSERT_TRUE(upol_failing(true, 0xFFFFFFFFu, 2)); /* across the wrap */
    TEST_ASSERT_TRUE(UPOL_FAILING_HOLD_S >= 2);           /* longer than the 1 s slow retry */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_interval_hs_fs);
    RUN_TEST(test_chunk_pkts_bounded);
    RUN_TEST(test_out_timeout_covers_healthy_stall);
    RUN_TEST(test_backoff_doubles_and_caps);
    RUN_TEST(test_sweep_window);
    RUN_TEST(test_in_completion_parks_only_while_held);
    RUN_TEST(test_close_halts_a_gone_decks_in_after_it_settles);
    RUN_TEST(test_close_never_halts_a_present_decks_queued_in);
    RUN_TEST(test_out_waits_for_a_submit_in_flight);
    RUN_TEST(test_queued_in_never_blocks_an_out);
    RUN_TEST(test_key_serial_or_stable_path);
    RUN_TEST(test_unreadable_serial_deck_quarantined);
    RUN_TEST(test_reset_floor);
    RUN_TEST(test_restall_on_probation_quarantines_past_floor);
    RUN_TEST(test_flapping_device_doesnt_delay_reset);
    RUN_TEST(test_root_settled_after_power_on);
    RUN_TEST(test_no_bypass_for_first_old_or_empty);
    RUN_TEST(test_cured_stalls_not_quarantined);
    RUN_TEST(test_fourth_reset_in_window_quarantines);
    RUN_TEST(test_replug_soon_after_reset_lifts);
    RUN_TEST(test_quarantine_len_doubles_and_caps);
    RUN_TEST(test_quarantine_lifts_while_attached);
    RUN_TEST(test_timed_lift_keeps_history_and_arms_probation);
    RUN_TEST(test_failed_retry_doubles);
    RUN_TEST(test_passed_retry);
    RUN_TEST(test_streak_resets_after_quiet_hour);
    RUN_TEST(test_attach_after_expiry_is_the_retry);
    RUN_TEST(test_nquar_saturates);
    RUN_TEST(test_streak_kept_while_quarantined);
    RUN_TEST(test_one_lift_at_a_time);
    RUN_TEST(test_restart_rebases_quarantine_end);
    RUN_TEST(test_foreign_detach_keeps_streak);
    RUN_TEST(test_lift_without_record);
    RUN_TEST(test_late_attach_after_reset_no_probation);
    RUN_TEST(test_lift_keeps_reset_history);
    RUN_TEST(test_path_key_dropped_on_foreign_detach);
    RUN_TEST(test_quarantine_message_fits_and_matches);
    RUN_TEST(test_records_evict_lru_but_keep_quarantined);
    RUN_TEST(test_all_quarantined_evicts_oldest);
    RUN_TEST(test_record_survives_restart);
    RUN_TEST(test_restart_ends_running_probation);
    RUN_TEST(test_after_restart_repairs_garbage);
    RUN_TEST(test_sum_detects_change);
    RUN_TEST(test_empty_key_ignored);
    RUN_TEST(test_failing_window);
    return UNITY_END();
}
