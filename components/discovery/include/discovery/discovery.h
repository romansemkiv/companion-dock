#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "boxconfig/config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char id[64];
    char host[48];
    uint16_t port;
    char version[24];
    char name[48]; /* its mDNS instance name (what the decks' Choose screen shows); "" when none */
} companion_instance_t;

typedef enum { SEL_NONE, SEL_CONNECT, SEL_AMBIGUOUS } sel_result_t;

/* Pure policy. host/pin_id_out are always NUL-terminated within their caps.
 * pin_id_out is "" on every call and set only when not pinned and exactly one
 * instance is visible (caller should persist it as the pin). */
sel_result_t discovery_select(const box_config_t *cfg, const companion_instance_t *list, int n,
                              char *host, size_t host_cap, uint16_t *port,
                              char *pin_id_out, size_t pin_cap);

/* Pure. Companion's Discover -> setup posted host (and port; 0 when none was posted) to /api/config
 *: when a visible instance is at that address (host compared without
 * case, port too when given), pins it: companion_auto true, companion_pin_id its id, and its host
 * and port stored as the last known address, so the box follows it when its IP changes. Returns
 * false, leaving cfg untouched, when none matches (the posted address stays a manual one). */
bool discovery_pin_posted(box_config_t *cfg, const companion_instance_t *list, int n, const char *host,
                          uint16_t port);

#ifdef ESP_PLATFORM
#include "esp_err.h"
/* Starts mDNS (hostname and instance from the box name), then the _companion-satellite._tcp
 * announcement and the Companion browse. In Network Dock mode (mode: the run's surface mode, as
 * main read it at boot) only mDNS itself: elgdock announces the docks. On mDNS init failure returns
 * the error; caller decides. */
esp_err_t discovery_start(const box_config_t *cfg, box_surface_mode_t mode);
/* Applies box rename / mdns_enabled changes (dock mode: the hostname only). */
void discovery_apply(const box_config_t *cfg);
/* Copies instances seen in the last 30 s; returns count. */
int discovery_instances(companion_instance_t *out, int max);
#endif

#ifdef __cplusplus
}
#endif
