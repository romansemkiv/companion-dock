#include <string.h>

#include "netmgr/wififo_logic.h"
#include "unity.h"

#define S(n) ((int64_t)(n) * 1000)

void setUp(void) {}
void tearDown(void) {}

/* associated and got an address: joined */
static void join(wfo_t *w, int64_t t)
{
    wfo_connected(w, t);
    wfo_got_ip(w, t);
}

static wfo_t started(bool backup)
{
    wfo_t w;
    memset(&w, 0, sizeof w);
    wfo_start(&w, 0, backup);
    return w;
}

static void test_starts_with_main(void)
{
    wfo_t w;
    memset(&w, 0, sizeof w);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_start(&w, S(100), true));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    TEST_ASSERT_FALSE(wfo_joined(&w));
    /* even when a previous run ended on the backup */
    w.net = WFO_NET_BACKUP;
    wfo_start(&w, S(200), true);
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
}

static void test_alternates_every_30s_while_neither_joins(void)
{
    wfo_t w = started(true);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(29)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, 29999));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(30)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(59)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_tick(&w, S(60)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(89)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(90)));
    /* failed attempts in between do not move the clock */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(95), 201));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(119)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_tick(&w, S(120)));
}

static void test_joined_backup_stays(void)
{
    wfo_t w = started(true);
    wfo_tick(&w, S(30)); /* now on the backup */
    join(&w, S(33));
    TEST_ASSERT_TRUE(wfo_joined(&w));
    for (int64_t t = 34; t < 4 * 3600; t += 7) TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(t)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_TRUE(wfo_joined(&w));
}

static void test_joined_main_stays(void)
{
    wfo_t w = started(true);
    join(&w, S(2));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(31)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(1000)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
}

static void test_backup_lost_tries_main_first(void)
{
    wfo_t w = started(true);
    wfo_tick(&w, S(30));
    join(&w, S(32));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_disconnected(&w, S(500), 8));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    TEST_ASSERT_FALSE(wfo_joined(&w));
    /* the main one then gets its 30 s before the backup is tried again */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(529)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(530)));
}

static void test_main_lost_keeps_main_for_30s_then_backup(void)
{
    wfo_t w = started(true);
    join(&w, S(2));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(1000), 8));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(1029)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(1030)));
}

static void test_empty_backup_never_switches(void)
{
    wfo_t w = started(false);
    for (int64_t t = 1; t < 900; t++) {
        TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(t)));
        if (t % 11 == 0) TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(t), 201));
    }
    join(&w, S(900));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(950), 8)); /* the glue's 5 s retry follows */
    wfo_scan_started(&w);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_scan_ended(&w, S(2000)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
}

static void test_config_change_starts_from_main(void)
{
    wfo_t w = started(true);
    wfo_tick(&w, S(30));
    join(&w, S(31));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_start(&w, S(100), true));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    TEST_ASSERT_FALSE(wfo_joined(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(129)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(130)));
    /* the backup removed: main only from then on */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_start(&w, S(200), false));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(400)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
}

/* a restart for a hotspot-only change (the hotspot on or off, forced, its password) keeps the network
 * the station was on: on the backup it stays. Only a change to the station's own
 * networks starts again from the main one. */
static void test_hotspot_change_keeps_backup(void)
{
    wfo_t w = started(true);
    wfo_tick(&w, S(30)); /* the main network not joined in 30 s: the backup */
    join(&w, S(31));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_restart_net(&w, true));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_start_on(&w, S(100), true, WFO_NET_BACKUP));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_FALSE(wfo_joined(&w));
    join(&w, S(103));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(200))); /* joined again: it stays */
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    /* not joined yet at the restart, the backup being tried: the backup again, its 30 s from now */
    wfo_disconnected(&w, S(210), 201);
    wfo_start_on(&w, S(220), true, WFO_NET_MAIN);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(250)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_restart_net(&w, true));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_start_on(&w, S(255), true, wfo_restart_net(&w, true)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(284)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_tick(&w, S(285)));
    /* on the main network: the main again */
    join(&w, S(286));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_restart_net(&w, true));
    /* a change to either pair: the main network */
    wfo_start_on(&w, S(300), true, WFO_NET_BACKUP);
    join(&w, S(301));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_restart_net(&w, false));
    /* the station stopped (the radio lost, Wi-Fi off) or no backup any more: the main network */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_start_on(&w, S(400), false, WFO_NET_BACKUP));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    wfo_start_on(&w, S(500), true, WFO_NET_BACKUP);
    wfo_stop(&w);
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_restart_net(&w, true));
}

/* "not found" only when the driver reported no such network on that network's last attempt; any
 * other failure (a wrong password) means it was found; joining it, or a restart, forgets it. */
static void test_not_found_only_when_reported(void)
{
    wfo_t w = started(true);
    TEST_ASSERT_FALSE(wfo_not_found(&w, WFO_NET_MAIN));
    TEST_ASSERT_FALSE(wfo_not_found(&w, WFO_NET_BACKUP));
    wfo_note_attempt(&w, WFO_NET_MAIN, 201); /* WIFI_REASON_NO_AP_FOUND */
    TEST_ASSERT_TRUE(wfo_not_found(&w, WFO_NET_MAIN));
    TEST_ASSERT_FALSE(wfo_not_found(&w, WFO_NET_BACKUP));
    wfo_note_attempt(&w, WFO_NET_MAIN, 15); /* 4-way handshake timeout: there, the password is wrong */
    TEST_ASSERT_FALSE(wfo_not_found(&w, WFO_NET_MAIN));
    static const int NOT_FOUND[] = {201, 210, 211, 212};
    for (size_t i = 0; i < sizeof NOT_FOUND / sizeof NOT_FOUND[0]; i++) {
        wfo_note_attempt(&w, WFO_NET_BACKUP, 2); /* auth expired: found */
        TEST_ASSERT_FALSE(wfo_not_found(&w, WFO_NET_BACKUP));
        wfo_note_attempt(&w, WFO_NET_BACKUP, NOT_FOUND[i]);
        TEST_ASSERT_TRUE(wfo_not_found(&w, WFO_NET_BACKUP));
    }
    wfo_note_attempt(&w, WFO_NET_MAIN, 201);
    wfo_tick(&w, S(30)); /* on to the backup, which then associates */
    wfo_connected(&w, S(31));
    TEST_ASSERT_FALSE(wfo_not_found(&w, WFO_NET_BACKUP));
    TEST_ASSERT_TRUE(wfo_not_found(&w, WFO_NET_MAIN));
    wfo_start(&w, S(40), true);
    TEST_ASSERT_FALSE(wfo_not_found(&w, WFO_NET_MAIN));
    wfo_stop(&w);
    wfo_note_attempt(&w, WFO_NET_MAIN, 201); /* stopped: nothing noted */
    TEST_ASSERT_FALSE(wfo_not_found(&w, WFO_NET_MAIN));
}

/* a network chosen by hand: joined now, then treated as the one in use: kept while it works; on a
 * loss tried first (for WFO_SWITCH_MS), then the other; a change to the networks starts from the main. */
static void test_use_by_hand(void)
{
    wfo_t w = started(true);
    join(&w, S(1));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_USE_BACKUP, wfo_use(&w, S(10), WFO_NET_BACKUP));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_FALSE(wfo_joined(&w)); /* dropped the main on purpose */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_use(&w, S(11), WFO_NET_BACKUP)); /* already being joined */
    join(&w, S(12));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(500))); /* stays while it works */
    /* lost: the backup first, for 30 s, then the main */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(600), 8));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(629)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_tick(&w, S(630)));
    /* joined the main instead, then lost: the backup (the chosen one) at once */
    join(&w, S(631));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_disconnected(&w, S(700), 8));
    /* back to the main by hand: then as before (a main loss keeps the main 30 s) */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_USE_MAIN, wfo_use(&w, S(710), WFO_NET_MAIN));
    join(&w, S(711));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(800), 8));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    /* the backup by hand, then a change to the networks: from the main, and a loss on the backup tries the main */
    wfo_use(&w, S(900), WFO_NET_BACKUP);
    wfo_start(&w, S(910), true);
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    wfo_tick(&w, S(940));
    join(&w, S(941));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_disconnected(&w, S(1000), 8));
    /* a hotspot-only restart on the backup chosen by hand keeps the choice */
    wfo_use(&w, S(1100), WFO_NET_BACKUP);
    join(&w, S(1101));
    wfo_start_on(&w, S(1200), true, wfo_restart_net(&w, true));
    join(&w, S(1201));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(1300), 8)); /* the backup first, still */
    /* no backup set, or the station stopped: nothing */
    wfo_t m = started(false);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_use(&m, S(1), WFO_NET_BACKUP));
    wfo_stop(&m);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_use(&m, S(2), WFO_NET_MAIN));
}

/* A switch by hand during a scan waits for its end (a scan and a switch never run at the same
 * time), and is still a switch by hand then; Use on the network already tried keeps it as the one
 * in use. */
static void test_use_during_scan_waits(void)
{
    wfo_t w = started(true);
    join(&w, S(1));
    wfo_scan_started(&w);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_use(&w, S(5), WFO_NET_BACKUP)); /* nothing while it scans */
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w)); /* not named before the switch starts */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_got_ip(&w, S(6))); /* a lease renewal meanwhile: still waiting */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(7)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_USE_BACKUP, wfo_scan_ended(&w, S(8))); /* the switch, by hand, now */
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_FALSE(wfo_joined(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_scan_ended(&w, S(9))); /* once */
    join(&w, S(10));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(100), 8)); /* the backup first after a loss */
    /* a CONNECTED during the scan (the station rejoined the main) never turns into a needless drop
     * and rejoin of the same network: the switch at the end goes to the network chosen, once */
    wfo_t c = started(true);
    join(&c, S(1));
    wfo_scan_started(&c);
    wfo_disconnected(&c, S(2), 8); /* the main lost during the scan: tried again (it is the first choice) */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_use(&c, S(3), WFO_NET_MAIN)); /* the main by hand: it is the one */
    join(&c, S(4)); /* CONNECTED, GOT_IP during the scan */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_scan_ended(&c, S(5))); /* no drop and rejoin of the main */
    TEST_ASSERT_TRUE(wfo_joined(&c));
    wfo_scan_started(&c);
    wfo_use(&c, S(6), WFO_NET_BACKUP);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_use(&c, S(7), WFO_NET_MAIN)); /* changed its mind: back to the main */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_scan_ended(&c, S(8))); /* nothing to do */
    /* Use on the network being tried already: no reconnect, but it is the one in use */
    wfo_t m = started(true);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&m, S(30))); /* the main failed: the backup tried */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_use(&m, S(31), WFO_NET_BACKUP));
    join(&m, S(32));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&m, S(100), 8)); /* lost: the backup again first */
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&m));
}

static void test_scan_defers_a_due_switch(void)
{
    wfo_t w = started(true);
    wfo_scan_started(&w);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(30)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(45)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_scan_ended(&w, S(46)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    /* a scan that ends before anything is due changes nothing */
    wfo_scan_started(&w);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_scan_ended(&w, S(50)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(75)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_tick(&w, S(76)));
}

static void test_loss_during_scan_waits_for_it(void)
{
    wfo_t w = started(true);
    wfo_tick(&w, S(30));
    join(&w, S(31)); /* joined the backup */
    wfo_scan_started(&w);
    /* the scan drops the link: main first, but only once the scan is over */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(32), 8));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_scan_ended(&w, S(38)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(67)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(68)));
}

static void test_wrong_password_counts_like_not_found(void)
{
    wfo_t w = started(true), v = started(true);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(5), 15));   /* 4-way handshake timeout */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(10), 202)); /* auth fail */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&v, S(5), 201));  /* no AP found */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&v, S(10), 201));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(30)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&v, S(30)));
}

static void test_associated_without_ip_fails_over(void)
{
    wfo_t w = started(true);
    wfo_connected(&w, S(2)); /* associates, but no address */
    TEST_ASSERT_FALSE(wfo_joined(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(29)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(30)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
}

static void test_got_ip_stays(void)
{
    wfo_t w = started(true);
    wfo_connected(&w, S(2));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_got_ip(&w, S(10)));
    TEST_ASSERT_TRUE(wfo_joined(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(500)));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
}

static void test_lost_ip_counts_as_loss(void)
{
    wfo_t w = started(true);
    wfo_tick(&w, S(30));
    join(&w, S(31)); /* on the backup */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_lost_ip(&w, S(100)));
    TEST_ASSERT_FALSE(wfo_joined(&w));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    /* on the main network: kept for 30 s from the loss */
    wfo_t m = started(true);
    join(&m, S(1));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_lost_ip(&m, S(100)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&m, S(129)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&m, S(130)));
}

static void test_disconnect_while_associated_only_is_no_loss(void)
{
    wfo_t w = started(true);
    wfo_tick(&w, S(30)); /* on the backup */
    wfo_connected(&w, S(31)); /* no address yet */
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(40), 8)); /* not joined: no main-first jump */
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_MAIN, wfo_tick(&w, S(60)));
}

/* the glue drops an attempt before a switch: its DISCONNECTED must not arm the 5 s retry (which would
 * connect in the middle of the new attempt) nor count as a loss */
static void test_own_disconnect_is_not_a_retry_or_a_loss(void)
{
    wfo_t w = started(true);
    TEST_ASSERT_TRUE(wfo_retry_ok(&w));
    TEST_ASSERT_FALSE(wfo_own_disconnect(&w)); /* an ordinary failed attempt */
    wfo_tick(&w, S(30));
    wfo_disconnect_requested(&w, S(30)); /* switch_to's esp_wifi_disconnect */
    TEST_ASSERT_FALSE(wfo_retry_ok(&w)); /* retry_task waits while the event is due */
    TEST_ASSERT_TRUE(wfo_own_disconnect(&w));
    TEST_ASSERT_TRUE(wfo_retry_ok(&w));
    TEST_ASSERT_FALSE(wfo_own_disconnect(&w)); /* only once per request */
    /* a joined link dropped on purpose does not jump to the main network */
    join(&w, S(31));
    wfo_disconnect_requested(&w, S(32));
    TEST_ASSERT_TRUE(wfo_own_disconnect(&w));
    TEST_ASSERT_EQUAL_INT(WFO_NET_BACKUP, wfo_net(&w));
    TEST_ASSERT_FALSE(wfo_joined(&w));
    /* a request whose esp_wifi_disconnect failed raises no event */
    wfo_disconnect_requested(&w, S(33));
    wfo_disconnect_failed(&w);
    TEST_ASSERT_TRUE(wfo_retry_ok(&w));
    /* a new start forgets requests */
    wfo_disconnect_requested(&w, S(34));
    wfo_start(&w, S(100), true);
    TEST_ASSERT_TRUE(wfo_retry_ok(&w));
}

/* esp_wifi_disconnect succeeded but raised no event: the expectation must not stick */
static void test_expected_disconnect_that_never_comes_is_dropped(void)
{
    wfo_t w = started(true);
    wfo_tick(&w, S(30));
    wfo_disconnect_requested(&w, S(30));
    wfo_tick(&w, S(35));
    TEST_ASSERT_FALSE(wfo_retry_ok(&w));
    wfo_tick(&w, S(40)); /* WFO_DISC_WAIT_MS after the request */
    TEST_ASSERT_TRUE(wfo_retry_ok(&w));
    TEST_ASSERT_FALSE(wfo_own_disconnect(&w));
    /* or by the next association: the own DISCONNECTED would have come before it */
    wfo_disconnect_requested(&w, S(41));
    wfo_connected(&w, S(42));
    TEST_ASSERT_TRUE(wfo_retry_ok(&w));
}

/* A tick flipped the network, but the old one associated before the switch was applied: the glue puts the
 * logic back on the network its station config really holds */
static void test_set_net_follows_the_real_connection(void)
{
    wfo_t w = started(true);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_CONNECT_BACKUP, wfo_tick(&w, S(30)));
    wfo_set_net(&w, WFO_NET_MAIN);
    join(&w, S(31));
    TEST_ASSERT_EQUAL_INT(WFO_NET_MAIN, wfo_net(&w));
    TEST_ASSERT_TRUE(wfo_joined(&w));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(500)));
}

static void test_stopped_does_nothing(void)
{
    wfo_t w = started(true);
    wfo_stop(&w);
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_tick(&w, S(100)));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_disconnected(&w, S(100), 8));
    TEST_ASSERT_EQUAL_INT(WFO_ACT_NONE, wfo_scan_ended(&w, S(100)));
    TEST_ASSERT_FALSE(wfo_joined(&w));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_with_main);
    RUN_TEST(test_alternates_every_30s_while_neither_joins);
    RUN_TEST(test_joined_backup_stays);
    RUN_TEST(test_joined_main_stays);
    RUN_TEST(test_backup_lost_tries_main_first);
    RUN_TEST(test_main_lost_keeps_main_for_30s_then_backup);
    RUN_TEST(test_empty_backup_never_switches);
    RUN_TEST(test_config_change_starts_from_main);
    RUN_TEST(test_hotspot_change_keeps_backup);
    RUN_TEST(test_not_found_only_when_reported);
    RUN_TEST(test_use_by_hand);
    RUN_TEST(test_use_during_scan_waits);
    RUN_TEST(test_scan_defers_a_due_switch);
    RUN_TEST(test_loss_during_scan_waits_for_it);
    RUN_TEST(test_wrong_password_counts_like_not_found);
    RUN_TEST(test_associated_without_ip_fails_over);
    RUN_TEST(test_got_ip_stays);
    RUN_TEST(test_lost_ip_counts_as_loss);
    RUN_TEST(test_disconnect_while_associated_only_is_no_loss);
    RUN_TEST(test_own_disconnect_is_not_a_retry_or_a_loss);
    RUN_TEST(test_expected_disconnect_that_never_comes_is_dropped);
    RUN_TEST(test_set_net_follows_the_real_connection);
    RUN_TEST(test_stopped_does_nothing);
    return UNITY_END();
}
