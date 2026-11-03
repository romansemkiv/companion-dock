#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "boxconfig/config.h"
#include "netmgr/wifiscan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Route policy (pure) ---- */

typedef struct {
    bool eth_link;        /* Ethernet PHY link is up */
    bool eth_ip;          /* Ethernet netif has an IPv4 address */
    bool wifi_configured; /* WiFi station is configured (SSID set) and running */
    bool wifi_ip;         /* WiFi station is associated and has an IPv4 address */
} net_state_t;

typedef enum { NET_ROUTE_NONE, NET_ROUTE_ETH, NET_ROUTE_WIFI } net_route_t;

/* The box's interfaces (the values are stored in NVS by the OTA upload: keep them). */
typedef enum { NETMGR_IF_NONE = 0, NETMGR_IF_ETH = 1, NETMGR_IF_STA = 2, NETMGR_IF_AP = 3 } netmgr_if_t;

/* Ethernet when it has link and an address, else the WiFi station when it is
 * configured and has an address, else none. */
net_route_t netpolicy_route(const net_state_t *s);

/* The settings Wi-Fi is started with differ between a (running) and b, so netmgr_apply restarts it:
 * the station's network or password (the backup network's too), whether the hotspot has a password (ap_secured), or that
 * password while it is on (an open hotspot doesn't use it). The hotspot on/off is netmgr's own
 * check (netmgr_ap_force can hold it on). */
bool netpolicy_wifi_changed(const box_config_t *a, const box_config_t *b);
/* The station's own networks differ (the main or the backup network's name or password), or it was turned
 * on or off (wifiEnabled): a Wi-Fi restart then starts again from the main network; one for the hotspot
 * alone keeps the network joined. */
bool netpolicy_sta_changed(const box_config_t *a, const box_config_t *b);
/* the station runs with c: Wi-Fi is on (wifiEnabled) and a main network is set. Off, nothing connects,
 * retries or fails over; the hotspot runs on its own settings, and a scan brings the station up just for it. */
bool netpolicy_sta_on(const box_config_t *c);

/* The hotspot's address, octet by octet: the one place it is set (netmgr_priv.h's NETMGR_AP_IP). */
#define NETMGR_AP_OCTETS 192, 168, 4, 1

/* The hotspot's address as text (netmgr_status_t.ap_ip, the decks' setup page) while it is on, else "". */
void netpolicy_ap_ip(bool ap_on, char out[16]);

/* the addresses netmgr_status_t reports, from each netif's IPv4 as text (eth, sta; "" when it can't be read):
 * ip, the route's (route: netpolicy_route), as ever; eth_addr, Ethernet's own while it has one (eth_ip); sta_addr, the
 * station's own while it is joined with one (sta_connected and sta_ip). "0.0.0.0" is no address. */
void netpolicy_addrs(net_route_t route, bool eth_ip, const char *eth, bool sta_connected, bool sta_ip, const char *sta,
                     char ip[16], char eth_addr[16], char sta_addr[16]);

/* ---- Network manager status ---- */

/* Which Wi-Fi network the station has joined (netmgr_status_t.sta_net). */
enum { NETMGR_STA_NONE = 0, NETMGR_STA_MAIN = 1, NETMGR_STA_BACKUP = 2 };

typedef struct {
    net_route_t route;
    char ip[16];          /* address of the routed interface, "" when NONE */
    char eth_addr[16];    /* Ethernet's own IPv4 while it has one (whatever carries the route), else "" */
    char sta_addr[16];    /* the station's own IPv4 while it is joined with one, else "" */
    bool eth_link;
    bool eth_ip;          /* Ethernet netif has an IPv4 address */
    bool wifi_connected;  /* station associated with an AP */
    uint8_t sta_net;      /* NETMGR_STA_*: the network joined (main or backup); NONE while not associated */
    char sta_ssid[33];    /* its name, "" when none */
    int8_t sta_rssi;      /* its signal in dBm, sampled every 10 s and moving by 3 dB or more; 0 when none */
    uint8_t sta_trying;   /* NETMGR_STA_*: the network the station is trying while it has joined none; NONE when
                           * joined or not running */
    uint8_t sta_not_found; /* bit 1 << (NETMGR_STA_x - 1): the driver's last attempt there found no such network */
    bool radio_down;      /* the co-processor (the Wi-Fi radio) doesn't answer: no station, no hotspot */
    bool wifi_enabled;    /* the station is on by the settings (wifiEnabled); false: off, sta_net NONE */
    bool sta_ip;          /* station has an IPv4 address */
    bool ap_on;
    bool ap_forced;       /* hotspot held on by netmgr_ap_force, whatever the config says */
    char ap_ssid[33];
    char ap_ip[16];       /* the hotspot's address (the setup page on it), "" while it is off */
    int ap_clients;
} netmgr_status_t;

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_event.h"

/* NETMGR_EVENT_CHANGED is posted on the default event loop (no payload)
 * whenever netmgr_status_t changes: route, IP (the route's, Ethernet's or the station's own), link, the joined
 * Wi-Fi network (sta_net, sta_ssid, sta_rssi), AP state or clients. */
ESP_EVENT_DECLARE_BASE(NETMGR_EVENT);
enum { NETMGR_EVENT_CHANGED };

/* Brings up Ethernet (DHCP or static) and WiFi (STA and/or soft-AP through
 * the ESP32-C6) per cfg. Initialises esp_netif and the default event loop if
 * not done yet. Waits for the C6 (ESP-Hosted) and keeps it on the firmware
 * this image carries: that may update the C6 and restart the P4 (c6fw). Call
 * once. Returns ESP_OK when at least one interface started. */
esp_err_t netmgr_start(const box_config_t *cfg);

/* Re-applies Ethernet IP mode and WiFi/AP settings without reboot. Only the
 * parts whose settings changed are restarted. */
esp_err_t netmgr_apply(const box_config_t *cfg);

/* Turns the setup hotspot on for `seconds` even when the config has it off
 * (RESET pressed 3 times, the service button), alongside the station when one
 * is configured (APSTA); afterwards Wi-Fi goes back to what the config says.
 * A new call replaces the deadline; 0 ends it now. netmgr_apply keeps it on
 * meanwhile. Blocks while Wi-Fi restarts (call from a task, not a callback).
 * Ignored before netmgr_start. */
void netmgr_ap_force(uint32_t seconds);

void netmgr_get_status(netmgr_status_t *out);

/* the station joins sta_net (NETMGR_STA_MAIN / NETMGR_STA_BACKUP) now, dropping the other, and keeps it
 * as the one in use: stays on it while it works, tries it first after a loss, then the other; a change to
 * the Wi-Fi settings starts again from the main one. ESP_ERR_INVALID_STATE: the station isn't running;
 * ESP_ERR_NOT_FOUND: no backup network is set up. Any task. */
esp_err_t netmgr_wifi_use(int sta_net);

/* Ethernet's address now (a DHCP lease, or the static one applied) as dotted quads (16 bytes each):
 * address, mask, gateway ("0.0.0.0" when none), main DNS ("" when none). False, all "", while Ethernet has no
 * address (no cable, DHCP still asking) or isn't running. Any task. */
bool netmgr_eth_lease(char ip[16], char mask[16], char gw[16], char dns[16]);

/* The C6 answers over SDIO and runs a firmware Wi-Fi can use. False from a failed start-up, or from
 * the moment it stops answering, until the next start (ESP-Hosted never retries it; the supervisor
 * restarts the box when that leaves it without a network). */
bool netmgr_c6_ready(void);

typedef enum {
    NETMGR_SCAN_STARTED,  /* a scan is under way now */
    NETMGR_SCAN_SCANNING, /* one was already under way (nothing queues) */
    NETMGR_SCAN_CACHED,   /* the last one ended less than WIFISCAN_GAP_MS ago: no new scan, use its results */
    NETMGR_SCAN_NO_WIFI,  /* no usable radio: why says what is wrong */
    NETMGR_SCAN_FAILED,   /* the driver refused: why says how */
} netmgr_scan_t;

/* Starts a Wi-Fi scan and returns at once (it takes 3-6 s): collect the answer with
 * netmgr_wifi_scan_results. One scan at a time, none sooner than WIFISCAN_GAP_MS after the last one
 * ended. The radio is used under netmgr's config lock only to start the scan and to put its mode back
 * when it is over (or after WIFISCAN_TIMEOUT_MS without an end): off (the station is started just for
 * the scan), hotspot only (the station joins it for the scan) or as it was. The hotspot's clients may
 * stall for the scan, and the station's link may drop. why is set for NO_WIFI and FAILED. */
netmgr_scan_t netmgr_wifi_scan_start(char *why, size_t why_n);

typedef struct {
    wifiscan_ap_t aps[WIFISCAN_MAX]; /* the last finished scan, strongest first */
    size_t n;
    bool scanning;   /* a scan is under way */
    int64_t age_ms;  /* since the results were taken; -1 when there are none yet */
    char error[64];  /* why the last scan failed, "" if it did not (only while not scanning) */
} netmgr_scan_results_t;
void netmgr_wifi_scan_results(netmgr_scan_results_t *out);

/* Which of the box's interfaces has the local IPv4 address ip (network byte order, as in
 * sockaddr_in.sin_addr.s_addr); NETMGR_IF_NONE for none. */
netmgr_if_t netmgr_iface_of_addr(uint32_t ip);
#endif

#ifdef __cplusplus
}
#endif
