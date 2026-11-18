/* The Ethernet receive filter (components/netmgr/rxfilter.c): unicast always passes; of broadcast
 * only ARP and DHCP replies, of multicast only mDNS, IGMP and IPv6; each kind within its own budget. */
#include <string.h>

#include "netmgr/rxfilter.h"
#include "unity.h"

#define MS(n) ((int64_t)(n) * 1000)

void setUp(void) {}
void tearDown(void) {}

static const uint8_t BOX_MAC[6] = {0x30, 0xed, 0xa0, 0x01, 0x02, 0x03};
static const uint8_t BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t MDNS_MAC[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};
static const uint8_t OWN_IP[4] = {192, 168, 1, 50};

static uint32_t own_ip(void)
{
    uint32_t v;
    memcpy(&v, OWN_IP, 4);
    return v;
}

static uint32_t eth(uint8_t *f, const uint8_t *dst, uint16_t et)
{
    memset(f, 0, 128);
    memcpy(f, dst, 6);
    f[6] = 0x02; /* a source MAC */
    f[12] = (uint8_t)(et >> 8);
    f[13] = (uint8_t)et;
    return 14;
}

/* an IPv4 UDP datagram to dst_ip:port with "Hello, broadcast!" as payload */
static uint32_t udp(uint8_t *f, const uint8_t *dst_mac, const uint8_t dst_ip[4], uint16_t port)
{
    uint32_t n = eth(f, dst_mac, 0x0800);
    uint8_t *ip = f + n;
    ip[0] = 0x45;
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, (uint8_t[]){192, 168, 1, 9}, 4);
    memcpy(ip + 16, dst_ip, 4);
    uint8_t *u = ip + 20;
    u[0] = 0xc0;
    u[1] = 0x00;
    u[2] = (uint8_t)(port >> 8);
    u[3] = (uint8_t)port;
    memcpy(u + 8, "Hello, broadcast!", 17);
    return n + 20 + 8 + 17;
}

static uint32_t arp_for(uint8_t *f, const uint8_t target[4])
{
    uint32_t n = eth(f, BCAST, 0x0806);
    uint8_t *a = f + n;
    a[1] = 1;       /* Ethernet */
    a[2] = 0x08;    /* IPv4 */
    a[4] = 6;
    a[5] = 4;
    a[7] = 1;       /* request */
    memcpy(a + 24, target, 4);
    return n + 28;
}

static rxf_t fresh(void)
{
    rxf_t f;
    rxf_init(&f);
    return f;
}

static void test_unicast_always_passes(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = udp(fr, BOX_MAC, OWN_IP, 5000);
    for (int i = 0; i < 10000; i++) TEST_ASSERT_EQUAL_INT(RXF_PASS, rxf_check(&f, fr, n, own_ip(), 0));
}

static void test_broadcast_udp_to_a_port_nobody_uses_is_dropped(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = udp(fr, BCAST, (uint8_t[]){255, 255, 255, 255}, 5000);
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n, own_ip(), 0));
    n = udp(fr, BCAST, (uint8_t[]){192, 168, 1, 255}, 5000); /* the subnet's broadcast too */
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n, own_ip(), 0));
    n = udp(fr, BCAST, (uint8_t[]){255, 255, 255, 255}, 67); /* to a DHCP server: not the box */
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n, own_ip(), 0));
}

static void test_dhcp_reply_passes(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = udp(fr, BCAST, (uint8_t[]){255, 255, 255, 255}, 68);
    TEST_ASSERT_EQUAL_INT(RXF_PASS, rxf_check(&f, fr, n, own_ip(), 0));
}

static void test_later_fragment_and_short_header_are_dropped(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = udp(fr, BCAST, (uint8_t[]){255, 255, 255, 255}, 68);
    fr[14 + 6] = 0x00;
    fr[14 + 7] = 0x10; /* fragment offset 16: no UDP header in it */
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n, own_ip(), 0));
    n = udp(fr, BCAST, (uint8_t[]){255, 255, 255, 255}, 68);
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, 14 + 20 + 2, own_ip(), 0));
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, 20, own_ip(), 0));
    fr[14] = 0x4f; /* IHL 60 bytes, longer than the frame */
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, 40, own_ip(), 0));
}

static void test_other_broadcast_ethertypes_are_dropped(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = eth(fr, BCAST, 0x8100); /* VLAN-tagged: lwIP has no VLAN support here */
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n + 40, own_ip(), 0));
    n = eth(fr, BCAST, 0x88cc);
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n + 40, own_ip(), 0));
}

static void test_mdns_and_igmp_pass_other_multicast_does_not(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = udp(fr, MDNS_MAC, (uint8_t[]){224, 0, 0, 251}, 5353);
    TEST_ASSERT_EQUAL_INT(RXF_PASS, rxf_check(&f, fr, n, own_ip(), 0));
    fr[14 + 9] = 2; /* IGMP */
    TEST_ASSERT_EQUAL_INT(RXF_PASS, rxf_check(&f, fr, n, own_ip(), 0));
    /* SSDP, 239.255.255.250:1900 */
    n = udp(fr, (uint8_t[]){0x01, 0x00, 0x5e, 0x7f, 0xff, 0xfa}, (uint8_t[]){239, 255, 255, 250}, 1900);
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n, own_ip(), 0));
    /* the mDNS group but another port */
    n = udp(fr, MDNS_MAC, (uint8_t[]){224, 0, 0, 251}, 5000);
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n, own_ip(), 0));
    /* spanning tree */
    n = eth(fr, (uint8_t[]){0x01, 0x80, 0xc2, 0x00, 0x00, 0x00}, 0x0026);
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n + 40, own_ip(), 0));
}

static void test_ipv6_multicast_passes(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = eth(fr, (uint8_t[]){0x33, 0x33, 0xff, 0x01, 0x02, 0x03}, 0x86dd);
    TEST_ASSERT_EQUAL_INT(RXF_PASS, rxf_check(&f, fr, n + 40, own_ip(), 0));
    eth(fr, (uint8_t[]){0x33, 0x33, 0x00, 0x00, 0x00, 0x01}, 0x0800); /* 33:33 but not IPv6 */
    TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n + 40, own_ip(), 0));
}

static int passes(rxf_t *f, const uint8_t *fr, uint32_t n, int count, int64_t now)
{
    int p = 0;
    for (int i = 0; i < count; i++) p += rxf_check(f, fr, n, own_ip(), now) == RXF_PASS;
    return p;
}

static void test_arp_storm_is_bounded_and_keeps_arp_for_the_box(void)
{
    rxf_t f = fresh();
    uint8_t other[128], ours[128];
    uint32_t no = arp_for(other, (uint8_t[]){192, 168, 1, 77});
    uint32_t nu = arp_for(ours, OWN_IP);
    /* a burst of 1000 ARP for other hosts in one instant: the burst passes, then nothing */
    TEST_ASSERT_EQUAL_INT(RXF_ARP_BURST, passes(&f, other, no, 1000, 0));
    TEST_ASSERT_EQUAL_INT(RXF_DROP_RATE, rxf_check(&f, other, no, own_ip(), 0));
    /* the requests for the box still pass: their budget is their own */
    TEST_ASSERT_EQUAL_INT(RXF_PASS, rxf_check(&f, ours, nu, own_ip(), 0));
    /* the storm goes on at 1 frame per 100 us for 1 s: the rate passes, no more */
    int p = 0;
    for (int i = 1; i <= 10000; i++) p += rxf_check(&f, other, no, own_ip(), (int64_t)i * 100) == RXF_PASS;
    TEST_ASSERT_INT_WITHIN(1, RXF_ARP_RATE, p);
    /* with no address yet, every ARP is someone else's: one budget for all of it */
    rxf_t g = fresh();
    int q = 0;
    for (int i = 0; i < 1000; i++) q += rxf_check(&g, ours, nu, 0, 0) == RXF_PASS;
    TEST_ASSERT_EQUAL_INT(RXF_ARP_BURST, q);
}

static void test_budgets_refill_and_are_separate(void)
{
    rxf_t f = fresh();
    uint8_t m[128], d[128];
    uint32_t nm = udp(m, MDNS_MAC, (uint8_t[]){224, 0, 0, 251}, 5353);
    uint32_t nd = udp(d, BCAST, (uint8_t[]){255, 255, 255, 255}, 68);
    TEST_ASSERT_EQUAL_INT(RXF_MDNS_BURST, passes(&f, m, nm, 500, MS(0)));
    TEST_ASSERT_EQUAL_INT(RXF_DHCP_BURST, passes(&f, d, nd, 500, MS(0))); /* mDNS used none of it */
    /* 100 ms later: 30 mDNS frames' worth (300 a second) */
    TEST_ASSERT_EQUAL_INT(RXF_MDNS_RATE / 10, passes(&f, m, nm, 500, MS(100)));
    /* long quiet: back to the burst, no more */
    TEST_ASSERT_EQUAL_INT(RXF_MDNS_BURST, passes(&f, m, nm, 500, MS(60000)));
    /* a clock that goes back gives nothing extra */
    TEST_ASSERT_EQUAL_INT(0, passes(&f, m, nm, 500, MS(1000)));
}

static void test_all_budgets_together_stay_under_1000_a_second(void)
{
    int sum = RXF_ARP_US_RATE + RXF_ARP_RATE + RXF_DHCP_RATE + RXF_MDNS_RATE + RXF_V6_RATE;
    TEST_ASSERT_LESS_THAN_INT(1000, sum);
}

static void test_hello_flood_drops_all_and_is_reported(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = udp(fr, BCAST, (uint8_t[]){255, 255, 255, 255}, 5000);
    uint8_t uc[128];
    uint32_t nu = udp(uc, BOX_MAC, OWN_IP, 5000);
    rxf_stats_t st;
    bool ended = false;
    /* 15,000 a second for 10 s, as Packet Sender at ~1 MB/s, and Companion's own frames among them */
    int64_t t = 0;
    for (int i = 0; i < 160000 && !ended; i++, t += 66) {
        TEST_ASSERT_EQUAL_INT(RXF_DROP_UNWANTED, rxf_check(&f, fr, n, own_ip(), t));
        if (i % 15 == 0) TEST_ASSERT_EQUAL_INT(RXF_PASS, rxf_check(&f, uc, nu, own_ip(), t));
        ended = rxf_report(&f, t, &st);
    }
    TEST_ASSERT_TRUE(ended);
    TEST_ASSERT_TRUE(rxf_worth_logging(&st));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(RXF_REPORT_UNWANTED, st.unwanted);
    TEST_ASSERT_UINT32_WITHIN(2, 10101, st.unicast); /* 10 s / 66 us / 15 */
    TEST_ASSERT_EQUAL_UINT32(0, st.rate);
    /* the counts start again */
    TEST_ASSERT_FALSE(rxf_report(&f, t + 1, &st));
    TEST_ASSERT_TRUE(rxf_report(&f, t + RXF_REPORT_US, &st));
    TEST_ASSERT_EQUAL_UINT32(0, st.unwanted);
}

static void test_ordinary_lan_chatter_is_not_reported(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = udp(fr, BCAST, (uint8_t[]){192, 168, 1, 255}, 137); /* NetBIOS, a few a second */
    rxf_stats_t st;
    for (int i = 0; i <= 50; i++) rxf_check(&f, fr, n, own_ip(), MS(i * 200));
    TEST_ASSERT_TRUE(rxf_report(&f, MS(10000), &st));
    TEST_ASSERT_FALSE(rxf_worth_logging(&st));
    TEST_ASSERT_EQUAL_UINT32(51, st.unwanted);
}

/* eth.c's clock is 32 bits of milliseconds: it starts again at 0 every 49.7 days, and the reports go on */
static void test_reports_go_on_after_the_clock_wraps(void)
{
    rxf_t f = fresh();
    uint8_t fr[128];
    uint32_t n = udp(fr, BCAST, (uint8_t[]){255, 255, 255, 255}, 5000);
    rxf_stats_t st;
    const int64_t wrap = (int64_t)UINT32_MAX * 1000;
    rxf_check(&f, fr, n, own_ip(), wrap - MS(20000));
    TEST_ASSERT_TRUE(rxf_report(&f, wrap - MS(5000), &st)); /* a window ends just before the wrap */
    rxf_check(&f, fr, n, own_ip(), MS(100));                /* the clock at 0 again */
    TEST_ASSERT_FALSE(rxf_report(&f, MS(100), &st));
    TEST_ASSERT_TRUE(rxf_report(&f, MS(100) + RXF_REPORT_US, &st));
    TEST_ASSERT_EQUAL_UINT32(1, st.unwanted);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unicast_always_passes);
    RUN_TEST(test_broadcast_udp_to_a_port_nobody_uses_is_dropped);
    RUN_TEST(test_dhcp_reply_passes);
    RUN_TEST(test_later_fragment_and_short_header_are_dropped);
    RUN_TEST(test_other_broadcast_ethertypes_are_dropped);
    RUN_TEST(test_mdns_and_igmp_pass_other_multicast_does_not);
    RUN_TEST(test_ipv6_multicast_passes);
    RUN_TEST(test_arp_storm_is_bounded_and_keeps_arp_for_the_box);
    RUN_TEST(test_budgets_refill_and_are_separate);
    RUN_TEST(test_all_budgets_together_stay_under_1000_a_second);
    RUN_TEST(test_hello_flood_drops_all_and_is_reported);
    RUN_TEST(test_ordinary_lan_chatter_is_not_reported);
    RUN_TEST(test_reports_go_on_after_the_clock_wraps);
    return UNITY_END();
}
