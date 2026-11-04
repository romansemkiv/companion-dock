/* The ESP32-C6's ESP-Hosted firmware travels in this P4 image (tools/build_c6.sh
 * builds it into build-c6/, this component's CMakeLists embeds it with its
 * version) and the C6 is kept on it.
 *
 * At every start netmgr's hosted.c calls c6fw_check once the SDIO link is up,
 * before Wi-Fi (the decision: c6fw_decide in c6fw_logic.c). Another version on
 * the C6 (older or newer: a rolled-back P4 puts its own back), or no answer
 * about it: ESP-Hosted's co-processor OTA writes the carried image to the C6's
 * other slot and activates it (the C6 restarts into it), then the P4 restarts
 * to meet it afresh, as Espressif's example does.
 *
 * Invariants:
 * - Never while this P4 image is pending verify (its first start after an OTA
 *   update): the bootloader would take that restart for a failed start and
 *   roll back. The update then waits on a task of its own that polls the
 *   image state every 5 s (supervisor.c's OTA self-check confirms it); Wi-Fi
 *   runs meanwhile when the C6 speaks the same major version (c6fw_usable).
 *   An image flashed over serial has no otadata entry and counts as confirmed.
 * - At most C6FW_MAX_TRIES updates per power-on: the counter lives in RTC
 *   memory that survives esp_restart (RTC_NOINIT_ATTR) and is cleared at a
 *   power-on (reset reason, or the magic missing).
 * - A power cut during the copy leaves the C6 on its old slot: the next start
 *   copies again.
 * - A build without build-c6/ stops (CMake error) unless asked for with
 *   -DCBX_NO_C6_IMAGE=ON (C6FW_HAVE_IMAGE 0): it carries no image, never
 *   updates, only reports a C6 on another version and uses it when it speaks
 *   the same major version. */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "breadcrumb/breadcrumb.h"
#include "c6fw/c6fw.h"
#include "c6fw_logic.h"
#include "esp_attr.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "feedback/feedback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#ifndef C6FW_HAVE_IMAGE
#error "C6FW_HAVE_IMAGE is set by components/c6fw/CMakeLists.txt"
#endif
#if !SOC_RTC_MEM_SUPPORTED
#error "c6fw keeps its try counter in RTC memory (RTC_NOINIT_ATTR)"
#endif

#if C6FW_HAVE_IMAGE
#if !EH_HOST_FEAT_OTA_READY
#error "C6 updating needs ESP-Hosted's co-processor OTA (CONFIG_ESP_HOSTED_HOST_FEAT_OTA)"
#endif
#include "c6fw_version.h" /* C6FW_MAJOR, C6FW_MINOR, C6FW_PATCH: build-c6/version.txt */
_Static_assert(C6FW_MAJOR == ESP_HOSTED_VERSION_MAJOR_1 && C6FW_MINOR == ESP_HOSTED_VERSION_MINOR_1 &&
                   C6FW_PATCH == ESP_HOSTED_VERSION_PATCH_1,
               "build-c6/ holds the C6 firmware of another ESP-Hosted version: run tools/build_c6.sh again");
extern const uint8_t c6fw_start[] asm("_binary_network_adapter_bin_start");
extern const uint8_t c6fw_end[] asm("_binary_network_adapter_bin_end");
#endif

static const char *TAG = "c6fw";

#define RTC_MAGIC 0x43364657u /* "C6FW" */
/* Bytes per co-processor OTA write: a proven size, below ESP-Hosted
 * 3.0.9's per-RPC limit (EH_RPC_OTA_CHUNK_MAX, 1536, not in a public header). */
#define CHUNK 1400
#define CONFIRM_POLL_MS 5000
#define VER_LEN 24

/* Updates since the last power-on. */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t tries;
} s_rtc;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED; /* guards s_running */
static atomic_bool s_updating; /* the C6's firmware is being written (c6fw_updating) */
typedef esp_err_t (*restart_fn_t)(const char *why);
typedef bool (*pending_fn_t)(void);
static _Atomic(restart_fn_t) s_restart; /* c6fw_set_restart's */
static _Atomic(pending_fn_t) s_restart_pending;

bool c6fw_updating(void) { return atomic_load(&s_updating); }
void c6fw_set_restart(esp_err_t (*restart)(const char *why), bool (*restart_pending)(void))
{
    atomic_store(&s_restart_pending, restart_pending);
    atomic_store(&s_restart, restart);
}
static char s_running[VER_LEN] = "unknown";
static bool s_checked;

/* The C6 firmware this image goes with: its own ESP-Hosted's (also the
 * carried image's, by the _Static_assert above when one is embedded). */
static c6ver_t carried(void)
{
    return (c6ver_t){ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1};
}

/* What the C6 says it runs; was: as text ("unknown" without an answer). */
static bool read_running(c6ver_t *v, char was[VER_LEN])
{
    esp_hosted_coprocessor_fwver_t f = {0};
    bool known = esp_hosted_get_coprocessor_fwversion(&f) == 0;
    *v = (c6ver_t){(uint8_t)f.major1, (uint8_t)f.minor1, (uint8_t)f.patch1};
    if (known)
        snprintf(was, VER_LEN, "%" PRIu32 ".%" PRIu32 ".%" PRIu32, f.major1, f.minor1, f.patch1);
    else
        strlcpy(was, "unknown", VER_LEN);
    portENTER_CRITICAL(&s_mux);
    memcpy(s_running, was, VER_LEN);
    portEXIT_CRITICAL(&s_mux);
    return known;
}

/* The running image is not confirmed yet (first start after an OTA update). */
static bool image_pending(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

#if C6FW_HAVE_IMAGE
static esp_err_t update(void)
{
    size_t len = (size_t)(c6fw_end - c6fw_start);
    esp_err_t e = esp_hosted_slave_ota_begin();
    for (size_t off = 0; e == ESP_OK && off < len; off += CHUNK)
        e = esp_hosted_slave_ota_write(c6fw_start + off, (uint32_t)(len - off < CHUNK ? len - off : CHUNK));
    if (e == ESP_OK) e = esp_hosted_slave_ota_end();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "the C6's update to ESP-Hosted %d.%d.%d did not finish: %s", C6FW_MAJOR, C6FW_MINOR,
                 C6FW_PATCH, esp_err_to_name(e));
        return e;
    }
    e = esp_hosted_slave_ota_activate();
    if (e != ESP_OK)
        ESP_LOGE(TAG, "the C6 did not confirm switching to ESP-Hosted %d.%d.%d: %s", C6FW_MAJOR, C6FW_MINOR,
                 C6FW_PATCH, esp_err_to_name(e));
    return e;
}

/* The update and the P4's restart (this image is confirmed); returns only on
 * failure, or (deferred) with the restart on its way through c6fw_set_restart's. */
static esp_err_t update_and_restart(const char *was, bool deferred)
{
    atomic_store(&s_updating, true); /* then the check: webui's restart task checks the restart, then this */
    const pending_fn_t pending = deferred ? atomic_load(&s_restart_pending) : NULL;
    if (pending && pending()) {
        atomic_store(&s_updating, false);
        ESP_LOGW(TAG, "the C6 runs ESP-Hosted %s: a restart is on its way, it is updated at the next start", was);
        return ESP_OK;
    }
    s_rtc.tries++;
    ESP_LOGW(TAG, "the C6 runs ESP-Hosted %s: updating it to %d.%d.%d (%u bytes, try %" PRIu32 " of %d)", was,
             C6FW_MAJOR, C6FW_MINOR, C6FW_PATCH, (unsigned)(c6fw_end - c6fw_start), s_rtc.tries, C6FW_MAX_TRIES);
    feedback_firmware_update(true); /* the status light's purple blink */
    esp_err_t e = update();
    atomic_store(&s_updating, false);
    if (e != ESP_OK) {
        feedback_firmware_update(false);
        return e;
    }
    ESP_LOGW(TAG, "C6 firmware %s -> %d.%d.%d: restarting to meet it", was, C6FW_MAJOR, C6FW_MINOR, C6FW_PATCH);
    vTaskDelay(pdMS_TO_TICKS(500)); /* the C6 restarting into its new slot */
    /* after the confirmation an upload may be under way: webui's restart waits for it (a restart already on its way,
     * ESP_ERR_INVALID_STATE, is one too) */
    const restart_fn_t r = deferred ? atomic_load(&s_restart) : NULL;
    if (r && r(BC_WHY_C6_UPDATE) != ESP_ERR_NO_MEM) return ESP_OK;
    breadcrumb_restart(BC_WHY_C6_UPDATE);
}

/* The update that waits for this image's confirmation. */
static void deferred_task(void *arg)
{
    (void)arg;
    while (image_pending()) vTaskDelay(pdMS_TO_TICKS(CONFIRM_POLL_MS));
    char was[VER_LEN];
    c6ver_t r;
    bool known = read_running(&r, was);
    switch (c6fw_decide(r, known, carried(), false, (int)s_rtc.tries, false)) {
    case C6FW_UPDATE:
        update_and_restart(was, true); /* returns on failure (logged), or with the restart on its way */
        break;
    case C6FW_GIVE_UP:
        ESP_LOGW(TAG, "the C6 stays on ESP-Hosted %s after %d updates (again after a power-on)", was,
                 C6FW_MAX_TRIES);
        break;
    default:
        break;
    }
    vTaskDelete(NULL);
}
#endif

esp_err_t c6fw_check(void)
{
    if (s_checked) return ESP_ERR_INVALID_STATE;
    s_checked = true;
    if (esp_reset_reason() == ESP_RST_POWERON || s_rtc.magic != RTC_MAGIC) {
        s_rtc.magic = RTC_MAGIC;
        s_rtc.tries = 0;
    }

    char was[VER_LEN];
    c6ver_t r, c = carried();
    bool known = read_running(&r, was);
    bool usable = c6fw_usable(r, known, c);
    esp_err_t err = ESP_ERR_INVALID_VERSION;
    switch (c6fw_decide(r, known, c, image_pending(), (int)s_rtc.tries, !C6FW_HAVE_IMAGE)) {
    case C6FW_OK:
        s_rtc.tries = 0;
        ESP_LOGI(TAG, "the C6 runs ESP-Hosted %s, the firmware this image carries", was);
        return ESP_OK;
    case C6FW_UPDATE:
#if C6FW_HAVE_IMAGE
        err = update_and_restart(was, false); /* returns only on failure */
#endif
        break;
    case C6FW_WAIT_CONFIRM:
#if C6FW_HAVE_IMAGE
        if (xTaskCreate(deferred_task, "c6fw", 4096, NULL, 3, NULL) == pdPASS)
            ESP_LOGW(TAG, "the C6 runs ESP-Hosted %s: it is updated to %d.%d.%d once this image is confirmed", was,
                     c.major, c.minor, c.patch);
        else
            ESP_LOGE(TAG, "no memory for the C6's deferred update: it waits for the next start");
#endif
        break;
    case C6FW_GIVE_UP:
        ESP_LOGW(TAG, "the C6 stays on ESP-Hosted %s after %d updates to %d.%d.%d (again after a power-on)", was,
                 C6FW_MAX_TRIES, c.major, c.minor, c.patch);
        break;
    case C6FW_MISMATCH:
        ESP_LOGW(TAG,
                 "the C6 runs ESP-Hosted %s, the host %d.%d.%d; this build carries no C6 image "
                 "(run tools/build_c6.sh and rebuild)",
                 was, c.major, c.minor, c.patch);
        break;
    }
    if (usable) return ESP_OK;
    ESP_LOGE(TAG, "the C6's ESP-Hosted %s does not speak %d.x: Wi-Fi stays off", was, c.major);
    return err;
}

void c6fw_get_versions(char *running, size_t n1, char *carried_out, size_t n2)
{
    if (running && n1) {
        char r[VER_LEN];
        portENTER_CRITICAL(&s_mux);
        memcpy(r, s_running, VER_LEN);
        portEXIT_CRITICAL(&s_mux);
        strlcpy(running, r, n1);
    }
    if (carried_out && n2) {
#if C6FW_HAVE_IMAGE
        snprintf(carried_out, n2, "%d.%d.%d", C6FW_MAJOR, C6FW_MINOR, C6FW_PATCH);
#else
        strlcpy(carried_out, "none", n2);
#endif
    }
}
