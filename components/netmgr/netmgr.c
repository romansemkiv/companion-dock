/* The network manager: Ethernet (eth.c), the ESP-Hosted link to the ESP32-C6
 * (hosted.c) and, through it, the Wi-Fi station and the always-on hotspot at
 * 192.168.4.1 with its captive DNS (wifi.c, captive_dns.c). Bring-up order:
 * Ethernet first, then ESP-Hosted (which may
 * update the C6 and restart the P4, c6fw), then Wi-Fi in the APSTA / AP / STA
 * mode the box config asks for.
 *
 * Default route vs reported route:
 * - lwIP picks the default route by route_prio: Ethernet 150, station 100,
 *   soft-AP 10 (eth.c, wifi.c). esp_netif re-evaluates it whenever a netif
 *   comes up, gets or loses an address or goes down. netmgr never calls
 *   esp_netif_set_default_netif: one manual call would switch that automatic
 *   choice off for good (esp_netif keeps the override until the netif is
 *   destroyed).
 * - netmgr_status_t.route (and .ip) come from netpolicy_route over the link
 *   flags kept here: Ethernet when it has link and an address, else a
 *   configured station with an address, else none. That is what the status
 *   screens and the web page show.
 * - The two agree whenever an interface that is up also has an address, which
 *   is the normal case (DHCP lease or static). They can differ in one corner:
 *   Ethernet has link but no address (no DHCP server on the wire) while the
 *   station has one. esp_netif counts a netif as up from link up, so after the
 *   station reconnects lwIP may keep Ethernet as the default route; the status
 *   still says "wifi". Traffic to the station's own subnet (Companion on the
 *   same LAN) is routed by subnet match and is not affected; only off-subnet
 *   destinations are.
 *
 * Invariants:
 * - s_lock guards s_link and s_status; s_cfg_lock serialises netmgr_apply and
 *   guards s_cfg. Event handlers run on the default event loop task, take only
 *   s_lock and post NETMGR_EVENT_CHANGED with zero wait.
 * - Wi-Fi is only touched while hosted.c says the C6 is ready. A C6 that stops
 *   answering turns Wi-Fi state off (netmgr_radio_lost) until the next start;
 *   netmgr never restarts the P4 for it (the supervisor does when the box is
 *   left without a network).
 * - netmgr_apply restarts only the parts whose settings changed; a box rename
 *   only changes the DHCP hostname (used from the next DHCP exchange).
 * - netmgr_ap_force holds the hotspot on until s_force_until_us: Wi-Fi is
 *   applied with the config's settings plus the hotspot (apply_wifi), and
 *   s_wifi_ap remembers which hotspot setting is running, so netmgr_apply and
 *   the end of the force restart Wi-Fi only when that changes. The deadline's
 *   esp_timer hands the restart to a short-lived task: it blocks on the C6. */
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "netmgr/netmgr.h"
#include "netmgr_priv.h"

static const char *TAG = "netmgr";

ESP_EVENT_DEFINE_BASE(NETMGR_EVENT);

typedef struct {
    bool eth_link, eth_ip;
    bool sta_on;        /* station configured and WiFi running */
    bool wifi_enabled;  /* wifiEnabled as Wi-Fi was last applied with */
    bool sta_connected, sta_ip;
    bool ap_on;
    bool ap_forced;     /* netmgr_ap_force's deadline not reached */
    int ap_clients;
} link_state_t;

static SemaphoreHandle_t s_lock;     /* guards s_link and s_status */
static SemaphoreHandle_t s_cfg_lock; /* serialises netmgr_apply; guards s_cfg */
static link_state_t s_link;
static netmgr_status_t s_status;
static box_config_t s_cfg;
static char s_ap_ssid[33];  /* fixed at start (Ethernet MAC) */
static char s_hostname[NETMGR_HOSTNAME_LEN];  /* from s_cfg.box_name */
static bool s_started;
/* Set when applying that part failed, so the next netmgr_apply retries it. */
static bool s_eth_dirty, s_wifi_dirty;
/* s_cfg_lock: the hotspot is on until then whatever s_cfg says (netmgr_ap_force);
 * the hotspot setting Wi-Fi was last applied with. */
static int64_t s_force_until_us;
static bool s_wifi_ap;
static esp_timer_handle_t s_force_timer;

#define FORCE_END_STACK 4096 /* Wi-Fi restart through esp_wifi_remote, like the svcbtn action */

static const char *route_name(net_route_t r)
{
    return r == NET_ROUTE_ETH ? "eth" : r == NET_ROUTE_WIFI ? "wifi" : "none";
}

static bool status_equal(const netmgr_status_t *a, const netmgr_status_t *b)
{
    return a->route == b->route && !strcmp(a->ip, b->ip) && !strcmp(a->eth_addr, b->eth_addr) &&
           !strcmp(a->sta_addr, b->sta_addr) && a->eth_link == b->eth_link && a->eth_ip == b->eth_ip && a->sta_ip == b->sta_ip &&
           a->wifi_connected == b->wifi_connected && a->sta_net == b->sta_net && !strcmp(a->sta_ssid, b->sta_ssid) &&
           a->sta_trying == b->sta_trying && a->sta_not_found == b->sta_not_found && a->radio_down == b->radio_down &&
           a->sta_rssi == b->sta_rssi && a->ap_on == b->ap_on && a->ap_forced == b->ap_forced &&
           a->wifi_enabled == b->wifi_enabled &&
           !strcmp(a->ap_ssid, b->ap_ssid) &&
           a->ap_clients == b->ap_clients;
}

/* Recomputes the reported route from s_link (the default route is lwIP's,
 * see the header) and updates s_status. Caller holds s_lock. Returns true
 * when s_status changed. */
static bool refresh_locked(const char *why)
{
    net_state_t st = {
        .eth_link = s_link.eth_link,
        .eth_ip = s_link.eth_ip,
        .wifi_configured = s_link.sta_on,
        .wifi_ip = s_link.sta_ip,
    };
    net_route_t route = netpolicy_route(&st);

    netmgr_status_t ns = s_status;
    ns.route = route;
    /* each interface's own address, and the route's from them */
    char eth[16] = "", sta[16] = "";
    esp_netif_ip_info_t info;
    esp_netif_t *en = netmgr_eth_netif(), *sn = netmgr_wifi_sta_netif();
    if (en && esp_netif_get_ip_info(en, &info) == ESP_OK) esp_ip4addr_ntoa(&info.ip, eth, sizeof eth);
    if (sn && esp_netif_get_ip_info(sn, &info) == ESP_OK) esp_ip4addr_ntoa(&info.ip, sta, sizeof sta);
    netpolicy_addrs(route, s_link.eth_ip, eth, s_link.sta_connected, s_link.sta_ip, sta, ns.ip, ns.eth_addr, ns.sta_addr);
    ns.eth_link = s_link.eth_link;
    ns.eth_ip = s_link.eth_ip;
    ns.wifi_connected = s_link.sta_connected;
    ns.sta_net = ns.sta_trying = NETMGR_STA_NONE;
    ns.sta_ssid[0] = '\0';
    ns.sta_rssi = 0;
    ns.sta_not_found = 0;
    if (s_link.sta_on) {
        netmgr_wifi_sta_info(&ns.sta_net, ns.sta_ssid, &ns.sta_rssi, &ns.sta_trying, &ns.sta_not_found);
        if (!s_link.sta_connected) { /* joined counts only while associated */
            ns.sta_net = NETMGR_STA_NONE;
            ns.sta_ssid[0] = '\0';
            ns.sta_rssi = 0;
        }
    }
    ns.radio_down = !netmgr_hosted_ready();
    ns.wifi_enabled = s_link.wifi_enabled;
    ns.sta_ip = s_link.sta_ip;
    ns.ap_on = s_link.ap_on;
    netpolicy_ap_ip(ns.ap_on, ns.ap_ip);
    ns.ap_forced = s_link.ap_forced;
    ns.ap_clients = s_link.ap_clients;
    if (status_equal(&ns, &s_status)) return false;

    s_status = ns;
    ESP_LOGI(TAG, "%s: route %s %s, eth %s, sta %s, ap %s (%d)", why, route_name(route), ns.ip[0] ? ns.ip : "-",
             ns.eth_link ? "up" : "down", !s_link.sta_on ? "off" : ns.wifi_connected ? "up" : "down",
             ns.ap_on ? "on" : "off", ns.ap_clients);
    return true;
}

static void post_changed(void)
{
    /* Zero wait: this also runs on the default loop's own task. */
    if (esp_event_post(NETMGR_EVENT, NETMGR_EVENT_CHANGED, NULL, 0, 0) != ESP_OK)
        ESP_LOGW(TAG, "event queue full, NETMGR_EVENT_CHANGED dropped");
}

static void refresh(const char *why)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = refresh_locked(why);
    xSemaphoreGive(s_lock);
    if (changed) post_changed();
}

void netmgr_sta_net_changed(void)
{
    if (s_lock) refresh("sta network");
}

/* Applies the event to s_link; returns a description, or NULL to ignore it. */
static const char *apply_event_locked(esp_event_base_t base, int32_t id)
{
    if (base == ETH_EVENT) {
        switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            s_link.eth_link = true;
            return "eth link up";
        case ETHERNET_EVENT_DISCONNECTED:
        case ETHERNET_EVENT_STOP:
            /* A static address is re-announced with GOT_IP on the next link up. */
            s_link.eth_link = s_link.eth_ip = false;
            return "eth link down";
        default:
            return NULL;
        }
    }
    if (base == IP_EVENT) {
        switch (id) {
        case IP_EVENT_ETH_GOT_IP:
            s_link.eth_ip = true;
            return "eth got ip";
        case IP_EVENT_ETH_LOST_IP:
            s_link.eth_ip = false;
            return "eth lost ip";
        case IP_EVENT_STA_GOT_IP:
            s_link.sta_ip = true;
            return "sta got ip";
        case IP_EVENT_STA_LOST_IP:
            s_link.sta_ip = false;
            return "sta lost ip";
        default:
            return NULL;
        }
    }
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_CONNECTED:
            s_link.sta_connected = true;
            return "sta connected";
        case WIFI_EVENT_STA_DISCONNECTED:
        case WIFI_EVENT_STA_STOP:
            s_link.sta_connected = s_link.sta_ip = false;
            return "sta disconnected";
        case WIFI_EVENT_AP_START:
            s_link.ap_on = true;
            s_link.ap_clients = 0;
            return "ap start";
        case WIFI_EVENT_AP_STOP:
            s_link.ap_on = false;
            s_link.ap_clients = 0;
            return "ap stop";
        case WIFI_EVENT_AP_STACONNECTED:
            s_link.ap_clients++;
            return "ap client joined";
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_link.ap_clients > 0) s_link.ap_clients--;
            return "ap client left";
        default:
            return NULL;
        }
    }
    return NULL;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *why = apply_event_locked(base, id);
    bool changed = why && refresh_locked(why);
    xSemaphoreGive(s_lock);
    if (changed) post_changed();

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        esp_netif_ip_info_t ap;
        if (esp_netif_get_ip_info(netmgr_wifi_ap_netif(), &ap) == ESP_OK && captive_dns_start(ap.ip.addr) != ESP_OK)
            ESP_LOGE(TAG, "captive dns: start failed");
    }
}

/* Read once by hosted.c at start; stays "unknown" when the C6 never answered. */
static void set_sta_on(bool on, const char *why)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_link.sta_on = on;
    if (!on) s_link.sta_connected = s_link.sta_ip = false;
    bool changed = refresh_locked(why);
    xSemaphoreGive(s_lock);
    if (changed) post_changed();
}

void netmgr_radio_lost(void)
{
    netmgr_wifi_radio_lost();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_link.sta_on = s_link.sta_connected = s_link.sta_ip = false;
    s_link.ap_on = false;
    s_link.ap_clients = 0;
    bool changed = refresh_locked("c6 lost");
    xSemaphoreGive(s_lock);
    if (changed) post_changed();
}

/* Caller holds s_cfg_lock. */
static bool forced_locked(void)
{
    return s_force_until_us > esp_timer_get_time();
}

/* The hotspot setting Wi-Fi should run with. Caller holds s_cfg_lock. */
static bool want_ap_locked(const box_config_t *cfg)
{
    return cfg->ap_enabled || forced_locked();
}

static void set_ap_forced(bool forced, const char *why)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_link.ap_forced = forced;
    bool changed = refresh_locked(why);
    xSemaphoreGive(s_lock);
    if (changed) post_changed();
}

/* Applies cfg's Wi-Fi settings, with the hotspot on while netmgr_ap_force
 * holds it. sta_same: the station's networks are those Wi-Fi runs with (only
 * the hotspot changes): the station comes back on the network it was on.
 * Caller holds s_cfg_lock. ESP_ERR_INVALID_STATE without a usable C6: the
 * settings apply at the next start that finds one. */
static esp_err_t apply_wifi(const box_config_t *cfg, bool sta_same)
{
    static box_config_t eff; /* s_cfg_lock; static: keeps ~500 bytes off the callers' stacks */
    eff = *cfg;
    eff.ap_enabled = want_ap_locked(cfg);
    s_wifi_ap = eff.ap_enabled;
    xSemaphoreTake(s_lock, portMAX_DELAY); /* the status says Wi-Fi is off by the setting */
    s_link.wifi_enabled = cfg->wifi_enabled;
    xSemaphoreGive(s_lock);
    set_sta_on(false, "wifi reconfigure");
    if (!netmgr_hosted_ready()) {
        ESP_LOGW(TAG, "wifi: off (%s)", netmgr_hosted_error());
        return ESP_ERR_INVALID_STATE;
    }
    bool sta_on = false;
    esp_err_t e = netmgr_wifi_apply(&eff, s_ap_ssid, s_hostname, sta_same, &sta_on);
    set_sta_on(sta_on, "wifi applied");
    return e;
}

/* A failed Wi-Fi apply is retried on the next netmgr_apply, unless the C6 is
 * missing: that lasts until the next start. */
static bool wifi_retry_needed(esp_err_t e)
{
    return e != ESP_OK && netmgr_hosted_ready();
}

/* Caller holds s_cfg_lock. */
static void apply_hostname(const char *box_name)
{
    boxcfg_hostname(box_name, s_hostname, sizeof s_hostname);
    esp_netif_t *nifs[] = {netmgr_eth_netif(), netmgr_wifi_sta_netif()};
    for (size_t i = 0; i < sizeof nifs / sizeof nifs[0]; i++) {
        if (!nifs[i]) continue;
        esp_err_t e = esp_netif_set_hostname(nifs[i], s_hostname);
        if (e != ESP_OK) ESP_LOGW(TAG, "hostname '%s': %s", s_hostname, esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "hostname %s", s_hostname);
}

static bool eth_changed(const box_config_t *a, const box_config_t *b)
{
    if (a->eth_dhcp != b->eth_dhcp) return true;
    if (a->eth_dhcp) return false;
    return strcmp(a->eth_ip, b->eth_ip) || strcmp(a->eth_mask, b->eth_mask) || strcmp(a->eth_gw, b->eth_gw) ||
           strcmp(a->eth_dns, b->eth_dns);
}

/* a: the config Wi-Fi runs with, b: the new one. Caller holds s_cfg_lock. */
static bool wifi_changed(const box_config_t *a, const box_config_t *b)
{
    return s_wifi_ap != want_ap_locked(b) || netpolicy_wifi_changed(a, b);
}

/* Restarts Wi-Fi when the hotspot setting it runs with is not the one wanted
 * now (or a failed apply waits for a retry). Caller holds s_cfg_lock. */
static void reconcile_ap_locked(void)
{
    if (!s_wifi_dirty && s_wifi_ap == want_ap_locked(&s_cfg)) return;
    esp_err_t e = apply_wifi(&s_cfg, !s_wifi_dirty); /* the hotspot only, unless a failed apply waits */
    s_wifi_dirty = wifi_retry_needed(e);
    if (e != ESP_OK) ESP_LOGE(TAG, "wifi reconfigure failed: %s", esp_err_to_name(e));
}

static void force_end_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    if (!forced_locked()) { /* else a newer netmgr_ap_force moved the deadline */
        ESP_LOGI(TAG, "hotspot force over: back to the config (hotspot %s)", s_cfg.ap_enabled ? "on" : "off");
        set_ap_forced(false, "ap force over");
        reconcile_ap_locked();
    }
    xSemaphoreGive(s_cfg_lock);
    vTaskDelete(NULL);
}

/* esp_timer task: the Wi-Fi restart blocks on the C6, so it runs on a task of
 * its own (internal stack). */
static void force_timer_cb(void *arg)
{
    (void)arg;
    if (xTaskCreate(force_end_task, "ap_force", FORCE_END_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "hotspot force end: no memory for its task, retrying in 1 s");
        esp_timer_start_once(s_force_timer, 1000 * 1000);
    }
}

void netmgr_ap_force(uint32_t seconds)
{
    if (!s_started || !s_force_timer) {
        ESP_LOGW(TAG, "hotspot force before netmgr_start: ignored");
        return;
    }
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    s_force_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    esp_timer_stop(s_force_timer);
    if (seconds) {
        esp_timer_start_once(s_force_timer, (uint64_t)seconds * 1000000);
        ESP_LOGW(TAG, "hotspot forced on for %lu s", (unsigned long)seconds);
    } else {
        ESP_LOGI(TAG, "hotspot force ended");
    }
    set_ap_forced(seconds > 0, seconds ? "ap forced" : "ap force ended");
    reconcile_ap_locked();
    xSemaphoreGive(s_cfg_lock);
}

esp_err_t netmgr_start(const box_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    s_cfg_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_cfg_lock) return ESP_ERR_NO_MEM;

    esp_err_t e = esp_netif_init();
    if (e != ESP_OK) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    boxcfg_ap_ssid(mac, s_ap_ssid);
    strlcpy(s_status.ap_ssid, s_ap_ssid, sizeof s_status.ap_ssid);

    const esp_timer_create_args_t targs = {.callback = force_timer_cb, .name = "ap_force"};
    e = esp_timer_create(&targs, &s_force_timer);
    if (e != ESP_OK) return e;

    static const esp_event_base_t *const bases[] = {&ETH_EVENT, &IP_EVENT, &WIFI_EVENT};
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        e = esp_event_handler_instance_register(*bases[i], ESP_EVENT_ANY_ID, on_event, NULL, NULL);
        if (e != ESP_OK) return e;
    }

    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    s_cfg = *cfg;
    s_started = true;
    boxcfg_hostname(cfg->box_name, s_hostname, sizeof s_hostname);
    esp_err_t eth_err = netmgr_eth_start(cfg, s_hostname);
    if (eth_err != ESP_OK) ESP_LOGE(TAG, "ethernet start failed: %s", esp_err_to_name(eth_err));
    netmgr_hosted_start(); /* may update the C6 and restart; logs why Wi-Fi is off */
    esp_err_t wifi_err = apply_wifi(cfg, false);
    if (wifi_err != ESP_OK) ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(wifi_err));
    s_wifi_dirty = wifi_retry_needed(wifi_err);
    xSemaphoreGive(s_cfg_lock);

    refresh("start");
    return (eth_err == ESP_OK || wifi_err == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t netmgr_apply(const box_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!s_started) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    if (s_eth_dirty || eth_changed(&s_cfg, cfg)) {
        /* Cleared first: the new address arrives as GOT_IP. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_link.eth_ip = false;
        bool changed = refresh_locked("eth reconfigure");
        xSemaphoreGive(s_lock);
        if (changed) post_changed();
        esp_err_t e = netmgr_eth_apply_ip(cfg);
        s_eth_dirty = e != ESP_OK;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "eth reconfigure failed: %s", esp_err_to_name(e));
            err = e;
        }
    }
    if (s_wifi_dirty || wifi_changed(&s_cfg, cfg)) {
        /* only a change to the station's own networks starts again from the main one */
        esp_err_t e = apply_wifi(cfg, !s_wifi_dirty && !netpolicy_sta_changed(&s_cfg, cfg));
        s_wifi_dirty = wifi_retry_needed(e);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "wifi reconfigure failed: %s", esp_err_to_name(e));
            if (err == ESP_OK) err = e;
        }
    }
    if (strcmp(s_cfg.box_name, cfg->box_name)) apply_hostname(cfg->box_name);
    s_cfg = *cfg;
    xSemaphoreGive(s_cfg_lock);
    refresh("apply");
    return err;
}

bool netmgr_c6_ready(void) { return netmgr_hosted_ready(); }

/* ---------- Wi-Fi scan (wifiscan.h) ----------
 * s_lock guards the state below. A scan starts in netmgr_wifi_scan_start; its end (the driver's
 * SCAN_DONE through netmgr_scan_done, or the 10 s timer when that never comes) is handled on a short
 * task, which takes s_cfg_lock only to read the records and put the radio mode back. */
#define SCAN_FINISH_STACK 4096 /* internal: RPCs to the C6 and a few log lines */
static bool s_scan_running, s_scan_have;
static int64_t s_scan_start_ms, s_scan_end_ms;
static wifiscan_ap_t s_scan_last[WIFISCAN_MAX];
static size_t s_scan_last_n;
static char s_scan_err[64];
static esp_timer_handle_t s_scan_timer;
static atomic_bool s_scan_finishing;
static wifiscan_ap_t s_scan_tmp[WIFISCAN_MAX]; /* s_cfg_lock; static: keeps the stack small */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void scan_finish_task(void *arg)
{
    const bool timed_out = arg != NULL;
    if (!atomic_exchange(&s_scan_finishing, true)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool go = s_scan_running && (!timed_out || wifiscan_timed_out(true, now_ms(), s_scan_start_ms));
        xSemaphoreGive(s_lock);
        if (go) {
            size_t n = 0;
            xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
            const esp_err_t e = netmgr_wifi_scan_finish(timed_out, s_scan_tmp, &n);
            xSemaphoreGive(s_cfg_lock);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            esp_timer_stop(s_scan_timer); /* before the running state ends: a new scan arms its own */
            s_scan_running = false;
            if (e == ESP_OK) {
                s_scan_last_n = n;
                memcpy(s_scan_last, s_scan_tmp, n * sizeof *s_scan_tmp);
                s_scan_have = true;
                s_scan_end_ms = now_ms();
            } else {
                if (e == ESP_ERR_TIMEOUT) snprintf(s_scan_err, sizeof s_scan_err, "the scan did not finish");
                else if (e == ESP_ERR_INVALID_STATE) snprintf(s_scan_err, sizeof s_scan_err, "the scan was cancelled");
                else snprintf(s_scan_err, sizeof s_scan_err, "reading the scan failed: %s", esp_err_to_name(e));
            }
            xSemaphoreGive(s_lock);
        }
        atomic_store(&s_scan_finishing, false);
    }
    vTaskDelete(NULL);
}

/* Event loop task or esp_timer task: no RPC to the C6 here, a task does that. When the task cannot be
 * made, the scan timer (still armed after a SCAN_DONE) covers it; one that fired is armed again. */
static void scan_spawn_finish(bool timed_out)
{
    if (xTaskCreate(scan_finish_task, "scan_fin", SCAN_FINISH_STACK, (void *)(uintptr_t)timed_out, 3, NULL) == pdPASS) return;
    ESP_LOGW(TAG, "scan: no memory for the finish task");
    if (timed_out) esp_timer_start_once(s_scan_timer, 1000 * 1000);
}

void netmgr_scan_cancelled(void)
{
    if (!s_lock || !s_scan_timer) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_scan_running) {
        s_scan_running = false;
        snprintf(s_scan_err, sizeof s_scan_err, "the scan was cancelled");
    }
    xSemaphoreGive(s_lock);
    esp_timer_stop(s_scan_timer);
}

void netmgr_scan_done(void)
{
    if (s_scan_running) scan_spawn_finish(false); /* else the station's own connect scan */
}

static void scan_timer_cb(void *arg)
{
    (void)arg;
    scan_spawn_finish(true);
}

netmgr_scan_t netmgr_wifi_scan_start(char *why, size_t why_n)
{
    if (why_n) why[0] = '\0';
    if (!s_started || !netmgr_hosted_ready()) {
        const char *e = s_started ? netmgr_hosted_error() : "";
        snprintf(why, why_n, "no Wi-Fi: %s", e[0] ? e : "not started");
        return NETMGR_SCAN_NO_WIFI;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const wifiscan_act_t act = wifiscan_decide(s_scan_running, s_scan_have, now_ms(), s_scan_end_ms);
    if (act == WIFISCAN_RUN) {
        s_scan_running = true;
        s_scan_start_ms = now_ms();
        s_scan_err[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    if (act == WIFISCAN_BUSY) return NETMGR_SCAN_SCANNING;
    if (act == WIFISCAN_CACHED) return NETMGR_SCAN_CACHED;

    esp_err_t e = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY); /* only to start it: a settings save never waits for the scan */
    if (netmgr_hosted_ready()) {
        e = ESP_OK;
        if (!s_scan_timer) {
            const esp_timer_create_args_t targs = {.callback = scan_timer_cb, .name = "scan_end"};
            e = esp_timer_create(&targs, &s_scan_timer);
        }
        /* armed before the scan, so a stale timer cannot leave this one without its timeout */
        if (e == ESP_OK) {
            esp_timer_stop(s_scan_timer);
            e = esp_timer_start_once(s_scan_timer, (uint64_t)WIFISCAN_TIMEOUT_MS * 1000);
        }
        if (e == ESP_OK) {
            e = netmgr_wifi_scan_begin();
            if (e != ESP_OK) esp_timer_stop(s_scan_timer);
        }
    }
    xSemaphoreGive(s_cfg_lock);
    if (e != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_scan_running = false;
        xSemaphoreGive(s_lock);
        if (!netmgr_hosted_ready()) {
            snprintf(why, why_n, "no Wi-Fi: %s", netmgr_hosted_error());
            return NETMGR_SCAN_NO_WIFI;
        }
        snprintf(why, why_n, "scan failed: %s", esp_err_to_name(e));
        return NETMGR_SCAN_FAILED;
    }
    return NETMGR_SCAN_STARTED;
}

void netmgr_wifi_scan_results(netmgr_scan_results_t *out)
{
    memset(out, 0, sizeof *out);
    out->age_ms = -1;
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->n = s_scan_last_n;
    memcpy(out->aps, s_scan_last, out->n * sizeof *out->aps);
    out->scanning = s_scan_running;
    out->age_ms = wifiscan_age_ms(s_scan_have, now_ms(), s_scan_end_ms);
    if (!s_scan_running) strlcpy(out->error, s_scan_err, sizeof out->error);
    xSemaphoreGive(s_lock);
}

netmgr_if_t netmgr_iface_of_addr(uint32_t ip)
{
    const struct {
        esp_netif_t *nif;
        netmgr_if_t id;
    } nifs[] = {{netmgr_eth_netif(), NETMGR_IF_ETH}, {netmgr_wifi_sta_netif(), NETMGR_IF_STA},
                {netmgr_wifi_ap_netif(), NETMGR_IF_AP}};
    for (size_t i = 0; ip && i < sizeof nifs / sizeof nifs[0]; i++) {
        esp_netif_ip_info_t info;
        if (nifs[i].nif && esp_netif_get_ip_info(nifs[i].nif, &info) == ESP_OK && info.ip.addr == ip) return nifs[i].id;
    }
    return NETMGR_IF_NONE;
}

void netmgr_get_status(netmgr_status_t *out)
{
    if (!out) return;
    if (!s_lock) {
        memset(out, 0, sizeof *out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
