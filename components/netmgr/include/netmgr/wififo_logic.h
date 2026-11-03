#pragma once
/* The station's failover between the main and the backup Wi-Fi network. Pure: no ESP-IDF
 * headers, no clock of its own (every call is given the time), so the host tests drive it.
 *
 * Rules:
 * - The main network first. If the box has not joined the network it tries for WFO_SWITCH_MS, it tries
 *   the other one, and keeps alternating at that pace while neither works.
 * - On the backup it stays while it is joined, however long. After a loss on the backup it tries the
 *   main network first and at once; a loss on the main network keeps trying the main for WFO_SWITCH_MS.
 * - With no backup configured it never switches and says nothing: the glue's own retry (every
 *   WFO_RETRY_MS) is the only pacing, as before the backup existed.
 * - A new configuration of the station's networks starts again from the main network; a restart for the
 *   hotspot alone keeps the network it was on (wfo_restart_net).
 * - A Wi-Fi scan defers a due switch until it ends.
 *
 * Every function returns the action the glue (wifi.c) applies on its own task: it configures the
 * network named and connects. WFO_ACT_NONE: leave it to the retry timer. */
#include <stdbool.h>
#include <stdint.h>

#define WFO_SWITCH_MS 30000 /* without joining for this long, try the other network */
#define WFO_DISC_WAIT_MS 10000 /* an own-disconnect event that has not come after this long is not coming */
#define WFO_RETRY_MS 5000   /* the glue's reconnect pace after a failed attempt (not decided here) */

typedef enum { WFO_NET_MAIN = 0, WFO_NET_BACKUP = 1 } wfo_net_t;
/* USE_*: the network chosen by hand: the glue drops the network joined too. */
typedef enum { WFO_ACT_NONE = 0, WFO_ACT_CONNECT_MAIN, WFO_ACT_CONNECT_BACKUP, WFO_ACT_USE_MAIN, WFO_ACT_USE_BACKUP } wfo_act_t;

typedef struct {
    bool started;     /* the station runs (between wfo_start and wfo_stop) */
    bool has_backup;  /* a backup network is configured */
    bool assoc;       /* associated with the current network */
    bool has_ip;      /* and the station has an IPv4 address: both make it "joined" */
    int64_t disc_ms;  /* when the oldest of them was asked for */
    int expect_disc;  /* DISCONNECTED events the glue's own esp_wifi_disconnect calls are still to raise */
    bool scanning;    /* a Wi-Fi scan is under way */
    bool lost_pending; /* the backup was lost during a scan: the main network is tried when it ends */
    bool use_pending;  /* a network chosen by hand during a scan: use_net, switched to when the scan ends */
    wfo_net_t use_net; /* ... (net keeps naming the network tried or joined until then) */
    wfo_net_t net;    /* the network being tried or joined */
    bool not_found[2]; /* index wfo_net_t: the driver's last report on that network was "no such network" */
    wfo_net_t pref;   /* the network tried first after a loss (the one chosen by hand; else the main) */
    int64_t since_ms; /* when this attempt began (the last start, switch or loss) */
} wfo_t;

/* The station starts (or restarts) with this configuration: the main network, and the clock starts. */
wfo_act_t wfo_start(wfo_t *w, int64_t now_ms, bool has_backup);
/* wfo_start beginning on net instead of the main network (the backup only with has_backup), its
 * WFO_SWITCH_MS from now: a restart that keeps the network (wfo_restart_net). */
wfo_act_t wfo_start_on(wfo_t *w, int64_t now_ms, bool has_backup, wfo_net_t net);
/* The network a restart of the station begins on, asked before the station stops. sta_same: the
 * station's two networks are the ones it runs with (only the hotspot changed: switched on or off, forced,
 * its password). Then the network being tried or joined is kept, so a box on the backup stays on it;
 * a change to either pair, or a station that was not running, begins with the main network. */
wfo_net_t wfo_restart_net(const wfo_t *w, bool sta_same);
/* the network chosen by hand (Wifi.Use, the deck's Use): joined now (WFO_ACT_USE_*: dropping the other),
 * then the one in use: kept while it works, tried first after a loss, then the other. During a scan it waits
 * for the scan's end (wfo_scan_ended returns it, when it is still another network than the one tried or
 * joined): a scan and a switch never run at the same time, and the network isn't named before. Already
 * the one tried or joined: NONE, but it is still the one in use from now on. NONE too when it is not set up
 * (the backup without one) or the station isn't running. A change to the networks (wfo_start) forgets it; a
 * hotspot-only restart on it keeps it (wfo_start_on). */
wfo_act_t wfo_use(wfo_t *w, int64_t now_ms, wfo_net_t net);
/* The station stopped (Wi-Fi off, the co-processor lost): no actions until the next wfo_start. */
void wfo_stop(wfo_t *w);
/* "Joined" means associated and holding an IPv4 address: a network that associates but gives no address
 * (DHCP exhausted, a captive network) fails over like one that is not found.
 * wfo_start is also how a changed configuration begins again from the main network. */
/* Associated with the current network (the station's CONNECTED event). */
wfo_act_t wfo_connected(wfo_t *w, int64_t now_ms);
/* The station got / lost its IPv4 address (IP_EVENT_STA_GOT_IP / IP_EVENT_STA_LOST_IP). Losing it while
 * joined is a loss like a disconnect. */
wfo_act_t wfo_got_ip(wfo_t *w, int64_t now_ms);
wfo_act_t wfo_lost_ip(wfo_t *w, int64_t now_ms);
/* The station's DISCONNECTED event. reason is the driver's; not found, a wrong password and a lost link
 * all count the same, so it is not looked at. */
wfo_act_t wfo_disconnected(wfo_t *w, int64_t now_ms, int reason);
/* A periodic call (about once a second) with the time. */
wfo_act_t wfo_tick(wfo_t *w, int64_t now_ms);
/* A scan began / ended (the end can return the switch it deferred). */
void wfo_scan_started(wfo_t *w);
wfo_act_t wfo_scan_ended(wfo_t *w, int64_t now_ms);

/* The glue called esp_wifi_disconnect (it succeeded) to drop an attempt before a switch: the DISCONNECTED
 * it raises is its own. wfo_own_disconnect, called first for each DISCONNECTED, says true (once per
 * request) for such an event: it is no loss and the 5 s retry must not follow it. wfo_retry_ok: no such
 * event is still to come, so a reconnect cannot cut into the new attempt. */
void wfo_disconnect_requested(wfo_t *w, int64_t now_ms);
/* ... but esp_wifi_disconnect failed, so no event follows. */
void wfo_disconnect_failed(wfo_t *w);
/* The expectation is dropped by a CONNECTED (a new attempt follows the own DISCONNECTED in event order, so
 * none is still due), by wfo_start/stop, and by a tick WFO_DISC_WAIT_MS after the request (esp_wifi_disconnect
 * can succeed without raising an event). */
/* The glue's station config holds this network (it may differ from the logic's after a tick flipped it
 * before the switch was applied): the logic follows what really connects. */
void wfo_set_net(wfo_t *w, wfo_net_t net);
bool wfo_own_disconnect(wfo_t *w);
bool wfo_retry_ok(const wfo_t *w);

/* What the driver said of an attempt on net that failed (the reason of a DISCONNECTED not the glue's own):
 * not found (WFO_REASON_*) or found but not joined (a wrong password, ...). A CONNECTED on a network, or
 * a start, forgets it. the decks say "Not found" only when the driver did. */
#define WFO_REASON_NO_AP_FOUND 201 /* = WIFI_REASON_NO_AP_FOUND, and the three below (wifi_failover.c checks) */
#define WFO_REASON_NO_AP_FOUND_SECURITY 210
#define WFO_REASON_NO_AP_FOUND_AUTHMODE 211
#define WFO_REASON_NO_AP_FOUND_RSSI 212
void wfo_note_attempt(wfo_t *w, wfo_net_t net, int reason);
bool wfo_not_found(const wfo_t *w, wfo_net_t net);

/* The network being tried or joined, and whether it is joined. */
wfo_net_t wfo_net(const wfo_t *w);
bool wfo_joined(const wfo_t *w);
