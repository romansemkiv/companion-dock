/* Broadcast storm control (components/netmgr/storm_logic.c): block on a storm (heavy broadcast while the EMAC
 * loses frames), probe after a hold that doubles while the storm goes on, open when a probe finds it over; a heavy
 * flood the box keeps up with is never blocked. */
#include <string.h>

#include "netmgr/storm.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static storm_t s;
static int64_t now;
static storm_ev_t ev;

static uint32_t lost_now = 5000; /* the EMAC loses frames in each tick (a flood over what the box keeps up with) */

static storm_act_t tick(uint32_t bcast)
{
    now += STORM_TICK_MS;
    return storm_tick(&s, now, bcast, bcast >= STORM_ON_PER_TICK ? lost_now : 0, &ev);
}

/* ticks of a steady flood (or quiet) until the given action, at most max ticks; the ticks it took, -1 if none */
static int ticks_until(storm_act_t want, uint32_t bcast, int max)
{
    for (int i = 1; i <= max; i++)
        if (tick(bcast) == want) return i;
    return -1;
}

/* a storm begins: STORM_ON_TICKS ticks of it in a row, the last one blocks */
static void storm_on(uint32_t bcast)
{
    for (int k = 0; k < STORM_ON_TICKS; k++) tick(bcast);
}

static void start(void)
{
    storm_init(&s);
    now = 0;
    ev = STORM_EV_NONE;
    lost_now = 5000;
}

/* a storm takes STORM_ON_TICKS ticks in a row to block: one tick's loss (a stalled task, a log line) never does */
static void test_a_moments_loss_does_not_block(void)
{
    start();
    for (int i = 0; i < 1000; i++) {
        for (int k = 1; k < STORM_ON_TICKS; k++) TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(15000));
        TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(10)); /* the run broken */
    }
    TEST_ASSERT_EQUAL_UINT32(0, s.storms);
    /* lost frames under STORM_LOST_PER_TICK (the line's odd frame) are no storm however long */
    lost_now = STORM_LOST_PER_TICK - 1;
    for (int i = 0; i < 600; i++) TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(15000));
    TEST_ASSERT_EQUAL_UINT32(0, s.storms);
}

/* the box keeps up: however heavy the broadcast, nothing lost, nothing blocked (ARP and discovery go on) */
static void test_a_flood_the_box_keeps_up_with_is_not_blocked(void)
{
    start();
    lost_now = 0;
    for (int i = 0; i < 600; i++) TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(15000)); /* 150 000 a second, 1 min */
    TEST_ASSERT_FALSE(storm_blocked(&s));
    TEST_ASSERT_EQUAL_UINT32(0, s.storms);
    /* frames lost without heavy broadcast (a unicast burst) is no broadcast storm either */
    now += STORM_TICK_MS;
    TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, storm_tick(&s, now, 20, 500, &ev));
    TEST_ASSERT_FALSE(storm_blocked(&s));
}

static void test_normal_traffic_never_blocks(void)
{
    start();
    for (int i = 0; i < 6000; i++) { /* 10 minutes of a busy LAN: 30 a second, bursts of 149 a tick */
        TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(i % 50 == 0 ? STORM_ON_PER_TICK - 1 : 3));
        TEST_ASSERT_EQUAL_INT(STORM_EV_NONE, ev);
    }
    TEST_ASSERT_FALSE(storm_blocked(&s));
    TEST_ASSERT_EQUAL_UINT32(0, s.storms);
}

static void test_a_storm_blocks_at_once(void)
{
    start();
    tick(5);
    for (int k = 1; k < STORM_ON_TICKS; k++) TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(STORM_ON_PER_TICK));
    TEST_ASSERT_EQUAL_INT(STORM_ACT_BLOCK, tick(STORM_ON_PER_TICK));
    TEST_ASSERT_EQUAL_INT(STORM_EV_BEGAN, ev);
    TEST_ASSERT_TRUE(storm_blocked(&s));
    TEST_ASSERT_EQUAL_UINT32(1, s.storms);
    TEST_ASSERT_EQUAL_UINT32(STORM_ON_PER_TICK, s.peak);
}

static void test_the_hold_doubles_while_the_storm_goes_on(void)
{
    start();
    storm_on(400);
    int32_t hold = STORM_HOLD_MS;
    for (int round = 0; round < 6; round++) {
        /* blocked: nothing counts (frames counted are from before), until the hold is over */
        TEST_ASSERT_EQUAL_INT(hold / STORM_TICK_MS, ticks_until(STORM_ACT_OPEN, 4000, 1000));
        TEST_ASSERT_EQUAL_INT(STORM_EV_NONE, ev); /* a probe says nothing */
        TEST_ASSERT_FALSE(storm_blocked(&s));
        /* the probe finds the storm still going: blocked again at once */
        TEST_ASSERT_EQUAL_INT(STORM_ACT_BLOCK, tick(4000));
        TEST_ASSERT_EQUAL_INT(STORM_EV_NONE, ev);
        hold = hold * 2 > STORM_HOLD_MAX_MS ? STORM_HOLD_MAX_MS : hold * 2;
    }
    TEST_ASSERT_EQUAL_INT32(STORM_HOLD_MAX_MS, s.hold_ms);
    TEST_ASSERT_EQUAL_UINT32(1, s.storms); /* one storm, however long */
}

static void test_a_quiet_probe_ends_the_storm(void)
{
    start();
    storm_on(4000);
    TEST_ASSERT_EQUAL_INT(STORM_HOLD_MS / STORM_TICK_MS, ticks_until(STORM_ACT_OPEN, 0, 1000)); /* the probe opens */
    /* the whole probe quiet: open for good, said once */
    for (int i = 0; i < STORM_PROBE_TICKS - 1; i++) {
        TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(10));
        TEST_ASSERT_EQUAL_INT(STORM_EV_NONE, ev);
    }
    TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(10));
    TEST_ASSERT_EQUAL_INT(STORM_EV_ENDED, ev);
    TEST_ASSERT_EQUAL_INT(STORM_OPEN, s.st);
    /* the next storm starts with the first hold again (after STORM_ON_TICKS ticks of it) */
    for (int k = 1; k < STORM_ON_TICKS; k++) TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(4000));
    TEST_ASSERT_EQUAL_INT(STORM_ACT_BLOCK, tick(4000));
    TEST_ASSERT_EQUAL_INT(STORM_EV_BEGAN, ev);
    TEST_ASSERT_EQUAL_INT32(STORM_HOLD_MS, s.hold_ms);
    TEST_ASSERT_EQUAL_UINT32(2, s.storms);
}

static void test_a_storm_late_in_the_probe_blocks_too(void)
{
    start();
    storm_on(4000);
    ticks_until(STORM_ACT_OPEN, 0, 1000);
    for (int i = 0; i < STORM_PROBE_TICKS - 1; i++) TEST_ASSERT_EQUAL_INT(STORM_ACT_NONE, tick(10));
    TEST_ASSERT_EQUAL_INT(STORM_ACT_BLOCK, tick(STORM_ON_PER_TICK));
    TEST_ASSERT_EQUAL_INT32(STORM_HOLD_MS * 2, s.hold_ms);
}

static void test_a_late_or_stalled_tick_does_not_stretch_the_hold(void)
{
    start();
    storm_on(4000);
    /* the timer late by 5 s: the hold (2 s) is over, the probe starts with that tick */
    now += 5000;
    TEST_ASSERT_EQUAL_INT(STORM_ACT_OPEN, storm_tick(&s, now, 0, 0, &ev));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normal_traffic_never_blocks);
    RUN_TEST(test_a_flood_the_box_keeps_up_with_is_not_blocked);
    RUN_TEST(test_a_moments_loss_does_not_block);
    RUN_TEST(test_a_storm_blocks_at_once);
    RUN_TEST(test_the_hold_doubles_while_the_storm_goes_on);
    RUN_TEST(test_a_quiet_probe_ends_the_storm);
    RUN_TEST(test_a_storm_late_in_the_probe_blocks_too);
    RUN_TEST(test_a_late_or_stalled_tick_does_not_stretch_the_hold);
    return UNITY_END();
}
