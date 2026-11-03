/* The service button (svcbtn.h): a small task polls the pin
 * every SVCBTN_POLL_MS through svcbtn_logic. It only reads a GPIO, so its stack can live in PSRAM;
 * each action runs on a task of its own with an internal stack, because the hotspot blocks while
 * Wi-Fi restarts and the factory reset erases flash. A short press (SVC_SHORT) asks the surfaces task to
 * open or close the decks' maintenance screen on demand: surfaces_maint_toggle only counts it, so
 * the open (dock mode: the deck's input held from Companion) and its log lines run on the surfaces task's
 * stack, not on this small one; it toggles from its own state, so two quick presses cancel out
 *. The poll is 10 ms so that the short press's 50 ms floor is several samples, not one (a glitch
 * never opens it). */
#include "svcbtn.h"

#include <stdint.h>

#include "boxconfig/config.h"
#include "breadcrumb/breadcrumb.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netmgr/netmgr.h"
#include "supervisor.h"
#include "surfaces/surfaces.h"
#include "svcbtn_logic.h"

static const char *TAG = "svcbtn";

#define SVCBTN_POLL_MS 10
#define POLL_STACK 4096   /* PSRAM: GPIO reads and one log line at a time (about 2 KB: the web log's line, vfprintf) */
#define ACTION_STACK 8192 /* internal: Wi-Fi restart / flash erase; the factory reset's esp_restart */
#define SETUP_HOTSPOT_S (15 * 60)

static void action_task(void *arg)
{
    if ((svc_event_t)(intptr_t)arg == SVC_RESET) {
        ESP_LOGW(TAG, "service button held 10 s: factory reset");
        /* held until the restart: no config or password save can land after the erase */
        supervisor_cfg_lock();
        esp_err_t e = boxcfg_factory_reset();
        if (e != ESP_OK) ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(e));
        vTaskDelay(pdMS_TO_TICKS(200)); /* let the log line out */
        breadcrumb_restart(BC_WHY_BUTTON_RESET);
    }
    ESP_LOGW(TAG, "service button held 5 s: setup hotspot on for 15 minutes");
    netmgr_ap_force(SETUP_HOTSPOT_S);
    vTaskDelete(NULL);
}

static void poll_task(void *arg)
{
    (void)arg;
    svcbtn_t b;
    svcbtn_init(&b);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SVCBTN_POLL_MS));
        const svc_event_t e = svcbtn_step(&b, gpio_get_level(SVCBTN_GPIO) == 0, esp_timer_get_time());
        switch (e) {
        case SVC_SETUP_STAGE:
            ESP_LOGI(TAG, "held 5 s: release now for the setup hotspot, hold on to 10 s for a factory reset");
            break;
        case SVC_RESET_STAGE:
            ESP_LOGW(TAG, "held 10 s: release now for a factory reset");
            break;
        case SVC_SHORT:
            surfaces_maint_toggle(); /* the surfaces task opens or closes it, and says so (once) */
            break;
        case SVC_SETUP:
        case SVC_RESET:
            if (xTaskCreate(action_task, "svcbtn_act", ACTION_STACK, (void *)(intptr_t)e, 5, NULL) != pdPASS)
                ESP_LOGE(TAG, "service button action not started: no memory");
            break;
        default:
            break;
        }
    }
}

esp_err_t svcbtn_start(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << SVCBTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&io);
    if (e != ESP_OK) return e;
    if (xTaskCreateWithCaps(poll_task, "svcbtn", POLL_STACK, NULL, 2, NULL, MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
