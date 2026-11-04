/* Wi-Fi station and the always-on hotspot through esp_wifi_remote (ESP-Hosted
 * on the ESP32-C6, hosted.c). The station netif gets route_prio
 * NETMGR_ROUTE_PRIO_STA (100, below Ethernet's 150) and the box hostname.
 *
 * Invariants:
 * - Nothing here talks to the C6 unless netmgr_hosted_ready(): netmgr.c checks
 *   it before netmgr_wifi_apply, and the station's reconnect checks it too.
 * - No RPC to the C6 on the esp_timer task: the reconnect timer hands
 *   esp_wifi_connect to a short-lived task, as netmgr.c's force_timer_cb does,
 *   so a slow C6 never holds up the other esp_timer callbacks (usbdeck's IN
 *   window, the hotspot force).
 * - The box config in our NVS is the only source of truth: the driver's own
 *   storage is RAM only.
 * - The backup network is wifi_failover.c's: this file feeds it the station's events and applies
 *   nothing it decides itself. */
#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netmgr_priv.h"

static const char *TAG = "netmgr";

#define STA_RETRY_US (5 * 1000 * 1000)
#define STA_RETRY_STACK 4096 /* internal: an RPC to the C6 and a log line */
#define STA_RETRY_AGAIN_US (1000 * 1000) /* no memory for the task: try again then */
#define AP_CHANNEL 1 /* moves to the station's channel once it associates */
#define AP_MAX_CONN 4

static esp_netif_t *s_sta, *s_ap;
static esp_timer_handle_t s_retry;
static bool s_inited;  /* esp_wifi_init done */
static bool s_running; /* esp_wifi_start done */
/* Shared between the caller of netmgr_wifi_apply, the event task and the
 * esp_timer task. Atomic (seq_cst), not volatile: volatile orders nothing between the two cores. */
static atomic_bool s_sta_wanted, s_sta_connected;
static atomic_bool s_retry_running; /* a retry task is under way */
/* A scan of ours (netmgr_wifi_scan_begin) is active / its esp_wifi_scan_start has returned OK. Written
 * under s_cfg_lock, read by the event and esp_timer tasks. s_scan_temp, s_scan_prev: s_cfg_lock. */
static atomic_bool s_scan_active, s_scan_started;
static bool s_scan_temp;
static wifi_mode_t s_scan_prev;
/* sta_connect found a scan active: the scan's end retries. With s_scan_active a store-then-load handshake on two
 * tasks (sta_connect: this, then s_scan_active; scan_restore: s_scan_active, then this), which needs seq_cst: with
 * volatile alone each side may read the other's old value, and the deferred connect is then never retried. */
static atomic_bool s_connect_deferred;

esp_netif_t *netmgr_wifi_sta_netif(void) { return s_sta; }

esp_netif_t *netmgr_wifi_ap_netif(void) { return s_ap; }

bool netmgr_wifi_sta_wanted(void) { return s_sta_wanted; }
bool netmgr_wifi_sta_connected(void) { return s_sta_connected; }
bool netmgr_wifi_scan_busy(void) { return s_scan_active; }
void netmgr_wifi_sta_dropped(void) { s_sta_connected = false; }

static void sta_connect(void)
{
    const wifiscan_conn_t act = wifiscan_connect_act(s_scan_active, s_sta_wanted, s_sta_connected);
    if (act == WIFISCAN_CONN_DEFER) { /* its connect scan would end ours, and ours would break the connect */
        s_connect_deferred = true;
        if (s_scan_active) return;
        s_connect_deferred = false; /* the scan ended meanwhile (scan_restore has looked at the flag already): connect now */
    }
    if (act == WIFISCAN_CONN_SKIP || !s_sta_wanted || !netmgr_hosted_ready()) return;
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "sta: connect failed (%s), retrying in 5 s", esp_err_to_name(e));
        esp_timer_start_once(s_retry, STA_RETRY_US);
    }
}

void netmgr_wifi_connect_now(void)
{
    esp_timer_stop(s_retry); /* its own connect starts now */
    sta_connect();
}

static void retry_task(void *arg)
{
    (void)arg;
    /* a switch dropped the attempt and connects the other network itself: not on top of it */
    if (netmgr_wfg_retry_ok()) sta_connect(); /* re-arms the timer itself when the connect is refused */
    atomic_store(&s_retry_running, false);
    vTaskDelete(NULL);
}

/* esp_timer task: never the RPC itself (see the header). */
static void retry_cb(void *arg)
{
    (void)arg;
    if (atomic_exchange(&s_retry_running, true)) return; /* one is under way: it re-arms on failure */
    if (xTaskCreate(retry_task, "sta_retry", STA_RETRY_STACK, NULL, 3, NULL) != pdPASS) {
        atomic_store(&s_retry_running, false);
        ESP_LOGW(TAG, "sta: no memory for the reconnect task, retrying in 1 s");
        esp_timer_start_once(s_retry, STA_RETRY_AGAIN_US);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        sta_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_sta_connected = true;
        netmgr_wfg_on_connected();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_connected = false;
        /* Fails with ESP_ERR_INVALID_STATE when already armed: fine. */
        if (netmgr_wfg_on_disconnected(data ? ((const wifi_event_sta_disconnected_t *)data)->reason : 0) && s_sta_wanted)
            esp_timer_start_once(s_retry, STA_RETRY_US);
        break;
    case WIFI_EVENT_STA_STOP:
        s_sta_connected = false;
        break;
    case WIFI_EVENT_SCAN_DONE:
        if (wifiscan_done_is_ours(s_scan_active, s_scan_started,
                                  wifiscan_done_status(data != NULL, data ? ((const wifi_event_sta_scan_done_t *)data)->status : 0)))
            netmgr_scan_done();
        break;
    default:
        break;
    }
}

/* The station's address: part of "joined" for the failover. */
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) netmgr_wfg_on_got_ip();
    else if (id == IP_EVENT_STA_LOST_IP) netmgr_wfg_on_lost_ip();
}

/* 192.168.4.1/24, DHCP server offering itself as DNS (for captive DNS). */
static esp_err_t ap_ip_setup(void)
{
    esp_err_t e = esp_netif_dhcps_stop(s_ap);
    if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return e;

    esp_netif_ip_info_t ip = {
        .ip.addr = NETMGR_AP_IP,
        .netmask.addr = NETMGR_AP_MASK,
        .gw.addr = NETMGR_AP_IP,
    };
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap, &ip), TAG, "ap: set ip");

    uint8_t offer_dns = 1;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                               &offer_dns, sizeof offer_dns),
                        TAG, "ap: dhcps dns option");
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = NETMGR_AP_IP};
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns), TAG, "ap: dns");
    return esp_netif_dhcps_start(s_ap);
}

/* esp_netif_create_default_wifi_sta with our route priority and hostname. */
static esp_netif_t *sta_netif_create(const char *hostname)
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.route_prio = NETMGR_ROUTE_PRIO_STA;
    esp_netif_t *nif = esp_netif_create_wifi(WIFI_IF_STA, &base);
    if (!nif) return NULL;
    esp_err_t e = esp_netif_set_hostname(nif, hostname);
    if (e != ESP_OK) ESP_LOGW(TAG, "sta: hostname '%s': %s", hostname, esp_err_to_name(e));
    return nif;
}

/* Each step runs once; a failed step is retried on the next call. */
static esp_err_t wifi_init_once(const char *hostname)
{
    static bool sta_handlers_done, ap_ip_done, handler_done;
    if (!s_sta) s_sta = sta_netif_create(hostname);
    if (!s_sta) return ESP_FAIL;
    if (!sta_handlers_done) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_default_wifi_sta_handlers(), TAG, "sta handlers");
        sta_handlers_done = true;
        /* That call (IDF's components/esp_wifi/src/wifi_default.c, built into esp_wifi_remote here)
         * registered esp_wifi_stop as a shutdown handler: every esp_restart would then make an RPC to
         * the C6 (esp_wifi_with_remote.c, esp_wifi_remote_stop) on the caller's stack with every task
         * still running, and to a C6 that stopped answering it can only time out (5 s), e.g. the
         * supervisor's "C6 lost" restart on its 4 KB stack. It achieves nothing: the C6 is reset at
         * every boot (CONFIG_ESP_HOSTED_HOST_CP_RESET_STRATEGY_ALWAYS). So it goes right here, before
         * any later step (esp_wifi_init: an RPC) can fail. Same function pointer as registered;
         * ESP_ERR_INVALID_STATE is IDF's "not registered" (esp_system.c), worth a line: a component
         * update may have changed what is registered. */
        esp_err_t e = esp_unregister_shutdown_handler((shutdown_handler_t)esp_wifi_stop);
        if (e == ESP_ERR_INVALID_STATE)
            ESP_LOGW(TAG, "esp_wifi_stop was not registered as a shutdown handler (nothing to unregister)");
        else if (e != ESP_OK)
            ESP_LOGW(TAG, "shutdown handler: %s", esp_err_to_name(e));
    }
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();
    if (!s_ap) return ESP_FAIL;
    if (!ap_ip_done) {
        ESP_RETURN_ON_ERROR(ap_ip_setup(), TAG, "ap ip setup");
        ap_ip_done = true;
    }
    if (!s_retry) {
        const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "sta_retry"};
        ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry), TAG, "retry timer");
    }
    ESP_RETURN_ON_ERROR(netmgr_wfg_init(), TAG, "failover timer");
    if (!handler_done) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                                                                NULL, NULL),
                            TAG, "wifi handler");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL),
                            TAG, "ip handler");
        handler_done = true;
    }
    if (!s_inited) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "esp_wifi_init");
        /* The box config in our NVS is the only source of truth. */
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        s_inited = true;
    }
    return ESP_OK;
}

/* WPA2 with ap_pass while ap_secured (the default; boxconfig keeps it 8-63 chars), open when the
 * config switches the password off. Every start of the hotspot comes through here (the
 * normal one, netmgr_ap_force for RESET x3, the service button and the OTA self-check). */
static esp_err_t config_ap(const box_config_t *cfg, const char *ssid)
{
    wifi_config_t wc = {0};
    size_t ssid_len = strnlen(ssid, sizeof wc.ap.ssid);
    memcpy(wc.ap.ssid, ssid, ssid_len);
    wc.ap.ssid_len = (uint8_t)ssid_len;
    if (cfg->ap_secured) {
        memcpy(wc.ap.password, cfg->ap_pass, strnlen(cfg->ap_pass, sizeof wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN; /* no password: the stored one stays unused */
    }
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_AP, &wc);
}

/* A request the C6 didn't answer: ESP-Hosted gives ESP_FAIL after its RPC timeout
 * (CONFIG_ESP_HOSTED_HOST_RPC_TIMEOUT_MS, 5 s) or its link wait (5 s); the C6's own answers keep their codes, and a
 * request that couldn't even be queued fails at once (ESP_FAIL too, but quick: not this). Every further request would
 * wait as long, and netmgr_apply's callers include the supervisor, which is on the 10 s task watchdog (panic): two
 * silent requests in a row would restart the box, which a C6 that stops answering must never do (hosted.c). The apply
 * stops at the first one instead and the C6 is lost, as at a transport failure (netmgr_hosted_lost). t0: when the
 * request was made. */
#define C6_SILENT_US (4 * 1000000LL) /* under ESP-Hosted's 5 s waits, far over any answer (milliseconds) */
static bool c6_silent(esp_err_t e, int64_t t0)
{
    return (e == ESP_FAIL || e == ESP_ERR_TIMEOUT) && esp_timer_get_time() - t0 >= C6_SILENT_US;
}

esp_err_t netmgr_wifi_apply(const box_config_t *cfg, const char *ap_ssid, const char *hostname, bool sta_same,
                            bool *sta_on)
{
    *sta_on = false;
    ESP_RETURN_ON_ERROR(wifi_init_once(hostname), TAG, "wifi init");
    /* the hotspot alone changed: the station comes back on the network it was on (the backup stays) */
    const int start_net = netmgr_wfg_restart_net(sta_same);

    esp_err_t silent = ESP_OK; /* a request the C6 didn't answer (c6_silent): the apply stops below */
    if (s_scan_active) { /* a scan (netmgr_wifi_scan_begin) loses to the new settings */
        int64_t t0 = esp_timer_get_time();
        esp_err_t se = esp_wifi_scan_stop();
        if (c6_silent(se, t0)) {
            silent = se;
        } else if (s_scan_temp) {
            t0 = esp_timer_get_time();
            se = esp_wifi_stop();
            if (c6_silent(se, t0)) silent = se;
        }
        s_scan_started = s_scan_active = false;
        s_connect_deferred = false; /* the new settings start the station themselves */
        netmgr_scan_cancelled();
    }

    s_sta_wanted = false;
    esp_timer_stop(s_retry);
    netmgr_wfg_stop();
    if (s_running && silent == ESP_OK) {
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t se = esp_wifi_stop();
        if (c6_silent(se, t0)) silent = se;
        else s_running = false;
        s_sta_connected = false;
    }
    if (silent != ESP_OK) {
        s_sta_connected = false;
        /* not tried again: Wi-Fi is off until the next start (netmgr_radio_lost resets this file's state), the box goes
         * on over Ethernet, and without a network the supervisor restarts it, which resets the C6 */
        netmgr_hosted_lost("the C6 stopped answering (a Wi-Fi request timed out): Wi-Fi is off until the next start");
        return silent;
    }

    bool ap = cfg->ap_enabled;
    const bool sta = netpolicy_sta_on(cfg); /* wifiEnabled off: no station, the hotspot as it is */
    if (!cfg->wifi_enabled) ESP_LOGI(TAG, "sta: off (wifiEnabled false)%s", ap ? ": the hotspot stays" : "");
    if (!ap && !sta) {
        ESP_LOGI(TAG, "wifi: off");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(ap && sta ? WIFI_MODE_APSTA : ap ? WIFI_MODE_AP : WIFI_MODE_STA), TAG,
                        "set mode");
    if (ap) {
        esp_err_t e = config_ap(cfg, ap_ssid);
        if (e != ESP_OK) {
            /* Never start a soft-AP on whatever config the C6 had before. */
            ESP_LOGE(TAG, "ap: config rejected (%s), hotspot off", esp_err_to_name(e));
            ap = false;
            if (!sta) return e;
            ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
        }
    }
    const bool has_backup = cfg->wifi_backup_ssid[0] != '\0';
    const int net = has_backup ? start_net : 0;
    if (sta) {
        netmgr_wfg_set_networks(cfg);
        ESP_RETURN_ON_ERROR(netmgr_wfg_config_net(net), TAG, "sta config");
    }

    s_sta_wanted = sta;
    /* the failover starts at the main network, or the one kept; the station's own START event
     * connects it */
    if (sta) netmgr_wfg_start(has_backup, net);
    if (sta && net) ESP_LOGI(TAG, "sta: the hotspot changed: staying on the backup network");
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK) {
        s_sta_wanted = false;
        netmgr_wfg_stop();
        return e;
    }
    s_running = true;
    *sta_on = sta;
    ESP_LOGI(TAG, "wifi: %s%s%s%s", ap ? "ap " : "", ap ? ap_ssid : "",
             !ap ? "" : cfg->ap_secured ? " (WPA2)" : " (open)", sta ? (ap ? " + sta" : "sta") : "");
    return ESP_OK;
}

void netmgr_wifi_radio_lost(void)
{
    s_scan_started = s_scan_active = false;
    s_connect_deferred = false;
    netmgr_scan_cancelled();
    s_sta_wanted = false;
    s_sta_connected = false;
    if (s_retry) esp_timer_stop(s_retry);
    netmgr_wfg_stop();
    /* esp_wifi_stop would be an RPC to a C6 that no longer answers; Wi-Fi
     * stays off until the next start. */
    s_running = false;
}

/* The wifiscan.h numbers are the driver's. */
_Static_assert((int)WIFI_AUTH_OPEN == WIFISCAN_AUTH_OPEN && (int)WIFI_AUTH_WEP == WIFISCAN_AUTH_WEP &&
                   (int)WIFI_AUTH_WPA_PSK == WIFISCAN_AUTH_WPA_PSK && (int)WIFI_AUTH_WPA2_PSK == WIFISCAN_AUTH_WPA2_PSK &&
                   (int)WIFI_AUTH_WPA_WPA2_PSK == WIFISCAN_AUTH_WPA_WPA2_PSK &&
                   (int)WIFI_AUTH_WPA2_ENTERPRISE == WIFISCAN_AUTH_WPA2_ENTERPRISE &&
                   (int)WIFI_AUTH_WPA3_PSK == WIFISCAN_AUTH_WPA3_PSK && (int)WIFI_AUTH_WPA2_WPA3_PSK == WIFISCAN_AUTH_WPA2_WPA3_PSK,
               "wifiscan.h auth numbers differ from wifi_auth_mode_t");

/* s_scan_active: a scan started by netmgr_wifi_scan_begin is under way. s_scan_prev is the radio's
 * mode before it, s_scan_temp says the station was started just for it. */

/* The scan is over (or cancelled): the radio mode goes back, and a connect it held up is tried soon. */
static void scan_restore(void)
{
    esp_err_t e = ESP_OK;
    if (s_scan_temp) {
        e = esp_wifi_stop();
        esp_err_t m = esp_wifi_set_mode(s_scan_prev);
        if (e == ESP_OK) e = m;
    } else if (s_scan_prev == WIFI_MODE_AP) {
        e = esp_wifi_set_mode(WIFI_MODE_AP);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "scan: putting the radio mode back failed: %s", esp_err_to_name(e));
    s_scan_started = false;
    s_scan_active = false;
    netmgr_wfg_scan_ended(); /* a switch the scan held up */
    if (s_connect_deferred) {
        s_connect_deferred = false;
        if (s_sta_wanted && s_retry) esp_timer_start_once(s_retry, STA_RETRY_AGAIN_US);
    }
}

esp_err_t netmgr_wifi_scan_begin(void)
{
    if (!s_inited || s_scan_active) return ESP_ERR_INVALID_STATE;
    s_scan_active = true; /* from here the station's connect waits */
    s_scan_started = false;
    s_scan_prev = WIFI_MODE_NULL;
    esp_err_t e = esp_wifi_get_mode(&s_scan_prev);
    if (e != ESP_OK) {
        s_scan_active = false;
        return e;
    }
    netmgr_wfg_scan_started();
    /* Bring the station up just for the scan (it never connects: s_sta_wanted is false whenever
     * the radio is not running), or let the hotspot-only radio scan as well. */
    s_scan_temp = !s_running;
    if (s_scan_temp) {
        e = esp_wifi_set_mode(WIFI_MODE_STA);
        if (e == ESP_OK) e = esp_wifi_start();
    } else if (s_scan_prev == WIFI_MODE_AP) {
        e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    if (e == ESP_OK) {
        const wifi_scan_config_t sc = {.show_hidden = false};
        e = esp_wifi_scan_start(&sc, false); /* WIFI_EVENT_SCAN_DONE says when */
    }
    if (e != ESP_OK) scan_restore();
    else s_scan_started = true; /* only now can a SCAN_DONE be ours */
    return e;
}

/* The records of the finished scan, tidy. Frees the driver's list on any failure. */
static esp_err_t scan_collect(wifiscan_ap_t *out, size_t *n)
{
    uint16_t cnt = 0;
    if (esp_wifi_scan_get_ap_num(&cnt) != ESP_OK) { /* the count unknown: a failure, the last list kept */
        esp_wifi_clear_ap_list();
        return ESP_FAIL;
    }
    if (cnt > WIFISCAN_RAW_MAX) cnt = WIFISCAN_RAW_MAX;
    wifi_ap_record_t *recs = calloc(cnt ? cnt : 1, sizeof *recs);
    wifiscan_ap_t *tidy = calloc(cnt ? cnt : 1, sizeof *tidy);
    esp_err_t e = ESP_OK;
    /* no network found is an empty list, not a failure: ESP-Hosted refuses a read of 0 records (ESP_ERR_INVALID_ARG) */
    if (!recs || !tidy || (cnt > 0 && esp_wifi_scan_get_ap_records(&cnt, recs) != ESP_OK)) {
        esp_wifi_clear_ap_list(); /* frees the driver's copy of the results */
        e = recs && tidy ? ESP_FAIL : ESP_ERR_NO_MEM;
    } else {
        if (cnt == 0) esp_wifi_clear_ap_list(); /* nothing read: the driver's (empty) list freed as a read would */
        for (uint16_t i = 0; i < cnt; i++) {
            /* the driver's name need not end in a NUL when it is 32 bytes long */
            memcpy(tidy[i].ssid, recs[i].ssid, strnlen((const char *)recs[i].ssid, sizeof recs[i].ssid));
            tidy[i].ssid[sizeof tidy[i].ssid - 1] = '\0';
            tidy[i].rssi = recs[i].rssi;
            tidy[i].channel = recs[i].primary;
            tidy[i].auth = (uint8_t)recs[i].authmode;
        }
        *n = wifiscan_tidy(tidy, cnt, WIFISCAN_MAX);
        memcpy(out, tidy, *n * sizeof *out);
    }
    free(recs);
    free(tidy);
    return e;
}

esp_err_t netmgr_wifi_scan_finish(bool timed_out, wifiscan_ap_t *out, size_t *n)
{
    *n = 0;
    if (!s_scan_active) return ESP_ERR_INVALID_STATE; /* netmgr_wifi_apply cancelled it and owns the radio now */
    esp_err_t e;
    if (timed_out) {
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        e = ESP_ERR_TIMEOUT;
    } else {
        e = scan_collect(out, n);
    }
    scan_restore();
    return e;
}
