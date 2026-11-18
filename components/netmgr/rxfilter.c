#include "netmgr/rxfilter.h"

#include <string.h>

#define ETH_HDR 14
#define ET_IPV4 0x0800
#define ET_ARP 0x0806
#define ET_IPV6 0x86DD
#define IPP_IGMP 2
#define IPP_UDP 17
#define PORT_DHCP_CLIENT 68
#define PORT_MDNS 5353
#define FRAME_US 1000000LL /* one frame in a bucket's units */

static const struct {
    int32_t rate, burst;
} k_budget[RXF_K_COUNT] = {
    [RXF_K_ARP_US] = {RXF_ARP_US_RATE, RXF_ARP_US_BURST},
    [RXF_K_ARP] = {RXF_ARP_RATE, RXF_ARP_BURST},
    [RXF_K_DHCP] = {RXF_DHCP_RATE, RXF_DHCP_BURST},
    [RXF_K_MDNS] = {RXF_MDNS_RATE, RXF_MDNS_BURST},
    [RXF_K_V6] = {RXF_V6_RATE, RXF_V6_BURST},
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

void rxf_init(rxf_t *f)
{
    memset(f, 0, sizeof *f);
}

static void start(rxf_t *f, int64_t now_us)
{
    for (int k = 0; k < RXF_K_COUNT; k++) {
        f->b[k].tokens = (int64_t)k_budget[k].burst * FRAME_US;
        f->b[k].last_us = now_us;
    }
    f->window_us = now_us;
    f->started = true;
}

static bool spend(rxf_t *f, rxf_kind_t k, int64_t now_us)
{
    rxf_bucket_t *b = &f->b[k];
    int64_t full = (int64_t)k_budget[k].burst * FRAME_US;
    int64_t dt = now_us - b->last_us;
    if (dt > 0) {
        if (dt > 10 * FRAME_US) dt = 10 * FRAME_US; /* full long before that; keeps the product small */
        b->tokens += dt * k_budget[k].rate;
        if (b->tokens > full) b->tokens = full;
    }
    b->last_us = now_us; /* a clock that went back starts the refill again from here */
    if (b->tokens < FRAME_US) return false;
    b->tokens -= FRAME_US;
    return true;
}

/* The IPv4 header after the Ethernet header: its protocol and where its payload starts, or false
 * for a short, malformed or non-first-fragment packet (a later fragment has no UDP header). */
static bool ipv4(const uint8_t *frame, uint32_t len, uint8_t *proto, uint32_t *payload)
{
    if (len < ETH_HDR + 20) return false;
    const uint8_t *ip = frame + ETH_HDR;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || len < ETH_HDR + ihl) return false;
    if ((be16(ip + 6) & 0x1fff) != 0) return false; /* fragment offset */
    *proto = ip[9];
    *payload = ETH_HDR + ihl;
    return true;
}

static bool udp_to(const uint8_t *frame, uint32_t len, uint32_t payload, uint16_t port)
{
    return len >= payload + 8 && be16(frame + payload + 2) == port;
}

/* The kind of a broadcast or multicast frame that may pass, or RXF_K_COUNT for none. */
static rxf_kind_t kind_of(const uint8_t *frame, uint32_t len, uint32_t own_ip)
{
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint16_t et = be16(frame + 12);
    uint8_t proto;
    uint32_t pl;

    if (memcmp(frame, bcast, 6) == 0) {
        if (et == ET_ARP) {
            /* ARP for IPv4 over Ethernet: the target protocol address at 24..27 of the ARP packet */
            if (own_ip != 0 && len >= ETH_HDR + 28 && memcmp(frame + ETH_HDR + 24, &own_ip, 4) == 0)
                return RXF_K_ARP_US;
            return RXF_K_ARP;
        }
        if (et == ET_IPV4 && ipv4(frame, len, &proto, &pl) && proto == IPP_UDP &&
            udp_to(frame, len, pl, PORT_DHCP_CLIENT))
            return RXF_K_DHCP;
        return RXF_K_COUNT;
    }
    if (frame[0] == 0x01 && frame[1] == 0x00 && frame[2] == 0x5e) { /* IPv4 multicast */
        if (et != ET_IPV4 || !ipv4(frame, len, &proto, &pl)) return RXF_K_COUNT;
        if (proto == IPP_IGMP) return RXF_K_MDNS;
        static const uint8_t mdns_ip[4] = {224, 0, 0, 251};
        if (proto == IPP_UDP && memcmp(frame + ETH_HDR + 16, mdns_ip, 4) == 0 &&
            udp_to(frame, len, pl, PORT_MDNS))
            return RXF_K_MDNS;
        return RXF_K_COUNT;
    }
    if (frame[0] == 0x33 && frame[1] == 0x33 && et == ET_IPV6) return RXF_K_V6;
    return RXF_K_COUNT;
}

rxf_verdict_t rxf_check(rxf_t *f, const uint8_t *frame, uint32_t len, uint32_t own_ip, int64_t now_us)
{
    if (!f->started) start(f, now_us);
    if (len < ETH_HDR) return RXF_PASS; /* lwIP drops a runt itself; nothing here to judge it by */
    if (!(frame[0] & 0x01)) {
        f->st.unicast++;
        return RXF_PASS;
    }

    rxf_kind_t k = kind_of(frame, len, own_ip);
    if (k == RXF_K_COUNT) {
        f->st.unwanted++;
        return RXF_DROP_UNWANTED;
    }
    if (!spend(f, k, now_us)) {
        f->st.rate++;
        return RXF_DROP_RATE;
    }
    return RXF_PASS;
}

bool rxf_report(rxf_t *f, int64_t now_us, rxf_stats_t *out)
{
    if (!f->started) return false;
    if (now_us < f->window_us) f->window_us = now_us; /* a clock that went back: the window starts again from here */
    if (now_us - f->window_us < RXF_REPORT_US) return false;
    *out = f->st;
    memset(&f->st, 0, sizeof f->st);
    f->window_us = now_us;
    return true;
}

bool rxf_worth_logging(const rxf_stats_t *st)
{
    return st->rate > 0 || st->unwanted >= RXF_REPORT_UNWANTED;
}
