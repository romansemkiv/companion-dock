/* The feedback task: every TICK_MS it steps the status tracker (fbstatus.c) with the supervisor's
 * last report, queues the status sounds it starts (ready, connected, problem) and shows the light.
 * Event sounds (deck in / out, the key click) are queued straight from the caller's task by
 * feedback_event: a spinlock-guarded copy of the settings and a queue send that never waits, so a
 * key-down is never held up by a sound. The same call asks this task for the event's flash:
 * a flag under the spinlock and a task notify, also with the sound off.
 *
 * The light is shown while ledEnabled is on; Find my box and Feedback.Test's flash show also while it
 * is off (someone asked for them). Real events flash it too while ledEnabled and ledFlash are on. Its pin is opened again when ledPin changes. Find my box and
 * Feedback.Test play whatever the sound settings say (Find my box at no less than half volume). */
#include <stdatomic.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "feedback/feedback.h"
#include "feedback_priv.h"
#include "fbstatus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "feedback";

#define TICK_MS 20
#define STACK 3072 /* PSRAM: the tracker, RMT and a queue send */
#define PRIO 2
#define IDENTIFY_MIN_VOLUME 50

typedef struct {
    bool speaker, ready, connected, problem, plug, click, led, flash;
    int volume, pin, brightness;
} settings_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* s_mux */
static settings_t s_set;
static fb_status_t s_status;
static bool s_identify_req;
static int s_flash_req = -1; /* fb_event_t; -1 none */

static TaskHandle_t s_task;
static fb_track_t s_track;       /* feedback task only */
static atomic_bool s_fw_update; /* feedback_firmware_update */

static settings_t settings(void)
{
    taskENTER_CRITICAL(&s_mux);
    const settings_t s = s_set;
    taskEXIT_CRITICAL(&s_mux);
    return s;
}

static bool sound_on(const settings_t *s, fb_event_t e)
{
    if (!s->speaker || s->volume <= 0) return false;
    switch (e) {
    case FB_EV_READY: return s->ready;
    case FB_EV_CONNECTED: return s->connected;
    case FB_EV_PROBLEM: return s->problem;
    case FB_EV_DECK_IN:
    case FB_EV_DECK_OUT: return s->plug;
    case FB_EV_KEY_CLICK:
    case FB_EV_CONFIRM:
    case FB_EV_LOCKED: return s->click;
    default: return false;
    }
}

static void feedback_task(void *arg)
{
    (void)arg;
    int open_pin = -1, failed_pin = -1;
    fb_led_t shown = FB_LED_STARTING;
    bool logged = false;
    for (;;) {
        taskENTER_CRITICAL(&s_mux);
        const settings_t set = s_set;
        fb_status_t st = s_status;
        const bool identify = s_identify_req;
        const int flash = s_flash_req;
        s_identify_req = false;
        s_flash_req = -1;
        taskEXIT_CRITICAL(&s_mux);

        st.update |= atomic_load(&s_fw_update); /* the C6's update, before the supervisor runs too */
        const int64_t now = esp_timer_get_time();
        if (identify) fb_track_identify(&s_track, now);
        if (flash >= 0) fb_track_flash(&s_track, (fb_event_t)flash, now);
        const uint32_t sounds = fb_track_step(&s_track, &st, now);
        for (int e = 0; e < FB_EV_COUNT; e++) {
            if (!(sounds & (1u << e))) continue;
            if (sound_on(&set, (fb_event_t)e)) speaker_play((fb_event_t)e, set.volume);
            if (fb_flash_wanted((fb_event_t)e, set.led, set.flash)) fb_track_flash(&s_track, (fb_event_t)e, now);
        }

        if (set.pin != open_pin && set.pin != failed_pin) {
            led_close();
            open_pin = led_open(set.pin) ? set.pin : -1;
            failed_pin = open_pin < 0 ? set.pin : -1;
            if (open_pin < 0) ESP_LOGE(TAG, "the status light on GPIO%d didn't start (no RMT channel)", set.pin);
        }
        const fb_led_t s = fb_track_led(&s_track, now);
        const bool flashing = now >= s_track.flash_start_us && now < s_track.flash_end_us;
        const bool show = set.led || s == FB_LED_IDENTIFY || flashing;
        led_show(show ? fb_rgb_scale(fb_track_color(&s_track, now), set.brightness) : (fb_rgb_t){0, 0, 0});
        if (s != shown || !logged) {
            ESP_LOGI(TAG, "status light: %s%s", fb_led_name(s), set.led ? "" : " (light off in the settings)");
            shown = s;
            logged = true;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
    }
}

void feedback_apply(const box_config_t *cfg)
{
    const settings_t s = {
        .speaker = cfg->speaker_enabled, .ready = cfg->sound_ready, .connected = cfg->sound_connected,
        .problem = cfg->sound_problem, .plug = cfg->sound_deck_plug, .click = cfg->sound_key_click,
        .led = cfg->led_enabled, .flash = cfg->led_flash, .volume = cfg->speaker_volume, .pin = cfg->led_pin,
        .brightness = cfg->led_brightness,
    };
    taskENTER_CRITICAL(&s_mux);
    s_set = s;
    taskEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void feedback_amp_off(void)
{
    speaker_amp_off();
}

void feedback_firmware_update(bool on)
{
    atomic_store(&s_fw_update, on);
    if (s_task) xTaskNotifyGive(s_task);
}

esp_err_t feedback_start(const box_config_t *cfg)
{
    if (s_task) return ESP_OK;
    feedback_apply(cfg);
    fb_track_init(&s_track);
    const esp_err_t e = speaker_start(); /* the light goes on without it */
    if (xTaskCreateWithCaps(feedback_task, "feedback", STACK, NULL, PRIO, &s_task, MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    return e;
}

void feedback_status(const fb_status_t *st)
{
    taskENTER_CRITICAL(&s_mux);
    s_status = *st;
    taskEXIT_CRITICAL(&s_mux);
}

/* Asks the feedback task for ev's flash: a flag and a notify, never waits. */
static void request_flash(fb_event_t ev)
{
    taskENTER_CRITICAL(&s_mux);
    s_flash_req = (int)ev;
    taskEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void feedback_event(fb_event_t ev)
{
    const settings_t s = settings();
    if (sound_on(&s, ev)) speaker_play(ev, s.volume);
    if (fb_flash_wanted(ev, s.led, s.flash)) request_flash(ev);
}

void feedback_identify(void)
{
    const settings_t s = settings();
    taskENTER_CRITICAL(&s_mux);
    s_identify_req = true;
    taskEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
    speaker_play(FB_EV_IDENTIFY, s.volume < IDENTIFY_MIN_VOLUME ? IDENTIFY_MIN_VOLUME : s.volume);
    ESP_LOGI(TAG, "Find my box: beeping, the light blinks white for 30 s");
}

void feedback_test(fb_event_t ev, int volume)
{
    if (ev == FB_EV_IDENTIFY) {
        feedback_identify();
        return;
    }
    if ((unsigned)ev >= FB_EV_COUNT) return;
    if (volume < 0) volume = settings().volume;
    request_flash(ev);
    if (volume > 0) speaker_play(ev, volume);
}

const char *feedback_speaker_error(void)
{
    return speaker_error();
}
