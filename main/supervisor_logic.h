#pragma once
/* The supervisor's decisions (pure: no ESP-IDF header; host-tested in
 * test/host/test_supervisor_logic.c). supervisor.c feeds them what netmgr, the config and the clock
 * say, and acts on the answer. */
#include <stdbool.h>
#include <stdint.h>

#include "netmgr/netmgr.h"

/* ---- C6 lost ----
 * ESP-Hosted never retries a C6 that stopped answering (or didn't answer at boot): Wi-Fi and the
 * hotspot stay off for the rest of the run. With Ethernet up that is fine (the box goes on over the
 * cable). Without it a Wi-Fi-only box has lost its only network and its setup hotspot, so the box
 * restarts (the C6 is reset at every boot): when the C6 isn't ready, Wi-Fi or the hotspot is
 * configured, and Ethernet has had no link or no IPv4 address for C6W_ETH_DOWN_US in a row. At most
 * C6W_FREE_RESTARTS such restarts per power-on, then one per C6W_SPACING_US (the uptime: every
 * restart starts it again). */
#define C6W_ETH_DOWN_US (3 * 60 * 1000000LL)
#define C6W_FREE_RESTARTS 3
#define C6W_SPACING_US (30 * 60 * 1000000LL)

typedef struct {
    bool counting;             /* Ethernet is down since eth_down_since_us */
    int64_t eth_down_since_us;
} c6w_t;

typedef struct {
    bool c6_ready;     /* the C6 answers and runs a firmware Wi-Fi can use */
    bool radio_wanted; /* the config has a Wi-Fi station (SSID) or the hotspot on */
    bool eth_up;       /* Ethernet has link and an IPv4 address */
} c6w_in_t;

void c6w_init(c6w_t *w);
/* Every supervisor tick. now_us: time since this start; restarts: "C6 lost" restarts since power-on.
 * True: restart now. */
bool c6w_step(c6w_t *w, const c6w_in_t *in, int64_t now_us, uint32_t restarts);

/* The restart count, kept in RTC no-init memory across those restarts (a magic and a checksum, like
 * the breadcrumb: random memory after a power-on reads as 0). */
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t sum;
} c6w_rec_t;

uint32_t c6w_rec_count(const c6w_rec_t *r); /* 0 when r isn't a record c6w_rec_set wrote */
void c6w_rec_set(c6w_rec_t *r, uint32_t count);

/* ---- OTA self-check ----
 * A new image is confirmed only once it runs healthy and the network path the update arrived on is
 * back: the interface the upload came in on (recorded by POST /ota before the restart) has an IPv4
 * address (Ethernet, station) or runs (hotspot); without a record, a route or the hotspot. The check
 * runs OTA_FIRST_CHECK_US after the start and then every OTA_RECHECK_US; an image that still fails
 * it OTA_GIVE_UP_US after the start is rolled back (so a switch rebooting at the same moment doesn't
 * cause a rollback). */
#define OTA_FIRST_CHECK_US (30 * 1000000LL)
#define OTA_RECHECK_US (30 * 1000000LL)
#define OTA_GIVE_UP_US (5 * 60 * 1000000LL)

typedef struct {
    netmgr_if_t via; /* the upload's interface; NETMGR_IF_NONE: no record */
    bool eth_ip, sta_ip, ap_on;
    bool routed;     /* netmgr_status_t.route != NET_ROUTE_NONE */
} ota_net_t;

/* The update's network path is back. *why (may be NULL) says what is missing when it isn't. */
bool ota_net_ok(const ota_net_t *n, const char **why);

typedef enum { OTA_CONFIRM, OTA_WAIT, OTA_ROLLBACK } ota_verdict_t;
/* At a check: healthy (everything above holds) confirms; otherwise wait for the next check, or roll
 * back once since_start_us reaches OTA_GIVE_UP_US. */
ota_verdict_t ota_verdict(bool healthy, int64_t since_start_us);

/* An update that came over the hotspot while the settings have it off (it was forced on by RESET x3
 * or the service button, and a restart forgets a force) would never find its path back, so its
 * self-check could only roll back. While such an image is pending verification the
 * hotspot is forced on for OTA_HOTSPOT_FORCE_S, and the force ends when the image is confirmed. Not
 * needed when the settings have the hotspot on; not when a force is already running (RESET x3 at
 * this start: a new force would shorten it). */
#define OTA_HOTSPOT_FORCE_S ((uint32_t)(OTA_GIVE_UP_US / 1000000) + 60)
bool ota_force_hotspot(bool pending, netmgr_if_t via, bool ap_enabled, bool already_forced);

/* ---- the Companion pin's save (deferred minor) ----
 * The supervisor updates the pin and the pinned instance's address in RAM at once; NVS gets them only
 * when they differ from what was last saved (or loaded), and at most once per PIN_SAVE_MIN_US (an
 * instance whose mDNS answers flip between two addresses must not wear the flash). */
#define PIN_SAVE_MIN_US (60 * 1000000LL)

/* changed: RAM differs from the last saved values; last_save_us: this run's last save attempt (-1:
 * none yet). */
bool pin_save_due(bool changed, int64_t now_us, int64_t last_save_us);
