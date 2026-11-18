/* The service key (svcbtn.h): a small task polls the pin
 * every SVCBTN_POLL_MS through svcbtn_logic. It only reads a GPIO, so its stack can live in PSRAM;
 * each action runs on a task of its own with an internal stack, because the hotspot blocks while
 * Wi-Fi restarts and the factory reset erases flash. A short press (SVC_SHORT) asks the surfaces task to
 * open or close the decks' maintenance screen on demand: surfaces_maint_toggle only counts it, so
 * the open (dock mode: the deck's input held from Companion) and its log lines run on the surfaces task's
 * stack, not on this small one; it toggles from its own state, so two quick presses cancel out
 *. The poll is 10 ms so that the short press's 50 ms floor is several samples, not one (a glitch
 * never opens it). */
#include "svcbtn.h"

#include <stdatomic.h>
#include <stdint.h>

#include "boxconfig/config.h"
#include "deckui/screens.h"
#include "breadcrumb/breadcrumb.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "feedback/feedback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netmgr/netmgr.h"
#include "supervisor.h"
#include "surfaces/surfaces.h"
#include "svcbtn_logic.h"
#include "webui/webui.h"

static const char *TAG = "svcbtn";

#define SVCBTN_POLL_MS 10
#define POLL_STACK 4096   /* PSRAM: GPIO reads and one log line at a time (about 2 KB: the web log's line, vfprintf) */
#define ACTION_STACK 8192 /* internal: Wi-Fi restart / flash erase; the factory reset's esp_restart */
#define SETUP_HOTSPOT_S (15 * 60)

static void action_task(void *arg)
{
    if ((svc_event_t)(intptr_t)arg == SVC_RESET) {
        ESP_LOGW(TAG, "service button held 10 s: factory reset");
        /* the web page's way (webui_restart_later, the same config lock): not over an update's self-check or an upload
         * being written (erased before such a wait too), and joining a restart already on its way */
        const esp_err_t r = webui_restart_later(true, BC_WHY_BUTTON_RESET);
        if (r == ESP_OK) vTaskDelete(NULL);
        ESP_LOGE(TAG, "factory reset: the restart task didn't take it (%s): here", esp_err_to_name(r));
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

/* the settings (svcbtn_apply), read by the polling task */
static atomic_bool s_enabled = true;
static atomic_int s_pin = BOXCFG_KEY_PIN;
static atomic_int s_short = BOXCFG_KEY_ACT_MENU;
static atomic_int s_long = BOXCFG_KEY_ACT_FIND;

void svcbtn_apply(const box_config_t *cfg)
{
    atomic_store(&s_enabled, cfg->key_enabled);
    atomic_store(&s_pin, cfg->key_pin);
    atomic_store(&s_short, cfg->key_short);
    atomic_store(&s_long, cfg->key_long);
}

/* the pin as an input with its pull-up (the key pulls it to GND) */
static esp_err_t open_pin(int pin)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

/* A press's action (keyShort / keyLong). The ones that save a setting or restart go to the deck menu's worker
 * (webui_deck_action: the same RPC, a notice on the decks), as from a deck unlocked with the settings PIN: the key
 * is in the hand of whoever stands at the box, as the 10 s factory reset is. */
static void act(int a, const char *press)
{
    static const char *const NAME[] = {"the box menu", "Find my box", "nothing", "sound on / off", "light on / off",
                                       "restart"};
    ESP_LOGI(TAG, "%s press: %s", press, a >= 0 && a < 6 ? NAME[a] : "?");
    switch (a) {
    case BOXCFG_KEY_ACT_MENU:
        surfaces_maint_toggle(); /* the surfaces task opens or closes it, and says so (once) */
        break;
    case BOXCFG_KEY_ACT_FIND:
        feedback_identify();
        break;
    case BOXCFG_KEY_ACT_SOUND:
        webui_deck_action(DUI_A_SOUND, NULL, true);
        break;
    case BOXCFG_KEY_ACT_LIGHT:
        webui_deck_action(DUI_A_LIGHT, NULL, true);
        break;
    case BOXCFG_KEY_ACT_RESTART:
        webui_deck_action(DUI_A_RESTART, NULL, true);
        break;
    default:
        break;
    }
}

static void poll_task(void *arg)
{
    (void)arg;
    svcbtn_t b;
    svcbtn_init(&b);
    int pin = -1;           /* the pin open now; -1: none */
    bool was_down = false;  /* for webui_key_report */
    uint32_t presses = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SVCBTN_POLL_MS));
        const bool on = atomic_load(&s_enabled);
        const int want = on ? atomic_load(&s_pin) : -1;
        /* a pin the light still holds waits a poll: feedback_apply only tells the light's task, which lets go of
         * its old pin on its own time and resets that pin as it does (the key's input would be off again) */
        if (want != pin && (want < 0 || feedback_led_pin() != want)) {
            /* the old pin only loses its pull-up, never a reset: the light may have just taken it (one save moving
             * both), and a reset would cut its output off */
            if (pin >= 0) gpio_pullup_dis(pin);
            pin = want >= 0 && open_pin(want) == ESP_OK ? want : -1;
            svcbtn_init(&b); /* a new pin (or the key back on) counts only once it reads released */
            if (want >= 0) ESP_LOGI(TAG, "service key on GPIO%d%s", want, pin < 0 ? ": the pin could not be set up" : "");
            else ESP_LOGI(TAG, "service key off");
        }
        const bool down = pin >= 0 && gpio_get_level(pin) == 0;
        const svc_event_t e = pin >= 0 ? svcbtn_step(&b, down, esp_timer_get_time()) : SVC_NONE;
        if (down != was_down || e == SVC_SHORT || e == SVC_LONG) {
            was_down = down;
            if (e == SVC_SHORT || e == SVC_LONG) presses++;
            webui_key_report(down, presses); /* the web page's live state */
        }
        switch (e) {
        case SVC_PRESSED:
            feedback_event(FB_EV_KEY_CLICK); /* a click and a flash, as a deck key (the sound and light settings) */
            break;
        case SVC_LONG_STAGE:
            feedback_event(FB_EV_CONFIRM); /* each mark confirms: let go now for the long press */
            break;
        case SVC_SETUP_STAGE:
            feedback_event(FB_EV_CONFIRM);
            ESP_LOGI(TAG, "held 5 s: release now for the setup hotspot, hold on to 10 s for a factory reset");
            break;
        case SVC_RESET_STAGE:
            feedback_event(FB_EV_CONFIRM);
            ESP_LOGW(TAG, "held 10 s: release now for a factory reset");
            break;
        case SVC_SHORT:
            act(atomic_load(&s_short), "short");
            break;
        case SVC_LONG:
            act(atomic_load(&s_long), "long");
            break;
        case SVC_SETUP:
        case SVC_RESET:
            if (xTaskCreate(action_task, "svcbtn_act", ACTION_STACK, (void *)(intptr_t)e, 3 /* (docs/manual.md, Task priorities) */, NULL) != pdPASS)
                ESP_LOGE(TAG, "service button action not started: no memory");
            break;
        default:
            break;
        }
    }
}

esp_err_t svcbtn_start(void)
{
    /* the pin is opened by the task itself, at its first poll (and again whenever keyPin changes) */
    if (xTaskCreateWithCaps(poll_task, "svcbtn", POLL_STACK, NULL, 2, NULL, MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
