#include <string.h>

#include "netmgr/wifiscan.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static wifiscan_ap_t ap(const char *ssid, int rssi, int ch, int auth)
{
    wifiscan_ap_t a = {0};
    strncpy(a.ssid, ssid, sizeof a.ssid - 1);
    a.rssi = (int8_t)rssi;
    a.channel = (uint8_t)ch;
    a.auth = (uint8_t)auth;
    return a;
}

static void test_tidy_sorts_strongest_first(void)
{
    wifiscan_ap_t a[] = {ap("weak", -80, 1, 3), ap("strong", -40, 6, 3), ap("mid", -60, 11, 0)};
    size_t n = wifiscan_tidy(a, 3, WIFISCAN_MAX);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_STRING("strong", a[0].ssid);
    TEST_ASSERT_EQUAL_STRING("mid", a[1].ssid);
    TEST_ASSERT_EQUAL_STRING("weak", a[2].ssid);
}

static void test_tidy_dedupes_keeping_strongest(void)
{
    wifiscan_ap_t a[] = {ap("Home", -70, 1, 3), ap("Home", -45, 11, 7), ap("Other", -60, 6, 0), ap("Home", -90, 6, 3)};
    size_t n = wifiscan_tidy(a, 4, WIFISCAN_MAX);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_STRING("Home", a[0].ssid);
    TEST_ASSERT_EQUAL_INT(-45, a[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(11, a[0].channel);
    TEST_ASSERT_EQUAL_UINT8(7, a[0].auth);
    TEST_ASSERT_EQUAL_STRING("Other", a[1].ssid);
}

static void test_tidy_dedupe_is_case_sensitive(void)
{
    wifiscan_ap_t a[] = {ap("home", -50, 1, 3), ap("Home", -60, 1, 3)};
    TEST_ASSERT_EQUAL_UINT(2, wifiscan_tidy(a, 2, WIFISCAN_MAX));
}

static void test_tidy_drops_hidden(void)
{
    wifiscan_ap_t a[] = {ap("", -30, 1, 3), ap("seen", -70, 1, 3)};
    size_t n = wifiscan_tidy(a, 2, WIFISCAN_MAX);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_STRING("seen", a[0].ssid);
}

static void test_tidy_caps_after_sorting(void)
{
    wifiscan_ap_t a[5];
    const char *names[] = {"a", "b", "c", "d", "e"};
    for (int i = 0; i < 5; i++) a[i] = ap(names[i], -90 + i * 10, 1, 3); /* e is the strongest */
    size_t n = wifiscan_tidy(a, 5, 3);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_STRING("e", a[0].ssid);
    TEST_ASSERT_EQUAL_STRING("c", a[2].ssid);
}

static void test_tidy_empty(void) { TEST_ASSERT_EQUAL_UINT(0, wifiscan_tidy(NULL, 0, WIFISCAN_MAX)); }

static void test_tidy_equal_signal_is_ordered_by_name(void)
{
    wifiscan_ap_t a[] = {ap("b", -50, 1, 3), ap("a", -50, 1, 3)};
    wifiscan_tidy(a, 2, WIFISCAN_MAX);
    TEST_ASSERT_EQUAL_STRING("a", a[0].ssid);
}

static void test_auth_names(void)
{
    TEST_ASSERT_EQUAL_STRING("open", wifiscan_auth_name(WIFISCAN_AUTH_OPEN));
    TEST_ASSERT_EQUAL_STRING("wep", wifiscan_auth_name(WIFISCAN_AUTH_WEP));
    TEST_ASSERT_EQUAL_STRING("wpa_psk", wifiscan_auth_name(WIFISCAN_AUTH_WPA_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa2_psk", wifiscan_auth_name(WIFISCAN_AUTH_WPA2_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa_wpa2_psk", wifiscan_auth_name(WIFISCAN_AUTH_WPA_WPA2_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa2_enterprise", wifiscan_auth_name(WIFISCAN_AUTH_WPA2_ENTERPRISE));
    TEST_ASSERT_EQUAL_STRING("wpa3_psk", wifiscan_auth_name(WIFISCAN_AUTH_WPA3_PSK));
    TEST_ASSERT_EQUAL_STRING("wpa2_wpa3_psk", wifiscan_auth_name(WIFISCAN_AUTH_WPA2_WPA3_PSK));
    TEST_ASSERT_EQUAL_STRING("other", wifiscan_auth_name(99));
    TEST_ASSERT_EQUAL_STRING("other", wifiscan_auth_name(-1));
}

static void test_decide_runs_with_no_history(void)
{
    TEST_ASSERT_EQUAL_INT(WIFISCAN_RUN, wifiscan_decide(false, false, 1000, 0));
}

static void test_decide_busy_beats_everything(void)
{
    TEST_ASSERT_EQUAL_INT(WIFISCAN_BUSY, wifiscan_decide(true, false, 1000, 0));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_BUSY, wifiscan_decide(true, true, 100000, 1000));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_BUSY, wifiscan_decide(true, true, 1001, 1000));
}

static void test_decide_cached_inside_the_gap(void)
{
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CACHED, wifiscan_decide(false, true, 10000, 10000));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CACHED, wifiscan_decide(false, true, 12999, 10000));
}

static void test_decide_runs_after_the_gap(void)
{
    TEST_ASSERT_EQUAL_INT(WIFISCAN_RUN, wifiscan_decide(false, true, 13000, 10000));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_RUN, wifiscan_decide(false, true, 60000, 10000));
}

static void test_decide_clock_going_backwards_is_cached(void)
{
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CACHED, wifiscan_decide(false, true, 5000, 10000));
}

static void test_timeout_only_while_scanning(void)
{
    TEST_ASSERT_FALSE(wifiscan_timed_out(true, 1000 + WIFISCAN_TIMEOUT_MS - 1, 1000));
    TEST_ASSERT_TRUE(wifiscan_timed_out(true, 1000 + WIFISCAN_TIMEOUT_MS, 1000));
    TEST_ASSERT_TRUE(wifiscan_timed_out(true, 99999999, 1000));
    TEST_ASSERT_FALSE(wifiscan_timed_out(false, 99999999, 1000)); /* a finished scan has nothing to restore */
}

static void test_age(void)
{
    TEST_ASSERT_EQUAL_INT64(-1, wifiscan_age_ms(false, 5000, 0));
    TEST_ASSERT_EQUAL_INT64(1500, wifiscan_age_ms(true, 6500, 5000));
    TEST_ASSERT_EQUAL_INT64(0, wifiscan_age_ms(true, 4000, 5000)); /* clock went backwards */
}

/* idle -> scanning -> done -> cached -> idle again, as netmgr drives it */
static void test_cycle(void)
{
    bool running = false, have = false;
    int64_t end = 0;
    TEST_ASSERT_EQUAL_INT(WIFISCAN_RUN, wifiscan_decide(running, have, 100, end));
    running = true; /* started */
    TEST_ASSERT_EQUAL_INT(WIFISCAN_BUSY, wifiscan_decide(running, have, 600, end)); /* a second call: still scanning */
    running = false; have = true; end = 4000; /* scan done */
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CACHED, wifiscan_decide(running, have, 5000, end));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_RUN, wifiscan_decide(running, have, 7000, end));
}

static void test_done_is_ours_only_after_our_start(void)
{
    TEST_ASSERT_TRUE(wifiscan_done_is_ours(true, true, 0));
    TEST_ASSERT_FALSE(wifiscan_done_is_ours(true, false, 0)); /* before scan_start returned OK: the station's own scan */
    TEST_ASSERT_FALSE(wifiscan_done_is_ours(false, true, 0)); /* no scan of ours */
    TEST_ASSERT_FALSE(wifiscan_done_is_ours(false, false, 0));
    TEST_ASSERT_FALSE(wifiscan_done_is_ours(true, true, 1)); /* an unexpected status: the timeout deals with it */
}

static void test_done_without_payload_counts_as_success(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, wifiscan_done_status(false, 7)); /* esp_wifi_remote may deliver no data */
    TEST_ASSERT_EQUAL_UINT32(1, wifiscan_done_status(true, 1));
    TEST_ASSERT_EQUAL_UINT32(0, wifiscan_done_status(true, 0));
    TEST_ASSERT_TRUE(wifiscan_done_is_ours(true, true, wifiscan_done_status(false, 0)));
    TEST_ASSERT_FALSE(wifiscan_done_is_ours(true, false, wifiscan_done_status(false, 0))); /* still only after our start */
}

static void test_connect_deferred_during_a_scan(void)
{
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CONN_DEFER, wifiscan_connect_act(true, true, false));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CONN_GO, wifiscan_connect_act(false, true, false));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CONN_SKIP, wifiscan_connect_act(true, false, false)); /* no station wanted: nothing to defer */
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CONN_SKIP, wifiscan_connect_act(false, false, false));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CONN_SKIP, wifiscan_connect_act(false, true, true));
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CONN_SKIP, wifiscan_connect_act(true, true, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tidy_sorts_strongest_first);
    RUN_TEST(test_tidy_dedupes_keeping_strongest);
    RUN_TEST(test_tidy_dedupe_is_case_sensitive);
    RUN_TEST(test_tidy_drops_hidden);
    RUN_TEST(test_tidy_caps_after_sorting);
    RUN_TEST(test_tidy_empty);
    RUN_TEST(test_tidy_equal_signal_is_ordered_by_name);
    RUN_TEST(test_auth_names);
    RUN_TEST(test_decide_runs_with_no_history);
    RUN_TEST(test_decide_busy_beats_everything);
    RUN_TEST(test_decide_cached_inside_the_gap);
    RUN_TEST(test_decide_runs_after_the_gap);
    RUN_TEST(test_decide_clock_going_backwards_is_cached);
    RUN_TEST(test_timeout_only_while_scanning);
    RUN_TEST(test_age);
    RUN_TEST(test_cycle);
    RUN_TEST(test_done_is_ours_only_after_our_start);
    RUN_TEST(test_done_without_payload_counts_as_success);
    RUN_TEST(test_connect_deferred_during_a_scan);
    return UNITY_END();
}
