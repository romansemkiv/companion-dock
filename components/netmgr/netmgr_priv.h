#pragma once
/* Internal interfaces between netmgr.c, eth.c, wifi.c, hosted.c and
 * captive_dns.c. */
#include <stdbool.h>
#include <stddef.h>

#include "boxconfig/config.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "netmgr/netmgr.h"
#include "netmgr/wifiscan.h"

/* Soft-AP address. Kept equal to the ESP-IDF default, set explicitly so the
 * captive DNS answer always matches the AP netif. */
#define NETMGR_IP4(octets) ESP_IP4TOADDR(octets)
#define NETMGR_AP_IP NETMGR_IP4(NETMGR_AP_OCTETS)
#define NETMGR_AP_MASK ESP_IP4TOADDR(255, 255, 255, 0)

/* lwIP default-route priorities (esp_netif route_prio): wired wins when both
 * are up. ESP-IDF's own default puts Ethernet (50) below the station (100). */
#define NETMGR_ROUTE_PRIO_ETH 150
#define NETMGR_ROUTE_PRIO_STA 100

/* DHCP hostname (option 12) of the Ethernet and station netifs (boxcfg_hostname: at most 32 chars). */
#define NETMGR_HOSTNAME_LEN 33

/* netmgr.c: the C6 stopped answering (hosted.c's transport-failure handler,
 * on the default event loop task): Wi-Fi state goes off until the next start. */
void netmgr_radio_lost(void);

/* eth.c: internal EMAC + IP101 PHY. */
esp_err_t netmgr_eth_start(const box_config_t *cfg, const char *hostname);
esp_err_t netmgr_eth_apply_ip(const box_config_t *cfg);
esp_netif_t *netmgr_eth_netif(void);

/* hosted.c: the ESP-Hosted SDIO link to the ESP32-C6. netmgr_hosted_start
 * waits for ESP-Hosted's start-up, keeps the C6 on the firmware this image
 * carries (c6fw_check: may update the C6 and restart the P4) and reads the
 * C6's version. ESP_OK when Wi-Fi can run. Call once, after the default event
 * loop exists. */
esp_err_t netmgr_hosted_start(void);
/* The C6 answers and runs a firmware Wi-Fi can use. */
bool netmgr_hosted_ready(void);
/* Why not ready: never NULL nor empty while not ready; "" when ready. */
const char *netmgr_hosted_error(void);

/* wifi.c: esp_wifi through esp_wifi_remote / ESP-Hosted on the ESP32-C6.
 * Callers check netmgr_hosted_ready first. netmgr_wifi_apply initialises WiFi
 * on first use, stops it if running and starts it in the mode cfg asks for
 * (APSTA, AP, STA or not at all). sta_same: the station's networks are the ones
 * it ran with (only the hotspot changed): the failover then starts again on the
 * network it was on, not the main one. *sta_on reports whether the station
 * part is running. */
esp_err_t netmgr_wifi_apply(const box_config_t *cfg, const char *ap_ssid, const char *hostname, bool sta_same,
                            bool *sta_on);
/* Stops the station's reconnect attempts and forgets the running state
 * without talking to the C6 (it no longer answers). */
void netmgr_wifi_radio_lost(void);
/* A scan that does not block (all with netmgr.c's s_cfg_lock held, and netmgr_hosted_ready checked):
 * begin starts it on the radio as it is (the station is brought up just for it, a hotspot-only radio
 * joins the hotspot for it) and returns; the driver's WIFI_EVENT_SCAN_DONE calls netmgr_scan_done
 * (netmgr.c, must not block), which has finish collect the tidy results (WIFISCAN_MAX records) and put
 * the radio mode back. finish with timed_out gives the scan up instead (ESP_ERR_TIMEOUT). A
 * netmgr_wifi_apply meanwhile cancels the scan: finish then says ESP_ERR_INVALID_STATE and leaves the
 * radio alone. */
esp_err_t netmgr_wifi_scan_begin(void);
esp_err_t netmgr_wifi_scan_finish(bool timed_out, wifiscan_ap_t *out, size_t *n);
void netmgr_scan_done(void);
/* wifi.c: the scan was cancelled (new Wi-Fi settings, the C6 lost): netmgr.c ends its running state at once. */
void netmgr_scan_cancelled(void);
/* wifi_failover.c: the station's networks for netmgr_status_t (no RPC, a short critical section):
 * *net is NETMGR_STA_* joined (NONE while not joined), ssid its name, *rssi its signal (0 when none);
 * *trying the one being tried while none is joined; *not_found bit 1 << (NETMGR_STA_x - 1) per network
 * whose last attempt the driver reported as not found. */
void netmgr_wifi_sta_info(uint8_t *net, char ssid[33], int8_t *rssi, uint8_t *trying, uint8_t *not_found);

/* wifi.c internals used by wifi_failover.c (the backup network's glue). */
bool netmgr_wifi_sta_wanted(void);
bool netmgr_wifi_sta_connected(void);
bool netmgr_wifi_scan_busy(void);
/* The link was dropped on purpose (esp_wifi_disconnect returned OK): forget "connected" now, not when the
 * DISCONNECTED arrives, so the connect that follows is not skipped. */
void netmgr_wifi_sta_dropped(void);
/* Stops the 5 s retry timer and connects now (deferred while a scan runs). Task context. */
void netmgr_wifi_connect_now(void);

/* wifi_failover.c: wififo_logic.c's decisions applied to the station (all but the setters: any task).
 * set_networks keeps both networks' names and passwords; start begins at the network given (the main one
 * unless a restart keeps the backup) and starts the 1 s clock, which also samples the signal (so a main-only box gets it too); stop ends both. on_*: the
 * station's events; on_disconnected says whether the 5 s retry should follow (false: a switch or the
 * glue's own disconnect takes care of the connect). retry_ok: no connect may be started by the retry
 * timer now. */
esp_err_t netmgr_wfg_init(void);
void netmgr_wfg_set_networks(const box_config_t *cfg);
/* The station's driver config for net (wfo_net_t: 0 main, 1 backup). */
esp_err_t netmgr_wfg_config_net(int net);
/* The network a restart begins on (wfo_restart_net): ask before netmgr_wfg_stop. */
int netmgr_wfg_restart_net(bool sta_same);
void netmgr_wfg_start(bool has_backup, int net);
void netmgr_wfg_stop(void);
void netmgr_wfg_on_connected(void);
void netmgr_wfg_on_got_ip(void);
void netmgr_wfg_on_lost_ip(void);
bool netmgr_wfg_on_disconnected(int reason);
bool netmgr_wfg_retry_ok(void);
void netmgr_wfg_scan_started(void);
void netmgr_wfg_scan_ended(void);
/* netmgr.c: the station's joined network changed (wifi.c's failover, its signal sample): refresh the
 * status and post NETMGR_EVENT_CHANGED. Any task but with no wifi.c lock held. */
void netmgr_sta_net_changed(void);
esp_netif_t *netmgr_wifi_sta_netif(void);
esp_netif_t *netmgr_wifi_ap_netif(void);

/* captive_dns.c: answers every A query on ip:53 with ip. Idempotent. */
esp_err_t captive_dns_start(uint32_t ip);
