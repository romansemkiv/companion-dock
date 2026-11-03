/* ESP glue for the dispatcher (rpc.h): the lock that runs handlers one at a time and the device id.
 * Handlers never call rpc_call themselves (the lock is not recursive). */
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rpc/rpc.h"

static const char *TAG = "rpc";

static SemaphoreHandle_t s_mtx;
static StaticSemaphore_t s_mtx_buf;

static void lock(void) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

static void overflow(const char *name)
{
    ESP_LOGE(TAG, "method table full (%d): %s and later ones dropped", RPC_MAX_METHODS, name);
}

esp_err_t rpc_init(void)
{
    if (s_mtx) return ESP_OK;
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_ETH);
    char id[32];
    snprintf(id, sizeof id, "companionbox-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    rpc_set_device_id(id);
    rpc_set_overflow_hook(overflow);
    rpc_set_lock(lock, unlock);
    return ESP_OK;
}
