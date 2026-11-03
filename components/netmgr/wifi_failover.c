/* The backup Wi-Fi network's glue: wififo_logic.c decides, from the station's events and a 1 s
 * clock, which of the two networks the station tries; this file applies it.
 *
 * Invariants:
 * - The actions (set the station's config to a network, connect) and the signal sample are RPCs to the
 *   C6: they run on work_task, never on the event or esp_timer task.
 * - s_wfo, the two credential pairs and the signal sample are guarded by s_mux: short critical sections,
 *   no RPC, no log, and never with netmgr.c's locks held or taken (netmgr_sta_net_changed is called
 *   after leaving it).
 * - A switch drops the attempt under way with esp_wifi_disconnect: the DISCONNECTED it raises is
 *   recognised (wfo_own_disconnect) and neither counted as a loss nor followed by the 5 s retry, and the
 *   retry task does not start while such an event is due (netmgr_wfg_retry_ok). */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netmgr/wififo_logic.h"
#include "netmgr_priv.h"

static const char *TAG = "netmgr";

#define TICK_US (1000 * 1000)  /* the failover's clock, and the signal sample's */
#define WORK_STACK 4096        /* internal: a few RPCs to the C6 and a log line */
#define RSSI_SAMPLE_TICKS 10   /* the signal is sampled every 10 s while joined */
#define RSSI_STEP_DB 3         /* and reported when it moved this much, so the status does not flutter */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static wfo_t s_wfo;        /* s_mux */
static char s_ssid[2][33]; /* s_mux; index wfo_net_t: the two networks' names and passwords */
static char s_pass[2][65];
static int8_t s_rssi;      /* s_mux: the last sample of the joined network's signal */
static bool s_rssi_valid;
static atomic_int s_pending; /* a wfo_act_t waiting for work_task (WFO_ACT_USE_*: chosen by hand) */
static atomic_bool s_rssi_due, s_work_running;
static esp_timer_handle_t s_tick;
static int s_tick_n; /* the tick callback only */
static int s_cfg_net = WFO_NET_MAIN; /* s_mux: the network the station's driver config holds */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* wififo_logic.h's "not found" reasons are the driver's. */
_Static_assert(WFO_REASON_NO_AP_FOUND == WIFI_REASON_NO_AP_FOUND &&
                   WFO_REASON_NO_AP_FOUND_SECURITY == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY &&
                   WFO_REASON_NO_AP_FOUND_AUTHMODE == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD &&
                   WFO_REASON_NO_AP_FOUND_RSSI == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD,
               "WFO_REASON_* differ from wifi_err_reason_t");

static uint8_t sta_of(wfo_net_t n) { return n == WFO_NET_BACKUP ? NETMGR_STA_BACKUP : NETMGR_STA_MAIN; }

void netmgr_wifi_sta_info(uint8_t *net, char ssid[33], int8_t *rssi, uint8_t *trying, uint8_t *not_found)
{
    *net = *trying = NETMGR_STA_NONE;
    ssid[0] = '\0';
    *rssi = 0;
    *not_found = 0;
    portENTER_CRITICAL(&s_mux);
    if (wfo_joined(&s_wfo)) {
        *net = sta_of(s_wfo.net);
        strlcpy(ssid, s_ssid[s_wfo.net], 33);
        if (s_rssi_valid) *rssi = s_rssi;
    } else if (s_wfo.started) {
        *trying = sta_of(s_wfo.net);
    }
    for (int n = WFO_NET_MAIN; n <= WFO_NET_BACKUP; n++)
        if (wfo_not_found(&s_wfo, (wfo_net_t)n)) *not_found |= (uint8_t)(1u << (sta_of((wfo_net_t)n) - 1));
    portEXIT_CRITICAL(&s_mux);
}

void netmgr_wfg_set_networks(const box_config_t *cfg)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_ssid[WFO_NET_MAIN], cfg->wifi_ssid, sizeof s_ssid[0]);
    strlcpy(s_pass[WFO_NET_MAIN], cfg->wifi_pass, sizeof s_pass[0]);
    strlcpy(s_ssid[WFO_NET_BACKUP], cfg->wifi_backup_ssid, sizeof s_ssid[0]);
    strlcpy(s_pass[WFO_NET_BACKUP], cfg->wifi_backup_pass, sizeof s_pass[0]);
    portEXIT_CRITICAL(&s_mux);
}

/* The station's config for one of the two networks. */
static esp_err_t config_sta_net(wfo_net_t net)
{
    wifi_config_t wc = {0};
    char ssid[33], pass[65];
    portENTER_CRITICAL(&s_mux);
    strlcpy(ssid, s_ssid[net], sizeof ssid);
    strlcpy(pass, s_pass[net], sizeof pass);
    portEXIT_CRITICAL(&s_mux);
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof wc.sta.ssid));
    memcpy(wc.sta.password, pass, strnlen(pass, sizeof wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_cfg_net = net;
        portEXIT_CRITICAL(&s_mux);
    }
    /* a wipe the compiler keeps (a memset of locals about to go out of scope is a dead store it may drop) */
    for (volatile char *v = pass; v < pass + sizeof pass; v++) *v = 0;
    for (volatile uint8_t *v = (volatile uint8_t *)&wc; v < (volatile uint8_t *)&wc + sizeof wc; v++) *v = 0;
    return e;
}

esp_err_t netmgr_wfg_config_net(int net) { return config_sta_net(net == WFO_NET_BACKUP ? WFO_NET_BACKUP : WFO_NET_MAIN); }

int netmgr_wfg_restart_net(bool sta_same)
{
    portENTER_CRITICAL(&s_mux);
    const wfo_net_t n = wfo_restart_net(&s_wfo, sta_same);
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* Applies a pending switch, on work_task. forced (chosen by hand): even away from a network joined. */
static void switch_to(wfo_net_t net, bool forced)
{
    if (!netmgr_wifi_sta_wanted() || !netmgr_hosted_ready()) return;
    bool stale;
    portENTER_CRITICAL(&s_mux);
    /* the old network joined (a failover switch only), or the logic moved on, meanwhile */
    stale = (!forced && wfo_joined(&s_wfo)) || s_wfo.net != net;
    if (!stale) wfo_disconnect_requested(&s_wfo, now_ms()); /* before the call: its event may beat our return */
    portEXIT_CRITICAL(&s_mux);
    if (stale) return;
    /* Event order after a successful disconnect while associated: our DISCONNECTED (recognised as own,
     * no loss, no retry), then the new attempt's CONNECTED / GOT_IP. The station's "connected" flag is
     * cleared here, not by that event, or the connect below would be skipped as "already connected". */
    if (esp_wifi_disconnect() != ESP_OK) { /* NOT_CONNECT when idle: no event follows */
        portENTER_CRITICAL(&s_mux);
        wfo_disconnect_failed(&s_wfo);
        portEXIT_CRITICAL(&s_mux);
    } else {
        netmgr_wifi_sta_dropped();
    }
    esp_err_t e = config_sta_net(net);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "sta: config for the %s network failed (%s)", net == WFO_NET_BACKUP ? "backup" : "main", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "sta: trying the %s network", net == WFO_NET_BACKUP ? "backup" : "main");
    netmgr_sta_net_changed(); /* the status names the network being tried now, not the last one */
    netmgr_wifi_connect_now();
}

static void sample_rssi(void)
{
    wifi_ap_record_t ap;
    if (!netmgr_wifi_sta_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    bool changed = false;
    portENTER_CRITICAL(&s_mux);
    if (wfo_joined(&s_wfo) && (!s_rssi_valid || abs((int)ap.rssi - (int)s_rssi) >= RSSI_STEP_DB)) {
        s_rssi = ap.rssi;
        s_rssi_valid = true;
        changed = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) netmgr_sta_net_changed();
}

static void work_task(void *arg);

/* Any task: starts work_task unless one runs (it loops until nothing is left; the tick calls this again
 * if the task could not be made). */
static void kick_work(void)
{
    if (atomic_exchange(&s_work_running, true)) return;
    if (xTaskCreate(work_task, "sta_work", WORK_STACK, NULL, 3, NULL) != pdPASS) atomic_store(&s_work_running, false);
}

static void work_task(void *arg)
{
    (void)arg;
    for (;;) {
        const int act = atomic_exchange(&s_pending, 0);
        const bool rssi = atomic_exchange(&s_rssi_due, false);
        if (!act && !rssi) break;
        /* the action says whether it was chosen by hand: it can't be split from it */
        const bool forced = act == WFO_ACT_USE_MAIN || act == WFO_ACT_USE_BACKUP;
        if (act) switch_to(act == WFO_ACT_CONNECT_BACKUP || act == WFO_ACT_USE_BACKUP ? WFO_NET_BACKUP : WFO_NET_MAIN, forced);
        if (rssi && !netmgr_wifi_scan_busy()) sample_rssi();
    }
    atomic_store(&s_work_running, false);
    if (atomic_load(&s_pending) || atomic_load(&s_rssi_due)) kick_work(); /* set between the last look and now */
    vTaskDelete(NULL);
}

static void do_action(wfo_act_t act)
{
    if (act == WFO_ACT_NONE) return;
    atomic_store(&s_pending, (int)act);
    kick_work();
}

/* esp_timer task: the failover's clock. */
static void tick_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    const wfo_act_t act = wfo_tick(&s_wfo, now_ms());
    const bool joined = wfo_joined(&s_wfo);
    portEXIT_CRITICAL(&s_mux);
    if (act != WFO_ACT_NONE) atomic_store(&s_pending, (int)act);
    if (joined && ++s_tick_n >= RSSI_SAMPLE_TICKS) {
        s_tick_n = 0;
        atomic_store(&s_rssi_due, true);
    }
    if (atomic_load(&s_pending) || atomic_load(&s_rssi_due)) kick_work();
}

esp_err_t netmgr_wfg_init(void)
{
    if (s_tick) return ESP_OK;
    const esp_timer_create_args_t targs = {.callback = tick_cb, .name = "sta_failover"};
    return esp_timer_create(&targs, &s_tick);
}

void netmgr_wfg_start(bool has_backup, int net)
{
    portENTER_CRITICAL(&s_mux);
    wfo_start_on(&s_wfo, now_ms(), has_backup, net == WFO_NET_BACKUP ? WFO_NET_BACKUP : WFO_NET_MAIN);
    s_rssi_valid = false;
    portEXIT_CRITICAL(&s_mux);
    atomic_store(&s_pending, 0);
    s_tick_n = 0;
    esp_timer_stop(s_tick);
    esp_timer_start_periodic(s_tick, TICK_US);
}

void netmgr_wfg_stop(void)
{
    if (s_tick) esp_timer_stop(s_tick);
    portENTER_CRITICAL(&s_mux);
    wfo_stop(&s_wfo);
    s_rssi_valid = false;
    portEXIT_CRITICAL(&s_mux);
    atomic_store(&s_pending, 0);
    atomic_store(&s_rssi_due, false);
}

void netmgr_wfg_on_connected(void)
{
    portENTER_CRITICAL(&s_mux);
    /* a tick may have flipped the logic's network before its switch was applied while the old one
     * associated: follow the network the driver really connects (the pending switch is then stale) */
    if ((int)s_wfo.net != s_cfg_net) wfo_set_net(&s_wfo, (wfo_net_t)s_cfg_net);
    wfo_connected(&s_wfo, now_ms());
    s_rssi_valid = false;
    portEXIT_CRITICAL(&s_mux);
    netmgr_sta_net_changed();
}

void netmgr_wfg_on_got_ip(void)
{
    portENTER_CRITICAL(&s_mux);
    wfo_got_ip(&s_wfo, now_ms());
    portEXIT_CRITICAL(&s_mux);
    atomic_store(&s_rssi_due, true); /* the first sample, now that it counts as joined */
    kick_work();
    netmgr_sta_net_changed();
}

void netmgr_wfg_on_lost_ip(void)
{
    portENTER_CRITICAL(&s_mux);
    const wfo_act_t act = wfo_lost_ip(&s_wfo, now_ms());
    s_rssi_valid = false;
    portEXIT_CRITICAL(&s_mux);
    do_action(act);
    netmgr_sta_net_changed();
}

bool netmgr_wfg_on_disconnected(int reason)
{
    portENTER_CRITICAL(&s_mux);
    const bool own = wfo_own_disconnect(&s_wfo);
    if (!own) wfo_note_attempt(&s_wfo, (wfo_net_t)s_cfg_net, reason); /* "not found" only when the driver says so */
    const wfo_act_t act = own ? WFO_ACT_NONE : wfo_disconnected(&s_wfo, now_ms(), reason);
    s_rssi_valid = false;
    portEXIT_CRITICAL(&s_mux);
    do_action(act); /* off the backup: the main network now */
    netmgr_sta_net_changed();
    return !own && act == WFO_ACT_NONE;
}

esp_err_t netmgr_wifi_use(int sta_net)
{
    if (sta_net != NETMGR_STA_MAIN && sta_net != NETMGR_STA_BACKUP) return ESP_ERR_INVALID_ARG;
    const wfo_net_t net = sta_net == NETMGR_STA_BACKUP ? WFO_NET_BACKUP : WFO_NET_MAIN;
    esp_err_t e = ESP_OK;
    portENTER_CRITICAL(&s_mux);
    wfo_act_t act = WFO_ACT_NONE;
    if (!s_wfo.started) e = ESP_ERR_INVALID_STATE;
    else if (net == WFO_NET_BACKUP && !s_wfo.has_backup) e = ESP_ERR_NOT_FOUND;
    else act = wfo_use(&s_wfo, now_ms(), net);
    portEXIT_CRITICAL(&s_mux);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "sta: the %s network chosen by hand%s", net == WFO_NET_BACKUP ? "backup" : "main",
                 act == WFO_ACT_NONE ? " (already the one in use, or after the scan)" : "");
        do_action(act); /* during a scan: none now, the scan's end switches (wfo_scan_ended) */
        netmgr_sta_net_changed();
    }
    return e;
}

bool netmgr_wfg_retry_ok(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool ok = wfo_retry_ok(&s_wfo);
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void netmgr_wfg_scan_started(void)
{
    portENTER_CRITICAL(&s_mux);
    wfo_scan_started(&s_wfo);
    portEXIT_CRITICAL(&s_mux);
}

/* The scan ended (or was cancelled): a switch it held up is applied. */
void netmgr_wfg_scan_ended(void)
{
    portENTER_CRITICAL(&s_mux);
    const wfo_act_t act = wfo_scan_ended(&s_wfo, now_ms());
    portEXIT_CRITICAL(&s_mux);
    do_action(act);
}
