#include <string.h>
#include "unity.h"
#include "supervisor_logic.h"

void setUp(void) {}
void tearDown(void) {}

#define S (1000000LL)
#define MIN (60 * S)

static const c6w_in_t LOST = {.c6_ready = false, .radio_wanted = true, .eth_up = false};

/* Steps once a second from `from` to `to` (inclusive); returns the first time it said restart, or -1. */
static int64_t run(c6w_t *w, c6w_in_t in, int64_t from, int64_t to, uint32_t restarts)
{
    for (int64_t t = from; t <= to; t += S)
        if (c6w_step(w, &in, t, restarts)) return t;
    return -1;
}

static void test_c6_lost_without_ethernet_restarts_after_3_min(void)
{
    c6w_t w;
    c6w_init(&w);
    TEST_ASSERT_EQUAL_INT64(5 * S + 3 * MIN, run(&w, LOST, 5 * S, 10 * MIN, 0));
}

static void test_ethernet_up_keeps_todays_behaviour(void)
{
    c6w_t w;
    c6w_init(&w);
    c6w_in_t in = LOST;
    in.eth_up = true;
    TEST_ASSERT_EQUAL_INT64(-1, run(&w, in, 0, 60 * MIN, 0));
}

static void test_c6_ready_or_no_radio_wanted_never_restarts(void)
{
    c6w_t w;
    c6w_init(&w);
    c6w_in_t in = LOST;
    in.c6_ready = true;
    TEST_ASSERT_EQUAL_INT64(-1, run(&w, in, 0, 10 * MIN, 0));
    c6w_init(&w);
    in = LOST;
    in.radio_wanted = false;
    TEST_ASSERT_EQUAL_INT64(-1, run(&w, in, 0, 10 * MIN, 0));
}

/* "3 minutes in a row": Ethernet coming up starts the count again. */
static void test_ethernet_up_in_between_restarts_the_count(void)
{
    c6w_t w;
    c6w_init(&w);
    TEST_ASSERT_EQUAL_INT64(-1, run(&w, LOST, 0, 2 * MIN, 0));
    c6w_in_t up = LOST;
    up.eth_up = true;
    TEST_ASSERT_FALSE(c6w_step(&w, &up, 2 * MIN + S, 0));
    TEST_ASSERT_EQUAL_INT64(2 * MIN + 2 * S + 3 * MIN, run(&w, LOST, 2 * MIN + 2 * S, 10 * MIN, 0));
}

/* A C6 lost after Ethernet has been down long enough: restart at once. */
static void test_c6_lost_late_restarts_at_once(void)
{
    c6w_t w;
    c6w_init(&w);
    c6w_in_t ok = LOST;
    ok.c6_ready = true;
    TEST_ASSERT_EQUAL_INT64(-1, run(&w, ok, 0, 10 * MIN, 0));
    TEST_ASSERT_TRUE(c6w_step(&w, &LOST, 10 * MIN + S, 0));
}

/* At most 3 per power-on, then one per 30 minutes of uptime. */
static void test_rate_limit_after_three(void)
{
    c6w_t w;
    for (uint32_t n = 0; n < 3; n++) {
        c6w_init(&w);
        TEST_ASSERT_EQUAL_INT64(3 * MIN, run(&w, LOST, 0, 10 * MIN, n));
    }
    c6w_init(&w);
    TEST_ASSERT_EQUAL_INT64(30 * MIN, run(&w, LOST, 0, 40 * MIN, 3));
    c6w_init(&w);
    TEST_ASSERT_EQUAL_INT64(30 * MIN, run(&w, LOST, 0, 40 * MIN, 17));
}

static void test_record_round_trip_and_garbage(void)
{
    c6w_rec_t r;
    memset(&r, 0xA5, sizeof r);
    TEST_ASSERT_EQUAL_UINT32(0, c6w_rec_count(&r));
    memset(&r, 0, sizeof r);
    TEST_ASSERT_EQUAL_UINT32(0, c6w_rec_count(&r));
    c6w_rec_set(&r, 2);
    TEST_ASSERT_EQUAL_UINT32(2, c6w_rec_count(&r));
    c6w_rec_set(&r, 3);
    TEST_ASSERT_EQUAL_UINT32(3, c6w_rec_count(&r));
    r.count ^= 0x10; /* a torn or random value */
    TEST_ASSERT_EQUAL_UINT32(0, c6w_rec_count(&r));
}

/* ---- OTA self-check ---- */

static bool net(netmgr_if_t via, bool eth_ip, bool sta_ip, bool ap_on, bool routed)
{
    const ota_net_t n = {.via = via, .eth_ip = eth_ip, .sta_ip = sta_ip, .ap_on = ap_on, .routed = routed};
    const char *why = NULL;
    const bool ok = ota_net_ok(&n, &why);
    TEST_ASSERT_NOT_NULL(why);
    if (!ok) TEST_ASSERT_TRUE(why[0] != '\0'); /* the log says what is missing */
    return ok;
}

/* The interface the update came over must be back: the others don't count. */
static void test_ota_needs_the_interface_it_came_over(void)
{
    TEST_ASSERT_TRUE(net(NETMGR_IF_ETH, true, false, false, true));
    TEST_ASSERT_FALSE(net(NETMGR_IF_ETH, false, true, true, true));
    TEST_ASSERT_TRUE(net(NETMGR_IF_STA, false, true, false, true));
    TEST_ASSERT_FALSE(net(NETMGR_IF_STA, true, false, true, true));
    TEST_ASSERT_TRUE(net(NETMGR_IF_AP, false, false, true, false));
    TEST_ASSERT_FALSE(net(NETMGR_IF_AP, true, true, false, true));
}

/* No record (e.g. the record couldn't be written): a route or the hotspot. */
static void test_ota_without_a_record_needs_a_route_or_the_hotspot(void)
{
    TEST_ASSERT_TRUE(net(NETMGR_IF_NONE, false, false, false, true));
    TEST_ASSERT_TRUE(net(NETMGR_IF_NONE, false, false, true, false));
    TEST_ASSERT_FALSE(net(NETMGR_IF_NONE, true, true, false, false));
}

/* the web page's countdown names the check that rolls back: the one ota_verdict rolls back at, the one before waits */
static void test_ota_rollback_check(void)
{
    const int64_t start = 4200000;
    const int64_t at = ota_rollback_check_us(start);
    TEST_ASSERT_EQUAL_INT64(start + 120 * 1000000LL, at); /* 30 s, then every 30 s: the fourth check, 2 min */
    TEST_ASSERT_EQUAL_INT64(0, (at - start - OTA_FIRST_CHECK_US) % OTA_RECHECK_US); /* a check runs then */
    TEST_ASSERT_EQUAL_INT(OTA_ROLLBACK, ota_verdict(false, at - start));
    TEST_ASSERT_EQUAL_INT(OTA_WAIT, ota_verdict(false, at - start - OTA_RECHECK_US));
}

/* Checked every 30 s from 30 s; rolled back only at 2 min. */
static void test_ota_verdict_waits_up_to_2_min(void)
{
    TEST_ASSERT_EQUAL_INT(OTA_CONFIRM, ota_verdict(true, OTA_FIRST_CHECK_US));
    for (int64_t t = OTA_FIRST_CHECK_US; t < OTA_GIVE_UP_US; t += OTA_RECHECK_US)
        TEST_ASSERT_EQUAL_INT(OTA_WAIT, ota_verdict(false, t));
    TEST_ASSERT_EQUAL_INT(OTA_CONFIRM, ota_verdict(true, OTA_GIVE_UP_US - OTA_RECHECK_US));
    TEST_ASSERT_EQUAL_INT(OTA_ROLLBACK, ota_verdict(false, OTA_GIVE_UP_US));
    TEST_ASSERT_EQUAL_INT(OTA_CONFIRM, ota_verdict(true, OTA_GIVE_UP_US));
    TEST_ASSERT_EQUAL_INT64(2 * 60 * S, OTA_GIVE_UP_US);
    TEST_ASSERT_EQUAL_INT64(30 * S, OTA_RECHECK_US);
}

/* ---- the Companion pin's save ---- */

static void test_pin_save_only_when_changed(void)
{
    TEST_ASSERT_FALSE(pin_save_due(false, 10 * S, -1));
    TEST_ASSERT_FALSE(pin_save_due(false, 10 * MIN, 0));
    TEST_ASSERT_TRUE(pin_save_due(true, 10 * S, -1)); /* the first pin: at once */
}

/* An address that flips every second is saved at most once a minute. */
static void test_pin_save_at_most_once_a_minute(void)
{
    TEST_ASSERT_FALSE(pin_save_due(true, 10 * S + 1 * S, 10 * S));
    TEST_ASSERT_FALSE(pin_save_due(true, 10 * S + PIN_SAVE_MIN_US - 1, 10 * S));
    TEST_ASSERT_TRUE(pin_save_due(true, 10 * S + PIN_SAVE_MIN_US, 10 * S));
    int saves = 0;
    int64_t last = -1;
    for (int64_t t = 0; t < 10 * MIN; t += S) /* flips every second: always "changed" */
        if (pin_save_due(true, t, last)) {
            saves++;
            last = t;
        }
    TEST_ASSERT_EQUAL_INT(10, saves);
}

/* an image uploaded over a forced hotspot gets the hotspot back for its self-check. */
static void test_ota_over_a_forced_hotspot_forces_it_again(void)
{
    TEST_ASSERT_TRUE(ota_force_hotspot(true, NETMGR_IF_AP, false, false));
    TEST_ASSERT_TRUE(OTA_HOTSPOT_FORCE_S * 1000000LL > OTA_GIVE_UP_US); /* covers the whole window */
}

static void test_ota_hotspot_force_only_when_needed(void)
{
    TEST_ASSERT_FALSE(ota_force_hotspot(false, NETMGR_IF_AP, false, false)); /* nothing pending */
    TEST_ASSERT_FALSE(ota_force_hotspot(true, NETMGR_IF_ETH, false, false)); /* another path */
    TEST_ASSERT_FALSE(ota_force_hotspot(true, NETMGR_IF_STA, false, false));
    TEST_ASSERT_FALSE(ota_force_hotspot(true, NETMGR_IF_NONE, false, false));
    TEST_ASSERT_FALSE(ota_force_hotspot(true, NETMGR_IF_AP, true, false));   /* the settings have it on */
    TEST_ASSERT_FALSE(ota_force_hotspot(true, NETMGR_IF_AP, false, true));   /* RESET x3: don't shorten */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_c6_lost_without_ethernet_restarts_after_3_min);
    RUN_TEST(test_ethernet_up_keeps_todays_behaviour);
    RUN_TEST(test_c6_ready_or_no_radio_wanted_never_restarts);
    RUN_TEST(test_ethernet_up_in_between_restarts_the_count);
    RUN_TEST(test_c6_lost_late_restarts_at_once);
    RUN_TEST(test_rate_limit_after_three);
    RUN_TEST(test_record_round_trip_and_garbage);
    RUN_TEST(test_ota_needs_the_interface_it_came_over);
    RUN_TEST(test_ota_without_a_record_needs_a_route_or_the_hotspot);
    RUN_TEST(test_ota_verdict_waits_up_to_2_min);
    RUN_TEST(test_pin_save_only_when_changed);
    RUN_TEST(test_ota_over_a_forced_hotspot_forces_it_again);
    RUN_TEST(test_ota_hotspot_force_only_when_needed);
    RUN_TEST(test_pin_save_at_most_once_a_minute);
    RUN_TEST(test_ota_rollback_check);
    return UNITY_END();
}
