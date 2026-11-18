#pragma once
/* The Ethernet receive filter: what of the broadcast and multicast frames reaches lwIP. Pure: no
 * ESP-IDF headers, no clock of its own (every call is given the time), so the host tests drive it.
 * eth.c runs it on the EMAC receive task, on the frame's header in its DMA buffer (the patched esp_eth), before
 * the frame is copied out, becomes a pbuf or is queued for lwIP.
 *
 * Why: every frame the box takes, wanted or not, goes through the same DMA buffers and lwIP queue as Companion's
 * TCP. A broadcast flood to a port nobody listens on (a 17-byte "Hello" every 1 ms and faster) cost a copy and a
 * trip through lwIP per frame, and Companion's own segments were lost with it. Dropped here, such a frame costs
 * one look at its header: a full 100 Mbit link of them keeps one core about 89 % busy.
 *
 * Rules:
 * - A unicast frame always passes (the EMAC has already kept only those to the box's MAC).
 * - Of the broadcast frames only these pass: ARP, and IPv4 UDP to port 68 (DHCP replies to the
 *   box's DHCP client). Everything else broadcast is dropped (RXF_DROP_UNWANTED).
 * - Of the IPv4 multicast frames only mDNS (UDP to 224.0.0.251:5353, how Companion and the box find
 *   each other in both modes) and IGMP (so snooping switches keep forwarding mDNS) pass.
 * - IPv6 multicast passes (neighbour discovery, mDNS over IPv6); any other multicast is dropped.
 * - Each kind that passes has its own budget (RXF_*_RATE frames a second, a burst of RXF_*_BURST):
 *   over it the frame is dropped (RXF_DROP_RATE), so a flood of a wanted kind is bounded too, and
 *   cannot use up another kind's budget. ARP asking for the box's own address has a budget apart
 *   from the rest of ARP, so an ARP storm on the LAN does not hide the requests that matter.
 * - All the budgets together stay under 1000 frames a second, a rate the box was seen to take
 *   with Companion connected. */
#include <stdbool.h>
#include <stdint.h>

typedef enum { RXF_PASS = 0, RXF_DROP_UNWANTED, RXF_DROP_RATE } rxf_verdict_t;

/* The kinds of non-unicast frame that pass, each with its own budget. */
typedef enum { RXF_K_ARP_US = 0, RXF_K_ARP, RXF_K_DHCP, RXF_K_MDNS, RXF_K_V6, RXF_K_COUNT } rxf_kind_t;

#define RXF_ARP_US_RATE 100 /* ARP for the box's own address */
#define RXF_ARP_US_BURST 50
#define RXF_ARP_RATE 200    /* any other broadcast ARP */
#define RXF_ARP_BURST 100
#define RXF_DHCP_RATE 50
#define RXF_DHCP_BURST 20
#define RXF_MDNS_RATE 300   /* mDNS and IGMP */
#define RXF_MDNS_BURST 100
#define RXF_V6_RATE 100     /* IPv6 multicast */
#define RXF_V6_BURST 50

#define RXF_REPORT_US (10LL * 1000 * 1000) /* rxf_report's window */
#define RXF_REPORT_UNWANTED 1000           /* unwanted drops in a window worth a log line (100 a second) */

typedef struct {
    int64_t tokens; /* in millionths of a frame */
    int64_t last_us;
} rxf_bucket_t;

/* One report window's counts. */
typedef struct {
    uint32_t unicast;  /* frames to the box itself, passed */
    uint32_t unwanted; /* RXF_DROP_UNWANTED */
    uint32_t rate;     /* RXF_DROP_RATE */
} rxf_stats_t;

typedef struct {
    rxf_bucket_t b[RXF_K_COUNT];
    bool started;
    rxf_stats_t st;    /* the current report window's */
    int64_t window_us; /* when the current report window began */
} rxf_t;

void rxf_init(rxf_t *f);

/* The verdict for one received Ethernet frame (from the destination MAC on, no FCS). own_ip: the
 * box's IPv4 address on this interface in network byte order as stored (lwIP's u32), 0 for none. */
rxf_verdict_t rxf_check(rxf_t *f, const uint8_t *frame, uint32_t len, uint32_t own_ip, int64_t now_us);

/* True once a window (RXF_REPORT_US) has passed, with that window's counts; then a new window begins. A clock
 * that went back (eth.c's wraps every 49.7 days) starts the window again, with the counts so far. */
bool rxf_report(rxf_t *f, int64_t now_us, rxf_stats_t *out);

/* A window worth a log line: any rate drop, or at least RXF_REPORT_UNWANTED unwanted ones. */
bool rxf_worth_logging(const rxf_stats_t *st);
