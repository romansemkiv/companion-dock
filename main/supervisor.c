/* The supervisor task (supervisor.h).
 *
 * Signals: supervisor_config_changed and the NETMGR_EVENT_CHANGED handler (default event loop task)
 * only set a flag and wake the task; all the work happens on the task. Every wake runs a full tick,
 * so a burst of network events costs a few cheap ticks.
 *
 * OTA self-check: when the running image is
 * pending verify (first boot after an update, bootloader rollback armed), it is checked 30 s after
 * the start finished (supervisor_start is the last boot stage) and then every 30 s. It is confirmed
 * at the first check where the box is healthy: internal free RAM >= OTA_MIN_INTERNAL_FREE, the USB
 * host running, the web server running, the RPC dispatcher answering Sys.GetStatus, and the network path the update came over back (the interface POST
 * /ota recorded has an address, or the hotspot runs; without a record a route or the hotspot:
 * supervisor_logic.h). Still not healthy 2 min after the start (OTA_GIVE_UP_US): marked invalid, and the bootloader
 * goes back to the previous image. Confirming also lets c6fw update the C6 if it was waiting for
 * that. A crash or watchdog reset before then rolls back by itself.
 *
 * C6 lost (supervisor_logic.h): ESP-Hosted never retries a C6 that stopped answering or
 * didn't answer at boot. When that leaves the box without a network (Wi-Fi or the hotspot
 * configured, Ethernet without link or address for 3 min in a row) the box restarts, which resets
 * the C6: at most 3 times per power-on (a count in RTC no-init memory), then once per 30 min. */
#include "boxconfig/config.h"
#include "supervisor.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "breadcrumb/breadcrumb.h"
#include "discovery/discovery.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "feedback/feedback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "netmgr/netmgr.h"
#include "rpc/rpc.h"
#include "satclient/satclient.h"
#include "supervisor_logic.h"
#include "surfaces/surfaces.h"
#include "usbdeck/usbdeck.h"
#include "webui/webui.h"

static const char *TAG = "supervisor";

#define TICK_MS 1000
#define STACK 4096 /* internal: it writes flash (config, otadata) */
#define PRIO 4
#define MAX_INSTANCES 8
#define OTA_MIN_INTERNAL_FREE (12 * 1024)

static box_config_t *s_cfg; /* s_cfg_mtx */
static SemaphoreHandle_t s_cfg_mtx;
static StaticSemaphore_t s_cfg_mtx_buf;
static TaskHandle_t s_task;
static bool s_dock; /* Network Dock mode (as main read it at boot): no Companion to pick */
static atomic_bool s_cfg_dirty, s_net_dirty;
static char s_ap_ssid[33];

/* Supervisor task only. */
static box_config_t s_snap;                          /* config copy for netmgr/discovery_apply */
static companion_instance_t *s_inst;                 /* MAX_INSTANCES, off the stack (PSRAM, supervisor_start) */
static int s_ap_shown = -1;                          /* hotspot state last given to surfaces; -1 none */
static bool s_ota_pending;
static int64_t s_ota_start_us, s_ota_due_us;
static netmgr_if_t s_ota_via; /* the interface the update came in on (NONE: no record) */
static bool s_ota_forced_ap;  /* the self-check forced the hotspot on: end it at confirmation */
static c6w_t s_c6w;
/* The Companion pin and address as last saved (or loaded), and when this run last saved them. */
static char s_saved_pin[sizeof s_snap.companion_pin_id];
static char s_saved_host[sizeof s_snap.companion_host];
static uint16_t s_saved_port;
static int64_t s_pin_saved_us = -1;
static bool s_kept_logged;

/* "C6 lost" restarts since power-on: survives esp_restart, undefined after a power-on (cleared then). */
static RTC_NOINIT_ATTR c6w_rec_t s_c6_rec;

void supervisor_cfg_lock(void) { xSemaphoreTake(s_cfg_mtx, portMAX_DELAY); }
void supervisor_cfg_unlock(void) { xSemaphoreGive(s_cfg_mtx); }

static void wake(void)
{
    TaskHandle_t t = s_task;
    if (t) xTaskNotifyGive(t);
}

void supervisor_config_changed(void)
{
    atomic_store(&s_cfg_dirty, true);
    wake();
}

/* Default event loop task: never block here. */
static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    atomic_store(&s_net_dirty, true);
    wake();
}

/* ---- Companion selection ---- */

static void select_companion(void)
{
    const int n = discovery_instances(s_inst, MAX_INSTANCES);
    char host[sizeof s_cfg->companion_host], pin[sizeof s_cfg->companion_pin_id];
    uint16_t port = 0;
    esp_err_t e = ESP_OK;
    bool kept = false;

    supervisor_cfg_lock();
    const sel_result_t sel = discovery_select(s_cfg, s_inst, n, host, sizeof host, &port, pin, sizeof pin);
    if (pin[0]) { /* exactly one Companion seen and none pinned yet: pin it */
        strlcpy(s_cfg->companion_pin_id, pin, sizeof s_cfg->companion_pin_id);
        ESP_LOGI(TAG, "pinned to Companion %s", pin);
    }
    /* Only a pinned instance seen by mDNS yields an address unlike the stored one: remember it, so
     * the box still finds it after a restart while mDNS has not seen it yet. */
    if (sel == SEL_CONNECT && s_cfg->companion_auto && s_cfg->companion_pin_id[0] &&
        (strcmp(s_cfg->companion_host, host) != 0 || s_cfg->companion_port != port)) {
        ESP_LOGI(TAG, "Companion %s is at %s:%u", s_cfg->companion_pin_id, host, (unsigned)port);
        strlcpy(s_cfg->companion_host, host, sizeof s_cfg->companion_host);
        s_cfg->companion_port = port;
    }
    /* NVS: only a change, at most once a minute (pin_save_due), and never over a stored config the
     * start couldn't read whole (boxconfig keeps it until a deliberate save). */
    const bool changed = strcmp(s_cfg->companion_pin_id, s_saved_pin) != 0 ||
                         strcmp(s_cfg->companion_host, s_saved_host) != 0 || s_cfg->companion_port != s_saved_port;
    const int64_t now = esp_timer_get_time();
    if (pin_save_due(changed, now, s_pin_saved_us)) {
        kept = boxcfg_stored_kept();
        if (!kept) {
            s_pin_saved_us = now; /* also after a failed save: retried a minute later */
            e = boxcfg_save(s_cfg); /* under the lock: no other writer's save can interleave */
            if (e == ESP_OK) {
                strlcpy(s_saved_pin, s_cfg->companion_pin_id, sizeof s_saved_pin);
                strlcpy(s_saved_host, s_cfg->companion_host, sizeof s_saved_host);
                s_saved_port = s_cfg->companion_port;
            }
        }
    }
    supervisor_cfg_unlock();
    if (e != ESP_OK) ESP_LOGE(TAG, "saving the Companion pin failed: %s", esp_err_to_name(e));
    if (kept && !s_kept_logged) {
        s_kept_logged = true;
        ESP_LOGW(TAG, "the Companion pin is not saved: the stored settings couldn't be read whole at start and are "
                      "kept until settings are saved on the page");
    }

    satclient_set_target(host, port); /* "" when none or ambiguous: idle; same target: no-op */
    surfaces_set_selection(sel);
}

/* ---- config changes and the hotspot on the status screens ---- */

static void apply_config(void)
{
    supervisor_cfg_lock();
    s_snap = *s_cfg;
    supervisor_cfg_unlock();
    ESP_LOGI(TAG, "config changed: applying");
    esp_err_t e = netmgr_apply(&s_snap); /* may restart Wi-Fi: seconds */
    if (e != ESP_OK) ESP_LOGW(TAG, "network settings: %s (retried at the next change)", esp_err_to_name(e));
    esp_task_wdt_reset();
    discovery_apply(&s_snap);
    surfaces_dock_apply(s_snap.box_name, s_snap.mdns_enabled); /* dock mode: the docks' mDNS names */
    feedback_apply(&s_snap);                                   /* the speaker and the status light */
    s_ap_shown = -1; /* the hotspot password, or whether it has one, may have changed */
}

static void update_ap_info(void)
{
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    const int on = ns.ap_on ? 1 : 0;
    if (on == s_ap_shown) return;
    s_ap_shown = on;
    if (!on) {
        surfaces_set_ap_info(NULL, NULL);
        return;
    }
    char pass[sizeof s_cfg->ap_pass];
    supervisor_cfg_lock();
    const bool secured = s_cfg->ap_secured;
    strlcpy(pass, s_cfg->ap_pass, sizeof pass);
    supervisor_cfg_unlock();
    surfaces_set_ap_info(s_ap_ssid, secured ? pass : NULL); /* the password, or NULL: open, "No password" */
    /* the hotspot's SSID and password are also shown on the serial console */
    if (secured)
        ESP_LOGI(TAG, "hotspot on%s: SSID %s, WPA2, password %s", ns.ap_forced ? " (forced)" : "", s_ap_ssid, pass);
    else
        ESP_LOGI(TAG, "hotspot on%s: SSID %s, open (no password)", ns.ap_forced ? " (forced)" : "", s_ap_ssid);
}

/* ---- OTA self-check ---- */

static const char *via_name(netmgr_if_t i)
{
    return i == NETMGR_IF_ETH ? "Ethernet" : i == NETMGR_IF_STA ? "Wi-Fi" : i == NETMGR_IF_AP ? "the hotspot" : "unknown";
}

static void ota_arm(void)
{
    s_ota_via = webui_ota_via_take(); /* always: an old record must not outlive its upload */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_ota_pending = true;
        s_ota_start_us = esp_timer_get_time();
        s_ota_due_us = s_ota_start_us + OTA_FIRST_CHECK_US;
        webui_ota_check_set(s_ota_due_us, ota_rollback_check_us(s_ota_start_us), NULL); /* the page's countdown */
        ESP_LOGI(TAG, "first boot of this image (%s, uploaded over %s): self-check in 30 s", run->label,
                 via_name(s_ota_via));
        netmgr_status_t ns;
        netmgr_get_status(&ns);
        supervisor_cfg_lock();
        const bool ap_enabled = s_cfg->ap_enabled;
        supervisor_cfg_unlock();
        if (ota_force_hotspot(true, s_ota_via, ap_enabled, ns.ap_forced)) {
            /* uploaded over a forced hotspot that the settings have off: bring it back for the check */
            ESP_LOGW(TAG, "self-check: hotspot forced on for %lu s (the update came over it)",
                     (unsigned long)OTA_HOTSPOT_FORCE_S);
            netmgr_ap_force(OTA_HOTSPOT_FORCE_S);
            s_ota_forced_ap = true;
        }
    }
}

static bool healthy(char *why, size_t n)
{
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < OTA_MIN_INTERNAL_FREE) {
        snprintf(why, n, "internal RAM low (%u bytes free)", (unsigned)free_int);
        return false;
    }
    if (!usbdeck_running()) {
        strlcpy(why, "the USB host is not running", n);
        return false;
    }
    /* the web page and the next update depend on the server and the dispatcher */
    if (!webui_running()) {
        strlcpy(why, "the web server is not running", n);
        return false;
    }
    /* in-process, so the settings password does not apply. Never called with the config lock held:
     * the handler takes it. */
    const rpc_caller_t self = {.src = "self-check", .trusted = true};
    rpc_err_t err = {0};
    cJSON *r = rpc_call("Sys.GetStatus", NULL, &self, &err);
    const bool answered = cJSON_IsObject(r);
    cJSON_Delete(r);
    if (!answered) {
        snprintf(why, n, "the RPC dispatcher does not answer Sys.GetStatus (%d)", err.code);
        return false;
    }
    /* the next update must be able to reach the box the way this one did */
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    const ota_net_t net = {.via = s_ota_via, .eth_ip = ns.eth_ip, .sta_ip = ns.sta_ip, .ap_on = ns.ap_on,
                           .routed = ns.route != NET_ROUTE_NONE};
    const char *w = NULL;
    if (!ota_net_ok(&net, &w)) {
        strlcpy(why, w, n);
        return false;
    }
    return true;
}

static void ota_check(void)
{
    char why[80];
    const int64_t since = esp_timer_get_time() - s_ota_start_us;
    const bool ok = healthy(why, sizeof why);
    const ota_verdict_t v = ota_verdict(ok, since);
    if (v == OTA_WAIT) {
        s_ota_due_us += OTA_RECHECK_US;
        webui_ota_check_set(s_ota_due_us, ota_rollback_check_us(s_ota_start_us), why);
        ESP_LOGW(TAG, "self-check at %lld s: %s; checking again in %lld s (rollback at %lld s)",
                 (long long)(since / 1000000), why, (long long)(OTA_RECHECK_US / 1000000),
                 (long long)((ota_rollback_check_us(s_ota_start_us) - s_ota_start_us) / 1000000));
        return;
    }
    s_ota_pending = false;
    webui_ota_check_set(0, 0, NULL); /* no more checks (confirmed, or rolling back) */
    if (v == OTA_ROLLBACK) {
        ESP_LOGE(TAG, "self-check failed (%s): rolling back to the previous image", why);
        /* a factory reset waiting for this check (webui's restart task): the previous firmware starts without the
         * settings, as asked; the lock held into the restart, so no save lands after the erase */
        const bool reset = webui_reset_pending();
        if (reset) {
            supervisor_cfg_lock();
            const esp_err_t re = boxcfg_factory_reset();
            if (re != ESP_OK) ESP_LOGE(TAG, "factory reset before the rollback failed: %s", esp_err_to_name(re));
            else ESP_LOGW(TAG, "factory reset asked for: settings erased before the rollback");
        }
        vTaskDelay(pdMS_TO_TICKS(200)); /* let the log line out */
        breadcrumb_note(BC_WHY_OTA_ROLLBACK); /* the call below restarts inside IDF */
        esp_ota_mark_app_invalid_rollback_and_reboot(); /* restarts; returns only without a fallback */
        if (reset) supervisor_cfg_unlock(); /* no fallback: webui's restart task erases and restarts on its own */
        breadcrumb_clear();
        ESP_LOGE(TAG, "no previous image to roll back to: staying on this one");
        return;
    }
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e == ESP_OK)
        ESP_LOGI(TAG, "self-check passed at %lld s (uploaded over %s, which is back): image confirmed",
                 (long long)(since / 1000000), via_name(s_ota_via));
    else ESP_LOGE(TAG, "confirming the image failed: %s", esp_err_to_name(e));
    if (s_ota_forced_ap) { /* after the confirmation: the Wi-Fi restart takes seconds */
        s_ota_forced_ap = false;
        netmgr_ap_force(0); /* back to the settings */
        esp_task_wdt_reset();
    }
}

/* ---- C6 lost ---- */

static void c6_watch(void)
{
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    supervisor_cfg_lock();
    const bool wanted = s_cfg->wifi_ssid[0] != '\0' || s_cfg->ap_enabled;
    supervisor_cfg_unlock();
    const c6w_in_t in = {.c6_ready = netmgr_c6_ready(), .radio_wanted = wanted, .eth_up = ns.eth_link && ns.eth_ip};
    const uint32_t n = c6w_rec_count(&s_c6_rec);
    if (!c6w_step(&s_c6w, &in, esp_timer_get_time(), n)) return;
    ESP_LOGE(TAG, "the C6 is lost and Ethernet has been down for 3 min: restarting to reset it (%lu since power-on)",
             (unsigned long)(n + 1));
    c6w_rec_set(&s_c6_rec, n + 1);
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the log line out */
    breadcrumb_restart(BC_WHY_C6_LOST);
}

/* ---- the status light and sounds ---- */

static void report_feedback(void)
{
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    usbdeck_trouble_t ut;
    usbdeck_trouble(&ut);
    bool driving = false, lost = false;
    surfaces_companion(&driving, &lost);
    const fb_status_t st = {
        .booted = true, /* this task runs once the boot finished */
        .update = webui_ota_busy(),
        .net_up = ns.route != NET_ROUTE_NONE,
        .setup_ap = ns.ap_forced,
        .companion = driving,
        .companion_lost = lost,
        .deck_trouble = ut.troubled > 0,
        .deck_alerts = ut.reports,
    };
    feedback_status(&st);
}

/* ---- task ---- */

/* the small tasks' stack margins, logged once STACK_LOG_US after start, as the deck and dock tasks' are
 * (log_gate_stacks) and deckact's: feedback's 3 KB in PSRAM first, then speaker's and the service button's poll task's
 * (PSRAM), this task's own (internal), and notify's (3 KB in PSRAM too); bytes never used so far. Measured
 * only: no stack size changes here. */
#define STACK_LOG_US (30 * 1000000LL)
static void log_task_stacks(void)
{
    static bool logged;
    if (logged || esp_timer_get_time() < STACK_LOG_US) return;
    logged = true;
    static const char *const NAMES[] = {"feedback", "speaker", "svcbtn", "supervisor", "notify"};
    char line[112] = "";
    int n = 0;
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        TaskHandle_t t = xTaskGetHandle(NAMES[i]); /* NULL: not running (no speaker, say) */
        if (t && n < (int)sizeof line)
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %u", NAMES[i], (unsigned)uxTaskGetStackHighWaterMark(t));
    }
    if (n) ESP_LOGI(TAG, "stack never used %d s after start:%s bytes", (int)(STACK_LOG_US / 1000000), line);
}

static void tick(void)
{
    log_task_stacks();
    if (atomic_exchange(&s_cfg_dirty, false)) apply_config();
    if (atomic_exchange(&s_net_dirty, false)) surfaces_net_changed();
    update_ap_info();
    if (!s_dock) select_companion();
    c6_watch();
    if (s_ota_pending && esp_timer_get_time() >= s_ota_due_us) ota_check();
    report_feedback();
}

static void supervisor_task(void *arg)
{
    (void)arg;
    if (esp_task_wdt_add(NULL) != ESP_OK) ESP_LOGW(TAG, "not on the task watchdog");
    for (;;) {
        esp_task_wdt_reset();
        tick();
        esp_task_wdt_reset();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
    }
}

void supervisor_init(box_config_t *cfg, box_surface_mode_t mode)
{
    s_cfg = cfg;
    s_dock = mode == BOX_SURFACE_DOCK;
    s_cfg_mtx = xSemaphoreCreateMutexStatic(&s_cfg_mtx_buf);
    strlcpy(s_saved_pin, cfg->companion_pin_id, sizeof s_saved_pin); /* what NVS holds (or kept) */
    strlcpy(s_saved_host, cfg->companion_host, sizeof s_saved_host);
    s_saved_port = cfg->companion_port;
    if (esp_reset_reason() == ESP_RST_POWERON) c6w_rec_set(&s_c6_rec, 0); /* the count is per power-on */
    c6w_init(&s_c6w);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    boxcfg_ap_ssid(mac, s_ap_ssid);
}

esp_err_t supervisor_start(void)
{
    if (!s_cfg) return ESP_ERR_INVALID_STATE;
    if (s_task) return ESP_OK;
    /* Network changes before now are covered by the first tick. */
    atomic_store(&s_net_dirty, true);
    esp_err_t e = esp_event_handler_instance_register(NETMGR_EVENT, NETMGR_EVENT_CHANGED, on_net_event, NULL, NULL);
    if (e != ESP_OK) ESP_LOGE(TAG, "network event handler: %s (status screens update every second only)",
                              esp_err_to_name(e));
    ota_arm();
    /* the instances the supervisor task picks from, in PSRAM (internal when PSRAM is short): that task's only,
     * copied in by discovery_instances, never with the cache off (its flash writes take none of it) */
    if (!s_inst) s_inst = heap_caps_calloc(MAX_INSTANCES, sizeof *s_inst, MALLOC_CAP_SPIRAM);
    if (!s_inst) s_inst = heap_caps_calloc(MAX_INSTANCES, sizeof *s_inst, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_inst) return ESP_ERR_NO_MEM;
    if (xTaskCreate(supervisor_task, "supervisor", STACK, NULL, PRIO, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
