#pragma once
/* Broadcast storm control on Ethernet: when to switch the EMAC's broadcast reception off and on again.
 * Pure: no ESP-IDF headers, no clock of its own (every call is given the time), so the host tests drive it.
 * eth.c calls storm_tick every STORM_TICK_MS with the broadcast frames received in that tick and the frames the
 * EMAC lost in it, and applies the action to the EMAC (its "disable broadcast frames" filter bit).
 *
 * Why: the receive task (the patched esp_eth, components/esp_eth/COMPANION_DOCK.md) drops a flood in its DMA
 * buffers at a small cost a frame, so broadcast stays received (ARP for the box, DHCP, discovery) as long as it
 * keeps up. A flood beyond what it keeps up with fills the 30 DMA receive buffers and the EMAC drops what comes
 * next, Companion's own frames among them: only then is broadcast blocked in the EMAC, where it never reaches
 * the buffers. Unicast (all of Companion's traffic) and multicast (mDNS) are never blocked. While it is blocked a
 * host that does not know the box yet cannot find its MAC address (ARP is broadcast); hosts that know it
 * (Companion) refresh it by unicast and go on.
 *
 * Rules:
 * - A storm tick: STORM_ON_PER_TICK broadcast frames or more in it, and the EMAC lost STORM_LOST_PER_TICK frames
 *   or more in it (its receive buffers were full). A heavy flood the box keeps up with is no storm: broadcast stays
 *   received (the patched receive path keeps up with a full 100 Mbit link of the smallest frames).
 * - Open (broadcast received): STORM_ON_TICKS storm ticks in a row block (a moment's loss, a stalled task, never
 *   does).
 * - Blocked for the hold: STORM_HOLD_MS for a new storm, doubled at each probe that finds it still going, up
 *   to STORM_HOLD_MAX_MS.
 * - Then a probe: open for STORM_PROBE_TICKS ticks. A storm tick blocks again (a longer hold); none, the storm
 *   is over: open, and the next storm starts again at STORM_HOLD_MS.
 * - What a blocked tick reports is ignored (the EMAC drops broadcast then; a frame counted is from before). */
#include <stdbool.h>
#include <stdint.h>

#define STORM_TICK_MS 100
#define STORM_ON_PER_TICK 150   /* 1500 broadcast frames a second; a busy LAN has tens */
#define STORM_LOST_PER_TICK 100 /* frames the EMAC lost in a tick: a flood over what the box keeps up with */
#define STORM_ON_TICKS 3        /* storm ticks in a row that block, while open */
#define STORM_HOLD_MS 2000
#define STORM_HOLD_MAX_MS 30000
#define STORM_PROBE_TICKS 2

typedef enum { STORM_OPEN = 0, STORM_BLOCKED, STORM_PROBE } storm_state_t;

/* What eth.c does with the EMAC after a tick. */
typedef enum { STORM_ACT_NONE = 0, STORM_ACT_BLOCK, STORM_ACT_OPEN } storm_act_t;

/* Worth a log line: a storm began (open -> blocked) or ended (probe -> open). Probes say nothing. */
typedef enum { STORM_EV_NONE = 0, STORM_EV_BEGAN, STORM_EV_ENDED } storm_ev_t;

typedef struct {
    storm_state_t st;
    int64_t until_ms;  /* BLOCKED: when the probe starts */
    int32_t hold_ms;   /* the current storm's hold */
    int probe_left;    /* PROBE: ticks still to go */
    uint32_t storms;   /* storms since the start */
    int64_t began_ms;  /* the current storm's start */
    uint32_t peak;     /* the most broadcast frames in an open tick of the current storm */
    int over;          /* OPEN: storm ticks in a row so far */
} storm_t;

void storm_init(storm_t *s);

/* One tick: bcast = the broadcast frames received in it, lost = the frames the EMAC lost in it (no free receive
 * buffer). *ev: what happened worth a log line. */
storm_act_t storm_tick(storm_t *s, int64_t now_ms, uint32_t bcast, uint32_t lost, storm_ev_t *ev);

/* Broadcast is blocked now (BLOCKED; a probe is open). */
bool storm_blocked(const storm_t *s);
