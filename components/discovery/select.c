#include <string.h>
#include <strings.h>
#include "discovery/discovery.h"

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

sel_result_t discovery_select(const box_config_t *cfg, const companion_instance_t *list, int n,
                              char *host, size_t host_cap, uint16_t *port,
                              char *pin_id_out, size_t pin_cap)
{
    copy_bounded(pin_id_out, pin_cap, "");
    copy_bounded(host, host_cap, "");
    if (port) *port = 0;

    if (!cfg->companion_auto) {
        if (cfg->companion_host[0] == '\0') return SEL_NONE;
        copy_bounded(host, host_cap, cfg->companion_host);
        if (port) *port = cfg->companion_port;
        return SEL_CONNECT;
    }

    if (cfg->companion_pin_id[0] != '\0') {
        for (int i = 0; list && i < n; i++) {
            if (strcmp(list[i].id, cfg->companion_pin_id) == 0) {
                copy_bounded(host, host_cap, list[i].host);
                if (port) *port = list[i].port;
                return SEL_CONNECT;
            }
        }
        if (cfg->companion_host[0] == '\0') return SEL_NONE;
        copy_bounded(host, host_cap, cfg->companion_host);
        if (port) *port = cfg->companion_port;
        return SEL_CONNECT;
    }

    if (n <= 0) return SEL_NONE;
    if (n > 1) return SEL_AMBIGUOUS;
    copy_bounded(host, host_cap, list[0].host);
    if (port) *port = list[0].port;
    copy_bounded(pin_id_out, pin_cap, list[0].id);
    return SEL_CONNECT;
}

bool discovery_pin_posted(box_config_t *cfg, const companion_instance_t *list, int n, const char *host,
                          uint16_t port)
{
    if (!host || !host[0]) return false;
    for (int i = 0; list && i < n; i++) {
        if (strcasecmp(list[i].host, host) != 0 || (port && list[i].port != port)) continue;
        cfg->companion_auto = true;
        copy_bounded(cfg->companion_pin_id, sizeof cfg->companion_pin_id, list[i].id);
        copy_bounded(cfg->companion_host, sizeof cfg->companion_host, list[i].host);
        cfg->companion_port = list[i].port;
        return true;
    }
    return false;
}
