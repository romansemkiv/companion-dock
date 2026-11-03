#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "netmgr/netmgr.h"
#include "netmgr/wififo_logic.h"
#include "netmgr/wifiscan.h"

void setUp(void) {}
void tearDown(void) {}

static void test_eth_preferred(void)
{
    net_state_t s = {.eth_link = true, .eth_ip = true, .wifi_configured = true, .wifi_ip = true};
    TEST_ASSERT_EQUAL_INT(NET_ROUTE_ETH, netpolicy_route(&s));
}

static void test_eth_link_no_ip_uses_wifi(void)
{
    net_state_t s = {.eth_link = true, .eth_ip = false, .wifi_configured = true, .wifi_ip = true};
    TEST_ASSERT_EQUAL_INT(NET_ROUTE_WIFI, netpolicy_route(&s));
}

static void test_nothing(void)
{
    net_state_t s = {0};
    TEST_ASSERT_EQUAL_INT(NET_ROUTE_NONE, netpolicy_route(&s));
}

static void test_eth_only(void)
{
    net_state_t s = {.eth_link = true, .eth_ip = true};
    TEST_ASSERT_EQUAL_INT(NET_ROUTE_ETH, netpolicy_route(&s));
}

/* Static address stays on the netif after the cable is pulled. */
static void test_eth_ip_without_link_uses_wifi(void)
{
    net_state_t s = {.eth_link = false, .eth_ip = true, .wifi_configured = true, .wifi_ip = true};
    TEST_ASSERT_EQUAL_INT(NET_ROUTE_WIFI, netpolicy_route(&s));
}

/* Station removed from the config while its old lease is still on the netif. */
static void test_wifi_ip_ignored_when_unconfigured(void)
{
    net_state_t s = {.wifi_configured = false, .wifi_ip = true};
    TEST_ASSERT_EQUAL_INT(NET_ROUTE_NONE, netpolicy_route(&s));
}

static void test_wifi_configured_no_ip(void)
{
    net_state_t s = {.eth_link = true, .wifi_configured = true};
    TEST_ASSERT_EQUAL_INT(NET_ROUTE_NONE, netpolicy_route(&s));
}

/* ---- Wi-Fi restart on a settings change (netmgr_apply) ---- */

static box_config_t wcfg(bool secured, const char *ap_pass)
{
    box_config_t c;
    memset(&c, 0, sizeof c);
    c.ap_enabled = true;
    c.ap_secured = secured;
    strcpy(c.ap_pass, ap_pass);
    strcpy(c.wifi_ssid, "Studio");
    strcpy(c.wifi_pass, "studiopass");
    return c;
}

static void test_wifi_same_settings(void)
{
    box_config_t a = wcfg(true, "abcdefghjk"), b = a;
    TEST_ASSERT_FALSE(netpolicy_wifi_changed(&a, &b));
    strcpy(b.box_name, "Rack"); /* not a Wi-Fi setting */
    b.companion_port = 17000;
    TEST_ASSERT_FALSE(netpolicy_wifi_changed(&a, &b));
}

/* the hotspot's password switched on or off restarts Wi-Fi, both ways. */
static void test_wifi_ap_secured_changed(void)
{
    box_config_t a = wcfg(false, "abcdefghjk"), b = wcfg(true, "abcdefghjk");
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&a, &b));
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&b, &a));
}

/* The password matters while it is on; while the hotspot is open it is unused. */
static void test_wifi_ap_pass_changed(void)
{
    box_config_t a = wcfg(true, "abcdefghjk"), b = wcfg(true, "otherpass1");
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&a, &b));
    a = wcfg(false, "abcdefghjk");
    b = wcfg(false, "otherpass1");
    TEST_ASSERT_FALSE(netpolicy_wifi_changed(&a, &b));
}

static void test_wifi_station_changed(void)
{
    box_config_t a = wcfg(false, "abcdefghjk"), b = a;
    strcpy(b.wifi_ssid, "Other");
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&a, &b));
    b = a;
    strcpy(b.wifi_pass, "");
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&a, &b));
}

/* The hotspot's address for the decks' setup page: its octets while it is on, "" while it is off. */
static void test_ap_ip(void)
{
    char ip[16] = "x";
    netpolicy_ap_ip(true, ip);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", ip);
    netpolicy_ap_ip(false, ip);
    TEST_ASSERT_EQUAL_STRING("", ip);
    static const int oct[4] = {NETMGR_AP_OCTETS};
    char want[16];
    snprintf(want, sizeof want, "%d.%d.%d.%d", oct[0], oct[1], oct[2], oct[3]);
    netpolicy_ap_ip(true, ip);
    TEST_ASSERT_EQUAL_STRING(want, ip);
}

/* the route's address as before; Ethernet's own while it has one, the station's own while joined with one,
 * whichever carries the route; "0.0.0.0" or an unread one is none. */
static void test_addrs(void)
{
    char ip[16] = "x", eth[16] = "x", sta[16] = "x";
    netpolicy_addrs(NET_ROUTE_ETH, true, "192.168.100.200", true, true, "192.168.8.225", ip, eth, sta);
    TEST_ASSERT_EQUAL_STRING("192.168.100.200", ip);
    TEST_ASSERT_EQUAL_STRING("192.168.100.200", eth);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225", sta); /* joined behind Ethernet: its own address too */
    netpolicy_addrs(NET_ROUTE_WIFI, false, "0.0.0.0", true, true, "192.168.8.225", ip, eth, sta);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225", ip);
    TEST_ASSERT_EQUAL_STRING("", eth);
    TEST_ASSERT_EQUAL_STRING("192.168.8.225", sta);
    netpolicy_addrs(NET_ROUTE_WIFI, true, "0.0.0.0", true, true, "192.168.8.225", ip, eth, sta);
    TEST_ASSERT_EQUAL_STRING("", eth); /* the flag says one, the netif has none */
    netpolicy_addrs(NET_ROUTE_ETH, true, "10.0.0.7", false, true, "192.168.8.225", ip, eth, sta);
    TEST_ASSERT_EQUAL_STRING("", sta); /* not joined (any more): none */
    netpolicy_addrs(NET_ROUTE_ETH, true, "10.0.0.7", true, false, "192.168.8.225", ip, eth, sta);
    TEST_ASSERT_EQUAL_STRING("", sta); /* joined, no address yet */
    netpolicy_addrs(NET_ROUTE_NONE, false, "", false, false, "", ip, eth, sta);
    TEST_ASSERT_EQUAL_STRING("", ip);
    TEST_ASSERT_EQUAL_STRING("", eth);
    TEST_ASSERT_EQUAL_STRING("", sta);
    netpolicy_addrs(NET_ROUTE_ETH, true, NULL, true, true, NULL, ip, eth, sta); /* unread */
    TEST_ASSERT_EQUAL_STRING("", ip);
    TEST_ASSERT_EQUAL_STRING("", eth);
    TEST_ASSERT_EQUAL_STRING("", sta);
}

/* the station's own networks (either pair) changed, not merely the hotspot. */
static void test_sta_changed(void)
{
    box_config_t a = wcfg(true, "abcdefghjk"), b = a;
    TEST_ASSERT_FALSE(netpolicy_sta_changed(&a, &b));
    b.ap_enabled = !a.ap_enabled; /* the hotspot only: switched, its password, open or not */
    strcpy(b.ap_pass, "otherpass1");
    b.ap_secured = false;
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&a, &b));
    TEST_ASSERT_FALSE(netpolicy_sta_changed(&a, &b));
    b = a;
    strcpy(b.wifi_ssid, "Other");
    TEST_ASSERT_TRUE(netpolicy_sta_changed(&a, &b));
    b = a;
    strcpy(b.wifi_pass, "changed99");
    TEST_ASSERT_TRUE(netpolicy_sta_changed(&a, &b));
    b = a;
    strcpy(b.wifi_backup_ssid, "Phone");
    TEST_ASSERT_TRUE(netpolicy_sta_changed(&a, &b));
    a = b;
    strcpy(b.wifi_backup_pass, "hotspot99");
    TEST_ASSERT_TRUE(netpolicy_sta_changed(&a, &b));
}

static void test_wifi_backup_changed(void)
{
    box_config_t a = wcfg(true, "hotpass123"), b = a;
    strcpy(b.wifi_backup_ssid, "Phone");
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&a, &b));
    a = b;
    TEST_ASSERT_FALSE(netpolicy_wifi_changed(&a, &b));
    strcpy(b.wifi_backup_pass, "hotspot99");
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&a, &b));
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&b, &a));
}

/* turning the station on or off restarts Wi-Fi, from the main network (a station change); the
 * station runs only with Wi-Fi on and a main network set. */
static void test_wifi_enabled_changed(void)
{
    box_config_t a = wcfg(true, "abcdefghjk"), b = a;
    a.wifi_enabled = b.wifi_enabled = true;
    TEST_ASSERT_FALSE(netpolicy_wifi_changed(&a, &b));
    b.wifi_enabled = false;
    TEST_ASSERT_TRUE(netpolicy_wifi_changed(&a, &b));
    TEST_ASSERT_TRUE(netpolicy_sta_changed(&a, &b)); /* back on: from the main network */
    TEST_ASSERT_TRUE(netpolicy_sta_changed(&b, &a));
    TEST_ASSERT_TRUE(netpolicy_sta_on(&a));
    TEST_ASSERT_FALSE(netpolicy_sta_on(&b)); /* off by the setting */
    a.wifi_ssid[0] = '\0';
    TEST_ASSERT_FALSE(netpolicy_sta_on(&a)); /* on, but no network set */
}

/* with the station off: no connect, no retry, the failover stopped; a scan still runs (the hotspot-only and
 * radio-off paths bring the station up just for it). */
static void test_station_off_decisions(void)
{
    box_config_t c = wcfg(true, "abcdefghjk");
    c.wifi_enabled = false;
    const bool sta_wanted = netpolicy_sta_on(&c); /* what wifi.c runs the station with */
    TEST_ASSERT_FALSE(sta_wanted);
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CONN_SKIP, wifiscan_connect_act(false, sta_wanted, false)); /* no connect, no retry */
    TEST_ASSERT_EQUAL_INT(WIFISCAN_CONN_SKIP, wifiscan_connect_act(true, sta_wanted, false));  /* nor after a scan */
    wfo_t w;
    memset(&w, 0, sizeof w);
    wfo_start(&w, 0, true);
    wfo_stop(&w); /* what netmgr does for a station that doesn't run */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, 60000)); /* no failover */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_scan_ended(&w, 60000));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_use(&w, 60000, WFO_NET_BACKUP)); /* Use: nothing runs */
    TEST_ASSERT_EQUAL_INT(WIFISCAN_RUN, wifiscan_decide(false, false, 0, 0)); /* a scan still runs */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ap_ip);
    RUN_TEST(test_addrs);
    RUN_TEST(test_eth_preferred);
    RUN_TEST(test_eth_link_no_ip_uses_wifi);
    RUN_TEST(test_nothing);
    RUN_TEST(test_eth_only);
    RUN_TEST(test_eth_ip_without_link_uses_wifi);
    RUN_TEST(test_wifi_ip_ignored_when_unconfigured);
    RUN_TEST(test_wifi_configured_no_ip);
    RUN_TEST(test_wifi_same_settings);
    RUN_TEST(test_wifi_ap_secured_changed);
    RUN_TEST(test_wifi_ap_pass_changed);
    RUN_TEST(test_wifi_station_changed);
    RUN_TEST(test_wifi_backup_changed);
    RUN_TEST(test_sta_changed);
    RUN_TEST(test_wifi_enabled_changed);
    RUN_TEST(test_station_off_decisions);
    return UNITY_END();
}
