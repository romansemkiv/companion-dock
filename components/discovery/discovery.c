#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include "discovery/discovery.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

#define TAG "discovery"
#define MAX_INSTANCES 8
#define BROWSE_PERIOD_MS 10000
#define BROWSE_TIMEOUT_MS 3000
#define ENTRY_TTL_US (30LL * 1000 * 1000)

typedef struct {
    companion_instance_t inst;
    int64_t seen_us;
    bool used;
} entry_t;

static entry_t s_table[MAX_INSTANCES];
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_started; /* mDNS initialised and named */
static bool s_dock;    /* Network Dock mode: no Satellite announcement, no Companion browsing */
static bool s_announcing;
static char s_name[33];

static void apply_names(const char *box_name)
{
    char host[33];
    boxcfg_hostname(box_name, host, sizeof host); /* at most 32 chars */
    esp_err_t e = mdns_hostname_set(host);
    if (e != ESP_OK) ESP_LOGW(TAG, "hostname_set: %s", esp_err_to_name(e));
    e = mdns_instance_name_set(box_name);
    if (e != ESP_OK) ESP_LOGW(TAG, "instance_name_set: %s", esp_err_to_name(e));
}

static void announce_on(const char *box_name)
{
    mdns_txt_item_t txt[1] = {{"restEnabled", "true"}};
    esp_err_t e = mdns_service_add(box_name, "_companion-satellite", "_tcp", 80, txt, 1);
    if (e == ESP_OK) {
        s_announcing = true;
    } else {
        ESP_LOGW(TAG, "service_add: %s", esp_err_to_name(e));
    }
}

static void announce_off(void)
{
    if (!s_announcing) return;
    esp_err_t e = mdns_service_remove("_companion-satellite", "_tcp");
    if (e != ESP_OK) ESP_LOGW(TAG, "service_remove: %s", esp_err_to_name(e));
    s_announcing = false;
}

static const char *txt_get(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) return r->txt[i].value ? r->txt[i].value : "";
    }
    return NULL;
}

static bool first_ipv4(const mdns_result_t *r, char *out, size_t cap)
{
    for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            esp_ip4addr_ntoa(&a->addr.u_addr.ip4, out, cap);
            return true;
        }
    }
    return false;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    strlcpy(dst, src, cap);
}

static void table_update(const companion_instance_t *in)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *slot = NULL, *free_slot = NULL, *oldest = &s_table[0];
    for (int i = 0; i < MAX_INSTANCES; i++) {
        entry_t *e = &s_table[i];
        if (e->used && strcmp(e->inst.id, in->id) == 0) { slot = e; break; }
        if (!free_slot && (!e->used || now - e->seen_us > ENTRY_TTL_US)) free_slot = e;
        if (e->seen_us < oldest->seen_us) oldest = e;
    }
    if (!slot) slot = free_slot ? free_slot : oldest;
    slot->inst = *in;
    slot->seen_us = now;
    slot->used = true;
    xSemaphoreGive(s_lock);
}

static void browse_once(void)
{
    mdns_result_t *res = NULL;
    esp_err_t e = mdns_query_ptr("_companion-satellite-tcp", "_tcp", BROWSE_TIMEOUT_MS, MAX_INSTANCES, &res);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "query_ptr: %s", esp_err_to_name(e));
        return;
    }
    for (const mdns_result_t *r = res; r; r = r->next) {
        const char *id = txt_get(r, "id");
        companion_instance_t in;
        memset(&in, 0, sizeof in);
        if (!id || id[0] == '\0' || !first_ipv4(r, in.host, sizeof in.host)) continue;
        copy_str(in.id, sizeof in.id, id);
        const char *ver = txt_get(r, "version");
        if (ver) copy_str(in.version, sizeof in.version, ver);
        if (r->instance_name) copy_str(in.name, sizeof in.name, r->instance_name);
        in.port = r->port;
        table_update(&in);
        ESP_LOGI(TAG, "companion id=%s %s:%u v=%s", in.id, in.host, in.port, in.version);
    }
    mdns_query_results_free(res);
}

static void browse_task(void *arg)
{
    (void)arg;
    for (;;) {
        browse_once();
        vTaskDelay(pdMS_TO_TICKS(BROWSE_PERIOD_MS));
    }
}

esp_err_t discovery_start(const box_config_t *cfg, box_surface_mode_t mode)
{
    if (s_started) return ESP_OK;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    esp_err_t e = mdns_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(e));
        return e;
    }
    s_started = true;
    s_dock = mode == BOX_SURFACE_DOCK;
    strlcpy(s_name, cfg->box_name, sizeof s_name);
    apply_names(s_name);
    if (s_dock) {
        ESP_LOGI(TAG, "Network Dock mode: no Satellite announcement, no Companion browsing");
        return ESP_OK; /* elgdock announces the docks */
    }
    if (cfg->mdns_enabled) announce_on(s_name);

    if (xTaskCreate(browse_task, "mdns_browse", 4096, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "browse task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void discovery_apply(const box_config_t *cfg)
{
    if (!s_started) return;
    if (strcmp(s_name, cfg->box_name) != 0) {
        strlcpy(s_name, cfg->box_name, sizeof s_name);
        apply_names(s_name);
        if (s_announcing) {
            /* service instance name is fixed at add time: re-add under the new name */
            announce_off();
            announce_on(s_name);
        }
    }
    if (s_dock) return; /* the mode is fixed until the next start */
    if (cfg->mdns_enabled && !s_announcing) announce_on(s_name);
    else if (!cfg->mdns_enabled) announce_off();
}

int discovery_instances(companion_instance_t *out, int max)
{
    if (!s_lock) return 0;
    int64_t now = esp_timer_get_time();
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_INSTANCES && n < max; i++) {
        if (s_table[i].used && now - s_table[i].seen_us <= ENTRY_TTL_US) out[n++] = s_table[i].inst;
    }
    xSemaphoreGive(s_lock);
    return n;
}
