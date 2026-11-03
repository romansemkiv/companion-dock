/* NotifyStatus pushes to the WebSocket clients (the box has one
 * status object, so it is polled rather than fed by producers).
 *
 * A small task (PSRAM stack: it only queues work) checks every second whether a WebSocket client
 * listens; if so, the HTTP server task builds Sys.GetStatus (it reads flash, so not on a PSRAM
 * stack), compares it with the last one pushed, leaving out the fields that change by themselves
 * (uptime_s, heap, message.remaining_s), and pushes
 *   {"src":"<device id>","dst":"user","method":"NotifyStatus","params":{"sys":{...Sys.GetStatus...}}}
 * when it differs. A client that connects or signs in gets the next one whatever it is (notify_kick). */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "notify_logic.h"
#include "rpc/rpc.h"
#include "webui_priv.h"

static const char *TAG = "notify";

#define PERIOD_MS 1000
#define STACK 3072

static char *s_last; /* server task only: the last status pushed, without the self-changing fields */
static atomic_bool s_queued;
static atomic_bool s_have_last; /* s_last is set: one more run frees it once nobody listens */
static atomic_bool s_fresh;     /* push the next status even when it did not change */

void notify_kick(void) { atomic_store(&s_fresh, true); }

/* On the HTTP server task. */
static void work(void *arg)
{
    (void)arg;
    atomic_store(&s_queued, false);
    if (!web_ws_listening()) {
        free(s_last); /* the next listener starts from a fresh comparison */
        s_last = NULL;
        atomic_store(&s_have_last, false);
        return;
    }
    if (atomic_exchange(&s_fresh, false)) {
        free(s_last);
        s_last = NULL;
    }
    const rpc_caller_t self = {.src = "notify", .trusted = true};
    rpc_err_t err = {0};
    cJSON *st = rpc_call("Sys.GetStatus", NULL, &self, &err);
    if (!cJSON_IsObject(st)) {
        cJSON_Delete(st);
        return;
    }
    char *key = notify_status_key(st); /* without the self-changing fields, and no copy of the status */
    if (!key || (s_last && !strcmp(key, s_last))) {
        cJSON_free(key);
        cJSON_Delete(st);
        return;
    }
    free(s_last);
    s_last = key;
    atomic_store(&s_have_last, true);

    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "src", rpc_device_id());
    cJSON_AddStringToObject(frame, "dst", "user");
    cJSON_AddStringToObject(frame, "method", "NotifyStatus");
    cJSON *params = cJSON_AddObjectToObject(frame, "params");
    cJSON_AddItemToObject(params, "sys", st);
    char *text = cJSON_PrintUnformatted(frame);
    cJSON_Delete(frame);
    if (text) web_ws_broadcast_now(text);
    cJSON_free(text);
}

static void notify_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
        if ((web_ws_listening() || atomic_load(&s_have_last)) && !atomic_exchange(&s_queued, true) && web_queue_work(work, NULL) != ESP_OK)
            atomic_store(&s_queued, false);
    }
}

esp_err_t notify_start(void)
{
    if (xTaskCreateWithCaps(notify_task, "notify", STACK, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
