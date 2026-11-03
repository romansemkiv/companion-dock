/* Companion Box: USB Stream Decks to Bitfocus Companion over the Satellite API, on the Waveshare
 * ESP32-P4-WIFI6-POE-ETH.
 *
 * Boot order (each stage logs one line when done): NVS -> quick-reset count -> config ->
 * speaker and status light (the light shows "starting" from here; the ready chime plays once the
 * supervisor runs) -> netif + event loop -> JPEG encoder -> network (may update the C6 and restart) ->
 * web server and API
 * -> discovery -> surfaces (Companion client + USB host; in Network Dock mode the dock servers instead
 * of the Companion client) -> service button -> supervisor -> recovery slot sync (slotsync,
 * a low-priority task that copies this firmware into the other app slot 10 minutes later). Only a
 * broken NVS stops the boot; any other stage that fails is logged and the box goes on with what
 * runs, so the hotspot or Ethernet stays a way in. Afterwards the supervisor task (supervisor.c)
 * runs the box; app_main returns. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "boxconfig/config.h"
#include "breadcrumb/breadcrumb.h"
#include "discovery/discovery.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "feedback/feedback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imgproc/jpeg_enc.h"
#include "netmgr/netmgr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "quickreset_logic.h"
#include "slotsync/slotsync.h"
#include "supervisor.h"
#include "surfaces/surfaces.h"
#include "svcbtn.h"
#include "webui/webui.h"

static const char *TAG = "main";

#define SETUP_HOTSPOT_S (15 * 60)
#define QR_NS "qreset" /* not "boxcfg": a factory reset leaves the count alone */
#define QR_KEY "n"
#define QR_CLEAR_STACK 3072 /* internal: it writes flash */

/* The box's one config, for the whole run; supervisor.c guards it once supervisor_init ran. */
static box_config_t s_cfg;

/* A boot stage is done. */
static void stage(const char *name)
{
    ESP_LOGI(TAG, "boot: %s (%lld ms)", name, (long long)(esp_timer_get_time() / 1000));
}

static void check(const char *what, esp_err_t e)
{
    if (e != ESP_OK) ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(e));
}

/* ---- RESET pressed 3 times (quickreset_logic.h) ----
 * The count lives in NVS (RESET is a power-on for the chip, so RTC memory
 * would not keep it). A task of its own clears it once the box has been up QUICKRESET_CLEAR_MS: the
 * clear writes flash, so neither the esp_timer task nor a PSRAM stack. */

static void quickreset_clear_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(QUICKRESET_CLEAR_MS));
    nvs_handle_t h;
    if (nvs_open(QR_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t e = nvs_erase_key(h, QR_KEY);
        if (e == ESP_OK) e = nvs_commit(h);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "quick-reset count not cleared: %s", esp_err_to_name(e));
        nvs_close(h);
    }
    vTaskDelete(NULL);
}

/* Counts this start; true on the third start in a row that came within QUICKRESET_CLEAR_MS of the
 * one before. */
static bool quickreset_start(void)
{
    uint8_t n = 0;
    nvs_handle_t h;
    esp_err_t e = nvs_open(QR_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "quick-reset count unavailable: %s", esp_err_to_name(e));
        return false;
    }
    if (nvs_get_u8(h, QR_KEY, &n) != ESP_OK) n = 0;
    const quickreset_action_t a = quickreset_on_start(&n);
    e = nvs_set_u8(h, QR_KEY, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "quick-reset count not stored: %s", esp_err_to_name(e));
    if (xTaskCreate(quickreset_clear_task, "qreset", QR_CLEAR_STACK, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "quick-reset clear not started: no memory");
    return a == QR_SETUP;
}

static void nvs_start(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* the settings cannot be read: start from defaults */
        ESP_LOGW(TAG, "NVS unreadable (%s): erased, starting from defaults", esp_err_to_name(e));
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
}

void app_main(void)
{
    feedback_amp_off();        /* the speaker's amplifier: never left floating on (GPIO53 low) */
    webui_log_capture_start(); /* Log.Get keeps the start-up lines too */
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    /* the slot too: after an OTA the box may run ota_1, where idf.py app-flash (ota_0) has no effect */
    ESP_LOGI(TAG, "%s %s on %s", app->project_name, app->version, run ? run->label : "?");
    breadcrumb_boot(); /* the reset reason and the last deliberate restart's, then clears it */

    nvs_start();
    stage("nvs");

    const bool setup_by_reset = quickreset_start();
    stage("quick-reset");

    bool saved = false;
    check("config load", boxcfg_load_and_store(&s_cfg, &saved));
    if (saved) ESP_LOGI(TAG, "config stored (first start, or completed with defaults)");
    /* Read once, here, before anything can change it: the whole run uses this mode, and a change
     * (Companion.SetConfig, which then restarts the box) takes effect at the next start. */
    const box_surface_mode_t mode = s_cfg.surface_mode;
    const bool dock = mode == BOX_SURFACE_DOCK;
    const int deck_limit = s_cfg.deck_limit; /* likewise (Device.SetConfig restarts the box): 2, or 4 unlocked */
    supervisor_init(&s_cfg, mode);
    stage("config");

    check("speaker and status light", feedback_start(&s_cfg)); /* the box works on without them */
    stage("feedback");

    check("esp_netif_init", esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_ERR_INVALID_STATE) check("default event loop", e);
    stage("netif");

    check("JPEG encoder", img_jpeg_init()); /* surfaces_start reports it on the decks' problem list */
    stage("jpeg");

    check("network", netmgr_start(&s_cfg)); /* may update the C6 and restart the P4 (c6fw) */
    if (setup_by_reset) {
        ESP_LOGW(TAG, "RESET pressed 3 times: setup hotspot on for 15 minutes");
        netmgr_ap_force(SETUP_HOTSPOT_S);
    }
    stage("net");

    webui_set_cfg_lock(supervisor_cfg_lock, supervisor_cfg_unlock);
    check("web server", webui_start(&s_cfg, supervisor_config_changed, mode)); /* applies go through the supervisor */
    /* the decks' maintenance screens: their actions on webui's worker (webui gives them the settings) */
    surfaces_set_hooks(&(surf_hooks_t){
        .action = webui_deck_action, .pin_check = webui_deck_pin_check, .pin_wait_s = webui_deck_pin_wait_s});
    stage("webui");

    check("discovery", discovery_start(&s_cfg, mode)); /* without mDNS a manual Companion address still works */
    stage("discovery");

    supervisor_cfg_lock();
    char name[sizeof s_cfg.box_name];
    snprintf(name, sizeof name, "%s", s_cfg.box_name);
    const bool announce = s_cfg.mdns_enabled;
    supervisor_cfg_unlock();
    if (dock) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_ETH); /* as the hotspot SSID: the dock serials end with it */
        check("surfaces (Network Dock mode)", surfaces_start_dock(name, mac, announce, deck_limit));
    } else {
        check("surfaces", surfaces_start(deck_limit));
    }
    stage(dock ? "surfaces (Network Dock mode)" : "surfaces");

    check("service button", svcbtn_start());
    stage("button");

    check("supervisor", supervisor_start());
    stage("supervisor");

    check("recovery slot sync", slotsync_init()); /* without it the box runs on, with no recovery copy */
    stage("slotsync");

    ESP_LOGI(TAG, "ready in %lld ms: internal heap %u free, PSRAM %u free",
             (long long)(esp_timer_get_time() / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
