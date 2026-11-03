#include <stdlib.h>
#include <string.h>

#include "boxconfig/config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "boxcfg";
#define NS "boxcfg"
#define KEY_CFG "cfg"

static uint32_t rand32(void) { return esp_random(); }

static void defaults(box_config_t *c)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    boxcfg_defaults(c, mac, rand32);
}

/* Set when the stored config couldn't be read or loaded whole: it stays in NVS as it is until a
 * deliberate save (boxcfg_save) replaces it; automatic saves check boxcfg_stored_kept first. */
static bool s_kept;

bool boxcfg_stored_kept(void) { return s_kept; }

/* Loads from NVS over the defaults, field by field (boxcfg_from_json_lenient): an invalid or unknown field is skipped
 * and logged; unreadable or not JSON: defaults. *stored (if not NULL) gets the stored JSON (caller frees), or NULL
 * when there was none or it could not be read; *how says which. Never writes NVS. */
static esp_err_t load(box_config_t *c, char **stored, boxcfg_load_t *how)
{
    if (stored) *stored = NULL;
    *how = BOXCFG_LOAD_ABSENT;
    defaults(c);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) {
        *how = BOXCFG_LOAD_FAILED;
        ESP_LOGW(TAG, "settings unreadable (%s): running on defaults, the stored ones are kept", esp_err_to_name(e));
        return e;
    }

    size_t len = 0;
    e = nvs_get_str(h, KEY_CFG, NULL, &len);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return ESP_OK;
    }
    char *json = (e == ESP_OK) ? malloc(len) : NULL;
    if (e == ESP_OK && !json) e = ESP_ERR_NO_MEM;
    if (json) e = nvs_get_str(h, KEY_CFG, json, &len);
    nvs_close(h);
    if (e != ESP_OK) {
        *how = BOXCFG_LOAD_FAILED;
        ESP_LOGW(TAG, "settings unreadable (%s): running on defaults, the stored ones are kept", esp_err_to_name(e));
        free(json);
        return ESP_OK;
    }
    char skipped[160];
    const int n = boxcfg_from_json_lenient(c, json, skipped, sizeof skipped);
    if (n < 0) {
        *how = BOXCFG_LOAD_FAILED;
        ESP_LOGW(TAG, "stored settings are not a JSON object: running on defaults, the stored ones are kept");
    } else if (n > 0) {
        *how = BOXCFG_LOAD_PARTIAL;
        ESP_LOGW(TAG, "stored settings: %d field(s) skipped (%s); the rest loaded, the stored ones are kept", n, skipped);
    } else {
        *how = BOXCFG_LOAD_CLEAN;
    }
    if (stored) *stored = json;
    else free(json);
    return ESP_OK;
}

static esp_err_t save_json(const char *json)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_str(h, KEY_CFG, json);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    return e;
}

esp_err_t boxcfg_load_and_store(box_config_t *c, bool *saved)
{
    if (saved) *saved = false;
    char *stored = NULL;
    boxcfg_load_t how;
    esp_err_t e = load(c, &stored, &how);
    s_kept = how == BOXCFG_LOAD_FAILED || how == BOXCFG_LOAD_PARTIAL;
    if (e != ESP_OK) {
        free(stored);
        return e;
    }
    char *now = boxcfg_to_json(c, true);
    if (!now) {
        free(stored);
        return ESP_ERR_NO_MEM;
    }
    if (boxcfg_should_store(how, stored && strcmp(stored, now) == 0)) {
        e = save_json(now);
        if (e == ESP_OK && saved) *saved = true;
    }
    free(stored);
    free(now);
    return e;
}

esp_err_t boxcfg_save(const box_config_t *c)
{
    char *json = boxcfg_to_json(c, true);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t e = save_json(json);
    free(json);
    if (e == ESP_OK) s_kept = false; /* a deliberate save replaced what was kept */
    return e;
}

esp_err_t boxcfg_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_erase_all(h);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
