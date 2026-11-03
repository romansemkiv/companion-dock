/* Wi-Fi on the Waveshare ESP32-P4-WIFI6-POE-ETH: the radio is the ESP32-C6's,
 * reached over SDIO with ESP-Hosted. esp_wifi_remote turns wifi.c's esp_wifi_*
 * calls into requests to the C6. No
 * Bluetooth.
 *
 * ESP-Hosted brings the link up on its own before app_main: it resets the C6
 * (GPIO54) and gives it a few seconds to answer. netmgr_hosted_start waits for
 * that (esp_hosted_init and esp_hosted_connect_to_slave return at once when its
 * start-up task is done, else wait for it), has c6fw keep the C6 on the
 * firmware this image carries (it may update the C6 and restart the P4), and
 * logs the version c6fw read (no second version request).
 *
 * Invariants:
 * - Without the C6, with the C6 on an incompatible version, or once it stops
 *   answering, the box runs on Ethernet alone: Wi-Fi and the hotspot are off
 *   and netmgr_hosted_error says why.
 * - A C6 that stops answering at run time (ESP-Hosted's transport failure)
 *   never restarts the P4: CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE
 *   is off, and on_transport turns Wi-Fi off until the next start
 *   (netmgr_radio_lost). It runs on the default event loop task and never
 *   talks to the C6. When that leaves the box without a network (no Ethernet
 *   for 3 min, Wi-Fi or the hotspot configured), the supervisor restarts it,
 *   which resets the C6 (main/supervisor_logic.h).
 * - s_error is non-empty whenever s_ready is false. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "c6fw/c6fw.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "netmgr_priv.h"

static const char *TAG = "c6";

/* Written by netmgr_hosted_start (before Wi-Fi starts) and on_transport;
 * read from netmgr's callers, the event task and wifi.c's retry timer. */
static volatile bool s_ready;
static char s_error[128] = "ESP-Hosted not started";

static void on_transport(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (!s_ready) return; /* posted once per failed transfer */
    strlcpy(s_error, "the C6 stopped answering over SDIO: Wi-Fi is off until the next start", sizeof s_error);
    s_ready = false;
    ESP_LOGE(TAG, "%s", s_error);
    netmgr_radio_lost();
}

esp_err_t netmgr_hosted_start(void)
{
    int rc = esp_hosted_init();
    if (rc == 0) {
        esp_err_t e = esp_event_handler_register(EH_HOST_EVENT, EH_HOST_EVENT_TRANSPORT_FAILURE, on_transport, NULL);
        if (e != ESP_OK) ESP_LOGW(TAG, "transport-failure handler: %s", esp_err_to_name(e));
        rc = esp_hosted_connect_to_slave();
    }
    if (rc != 0) {
        snprintf(s_error, sizeof s_error, "the C6 does not answer over SDIO (%d)", rc);
    } else {
        esp_err_t e = c6fw_check(); /* may update the C6 and restart the P4 */
        if (e == ESP_OK) {
            s_error[0] = '\0';
            s_ready = true;
        } else {
            char running[24], carried[24];
            c6fw_get_versions(running, sizeof running, carried, sizeof carried);
            snprintf(s_error, sizeof s_error, "the C6 runs ESP-Hosted %s, incompatible with this firmware's %d.%d.%d",
                     running, ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
        }
    }
    if (!s_ready) {
        ESP_LOGE(TAG, "no Wi-Fi: %s", s_error);
        return ESP_FAIL;
    }
    char running[24];
    c6fw_get_versions(running, sizeof running, NULL, 0); /* what c6fw_check read ("unknown" without an answer) */
    ESP_LOGI(TAG, "ESP-Hosted %s on the C6", strcmp(running, "unknown") ? running : "(version unknown)");
    return ESP_OK;
}

bool netmgr_hosted_ready(void)
{
    return s_ready;
}

const char *netmgr_hosted_error(void)
{
    return s_error;
}

