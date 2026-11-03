#pragma once
/* The ESP32-C6's ESP-Hosted firmware, carried in this P4 image and kept on
 * the C6 (c6fw.c explains how). */
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, after esp_hosted_connect_to_slave succeeded (netmgr's hosted.c
 * does). Compares the C6's ESP-Hosted version with the image this build
 * carries. When they differ and the running P4 image is confirmed (not
 * pending verify), updates the C6 over ESP-Hosted's co-processor OTA and
 * restarts the P4: it then does not return. While the P4 image is pending
 * verify, a background task polls its state every 5 s and updates once it is
 * confirmed. At most C6FW_MAX_TRIES (2) updates per power-on.
 *
 * Returns ESP_OK when Wi-Fi can run on the C6 as it is now: it runs the
 * carried firmware, or one of the same major version (same protocol) while
 * the update waits, gave up or failed. ESP_ERR_INVALID_VERSION when it runs an
 * incompatible or unknown version; the update's error when an update failed
 * and the C6 stays incompatible. */
esp_err_t c6fw_check(void);

/* For the status page. running: the version the C6 reported at c6fw_check,
 * "x.y.z", or "unknown". carried: the image this build carries, "x.y.z", or
 * "none" when it was built without build-c6/ (tools/build_c6.sh). */
void c6fw_get_versions(char *running, size_t n1, char *carried, size_t n2);

#ifdef __cplusplus
}
#endif
