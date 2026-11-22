#include <string.h>
#include "unity.h"
#include "discovery/discovery.h"

void setUp(void) {}
void tearDown(void) {}

static box_config_t cfg_auto(const char *pin, const char *host, uint16_t port)
{
    box_config_t c;
    memset(&c, 0, sizeof c);
    c.companion_auto = true;
    strcpy(c.companion_pin_id, pin);
    strcpy(c.companion_host, host);
    c.companion_port = port;
    return c;
}

static companion_instance_t inst(const char *id, const char *host, uint16_t port)
{
    companion_instance_t i;
    memset(&i, 0, sizeof i);
    strcpy(i.id, id);
    strcpy(i.host, host);
    i.port = port;
    return i;
}

static void test_select_manual(void)
{
    box_config_t c = cfg_auto("", "10.0.0.5", 16622);
    c.companion_auto = false;
    companion_instance_t l[1] = {inst("zzz", "1.2.3.4", 1)};
    char h[64], p[64];
    uint16_t port = 0;
    TEST_ASSERT_EQUAL_INT(SEL_CONNECT, discovery_select(&c, l, 1, h, sizeof h, &port, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", h);
    TEST_ASSERT_EQUAL_UINT16(16622, port);
    TEST_ASSERT_EQUAL_STRING("", p);
    c.companion_host[0] = 0;
    TEST_ASSERT_EQUAL_INT(SEL_NONE, discovery_select(&c, l, 1, h, sizeof h, &port, p, sizeof p));
}

static void test_select_single_auto_pins(void)
{
    box_config_t c = cfg_auto("", "", 16622);
    companion_instance_t l[1] = {inst("abc", "10.0.0.9", 16623)};
    char h[64], p[64] = "junk";
    uint16_t port = 0;
    TEST_ASSERT_EQUAL_INT(SEL_CONNECT, discovery_select(&c, l, 1, h, sizeof h, &port, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("10.0.0.9", h);
    TEST_ASSERT_EQUAL_UINT16(16623, port);
    TEST_ASSERT_EQUAL_STRING("abc", p);
}

static void test_select_ambiguous(void)
{
    box_config_t c = cfg_auto("", "", 16622);
    companion_instance_t l[2] = {inst("a", "1.1.1.1", 1), inst("b", "2.2.2.2", 2)};
    char h[64], p[64] = "junk";
    uint16_t port;
    TEST_ASSERT_EQUAL_INT(SEL_AMBIGUOUS, discovery_select(&c, l, 2, h, sizeof h, &port, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("", p);
}

static void test_select_none(void)
{
    box_config_t c = cfg_auto("", "", 16622);
    char h[64], p[64];
    uint16_t port;
    TEST_ASSERT_EQUAL_INT(SEL_NONE, discovery_select(&c, NULL, 0, h, sizeof h, &port, p, sizeof p));
}

static void test_select_pinned_follows_new_address(void)
{
    box_config_t c = cfg_auto("abc", "10.0.0.1", 16622);
    companion_instance_t l[2] = {inst("xyz", "9.9.9.9", 9), inst("abc", "10.0.0.77", 16700)};
    char h[64], p[64] = "junk";
    uint16_t port = 0;
    TEST_ASSERT_EQUAL_INT(SEL_CONNECT, discovery_select(&c, l, 2, h, sizeof h, &port, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("10.0.0.77", h);
    TEST_ASSERT_EQUAL_UINT16(16700, port);
    TEST_ASSERT_EQUAL_STRING("", p);
}

static void test_select_pinned_ignores_newcomer(void)
{
    box_config_t c = cfg_auto("abc", "10.0.0.1", 16622);
    companion_instance_t l[1] = {inst("xyz", "9.9.9.9", 9)};
    char h[64], p[64];
    uint16_t port = 0;
    TEST_ASSERT_EQUAL_INT(SEL_CONNECT, discovery_select(&c, l, 1, h, sizeof h, &port, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", h);
    TEST_ASSERT_EQUAL_UINT16(16622, port);
}

static void test_select_pinned_absent_no_host(void)
{
    box_config_t c = cfg_auto("abc", "", 16622);
    companion_instance_t l[1] = {inst("xyz", "9.9.9.9", 9)};
    char h[64], p[64];
    uint16_t port;
    TEST_ASSERT_EQUAL_INT(SEL_NONE, discovery_select(&c, l, 1, h, sizeof h, &port, p, sizeof p));
}

static void test_select_truncates_safely(void)
{
    box_config_t c = cfg_auto("", "", 16622);
    companion_instance_t l[1] = {inst("abcdefgh", "10.0.0.9", 1)};
    char h[4], p[4];
    uint16_t port;
    TEST_ASSERT_EQUAL_INT(SEL_CONNECT, discovery_select(&c, l, 1, h, sizeof h, &port, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("10.", h);
    TEST_ASSERT_EQUAL_STRING("abc", p);
}

/* Companion's Discover -> setup posts {"host", "port"}; a visible instance at that address is
 * pinned (auto, its id, its address as the last known one), so the box follows it to a new IP. */
static void test_posted_host_of_a_visible_instance_pins_it(void)
{
    box_config_t c = cfg_auto("", "192.168.8.114", 16622);
    c.companion_auto = false; /* what boxcfg_apply_satellite_config left */
    companion_instance_t l[2] = {inst("other", "192.168.8.50", 16622), inst("3GQD", "192.168.8.114", 16622)};
    TEST_ASSERT_TRUE(discovery_pin_posted(&c, l, 2, "192.168.8.114", 16622));
    TEST_ASSERT_TRUE(c.companion_auto);
    TEST_ASSERT_EQUAL_STRING("3GQD", c.companion_pin_id);
    TEST_ASSERT_EQUAL_STRING("192.168.8.114", c.companion_host);
    TEST_ASSERT_EQUAL_UINT16(16622, c.companion_port);
    /* ...and then follows it: the pinned instance's new address wins over the stored one */
    l[1] = inst("3GQD", "192.168.8.120", 16622);
    char h[64], p[64];
    uint16_t port = 0;
    TEST_ASSERT_EQUAL_INT(SEL_CONNECT, discovery_select(&c, l, 2, h, sizeof h, &port, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("192.168.8.120", h);
}

/* No port posted: any port of that host; the instance's port becomes the last known one. */
static void test_posted_host_without_port_takes_the_instance_port(void)
{
    box_config_t c = cfg_auto("", "10.0.0.7", 16622);
    c.companion_auto = false;
    companion_instance_t l[1] = {inst("abc", "10.0.0.7", 16700)};
    TEST_ASSERT_TRUE(discovery_pin_posted(&c, l, 1, "10.0.0.7", 0));
    TEST_ASSERT_EQUAL_STRING("abc", c.companion_pin_id);
    TEST_ASSERT_EQUAL_UINT16(16700, c.companion_port);
}

/* No match (another host, or the posted port differs): manual, the config untouched. */
static void test_posted_host_without_a_match_stays_manual(void)
{
    companion_instance_t l[1] = {inst("abc", "10.0.0.7", 16622)};
    box_config_t c = cfg_auto("", "10.0.0.8", 16622);
    c.companion_auto = false;
    box_config_t before = c;
    TEST_ASSERT_FALSE(discovery_pin_posted(&c, l, 1, "10.0.0.8", 16622));
    TEST_ASSERT_FALSE(discovery_pin_posted(&c, l, 1, "10.0.0.7", 17000));
    TEST_ASSERT_FALSE(discovery_pin_posted(&c, l, 1, "", 0));
    TEST_ASSERT_FALSE(discovery_pin_posted(&c, NULL, 0, "10.0.0.7", 0));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
}

/* Host names compare without case (mDNS gives IPv4 addresses; a posted name only matches a name). */
static void test_posted_host_compares_without_case(void)
{
    companion_instance_t l[1] = {inst("abc", "Companion.local", 16622)};
    box_config_t c = cfg_auto("", "companion.LOCAL", 16622);
    c.companion_auto = false;
    TEST_ASSERT_TRUE(discovery_pin_posted(&c, l, 1, "companion.LOCAL", 16622));
    TEST_ASSERT_EQUAL_STRING("Companion.local", c.companion_host);
}

/* a.b.c.d as the box holds it: the same order for the addresses, the box's own and its mask */
#define IP4(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

/* A Companion PC with several addresses (a VPN, a virtual adapter): the one the box can reach, on its own subnet */
static void test_pick_addr_prefers_the_boxes_subnet(void)
{
    const uint32_t addrs[] = {IP4(172, 29, 0, 1), IP4(192, 168, 1, 20), IP4(192, 168, 1, 21)};
    TEST_ASSERT_EQUAL(1, discovery_pick_addr(addrs, 3, IP4(192, 168, 1, 50), IP4(255, 255, 255, 0)));
    TEST_ASSERT_EQUAL(0, discovery_pick_addr(addrs, 3, IP4(172, 29, 5, 9), IP4(255, 255, 0, 0)));
    TEST_ASSERT_EQUAL(0, discovery_pick_addr(addrs, 1, IP4(192, 168, 1, 50), IP4(255, 255, 255, 0)));
}

/* None on the box's subnet (a routed Companion), or the subnet not known: the first, as before */
static void test_pick_addr_falls_back_to_the_first(void)
{
    const uint32_t addrs[] = {IP4(172, 29, 0, 1), IP4(192, 168, 1, 20)};
    TEST_ASSERT_EQUAL(0, discovery_pick_addr(addrs, 2, IP4(10, 0, 0, 5), IP4(255, 255, 255, 0)));
    TEST_ASSERT_EQUAL(0, discovery_pick_addr(addrs, 2, 0, 0));
    TEST_ASSERT_EQUAL(0, discovery_pick_addr(addrs, 2, IP4(192, 168, 1, 50), 0));
    TEST_ASSERT_EQUAL(0, discovery_pick_addr(addrs, 2, 0, IP4(255, 255, 255, 0)));
    TEST_ASSERT_EQUAL(-1, discovery_pick_addr(addrs, 0, IP4(192, 168, 1, 50), IP4(255, 255, 255, 0)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_select_manual);
    RUN_TEST(test_select_single_auto_pins);
    RUN_TEST(test_select_ambiguous);
    RUN_TEST(test_select_none);
    RUN_TEST(test_select_pinned_follows_new_address);
    RUN_TEST(test_select_pinned_ignores_newcomer);
    RUN_TEST(test_select_pinned_absent_no_host);
    RUN_TEST(test_select_truncates_safely);
    RUN_TEST(test_posted_host_of_a_visible_instance_pins_it);
    RUN_TEST(test_posted_host_without_port_takes_the_instance_port);
    RUN_TEST(test_posted_host_without_a_match_stays_manual);
    RUN_TEST(test_posted_host_compares_without_case);
    RUN_TEST(test_pick_addr_prefers_the_boxes_subnet);
    RUN_TEST(test_pick_addr_falls_back_to_the_first);
    return UNITY_END();
}
