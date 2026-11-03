#include <string.h>

#include "netmgr/wifiscan.h"

/* a before b: stronger first, then by name */
static bool before(const wifiscan_ap_t *a, const wifiscan_ap_t *b)
{
    return a->rssi != b->rssi ? a->rssi > b->rssi : strcmp(a->ssid, b->ssid) < 0;
}

size_t wifiscan_tidy(wifiscan_ap_t *aps, size_t n, size_t max)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (!aps[i].ssid[0]) continue;
        size_t j = 0;
        while (j < k && strcmp(aps[j].ssid, aps[i].ssid)) j++;
        if (j < k) {
            if (aps[i].rssi > aps[j].rssi) aps[j] = aps[i];
        } else {
            aps[k++] = aps[i];
        }
    }
    for (size_t i = 1; i < k; i++) { /* insertion sort: a few dozen records */
        wifiscan_ap_t v = aps[i];
        size_t j = i;
        for (; j > 0 && before(&v, &aps[j - 1]); j--) aps[j] = aps[j - 1];
        aps[j] = v;
    }
    return k < max ? k : max;
}

const char *wifiscan_auth_name(int auth)
{
    switch (auth) {
    case WIFISCAN_AUTH_OPEN: return "open";
    case WIFISCAN_AUTH_WEP: return "wep";
    case WIFISCAN_AUTH_WPA_PSK: return "wpa_psk";
    case WIFISCAN_AUTH_WPA2_PSK: return "wpa2_psk";
    case WIFISCAN_AUTH_WPA_WPA2_PSK: return "wpa_wpa2_psk";
    case WIFISCAN_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";
    case WIFISCAN_AUTH_WPA3_PSK: return "wpa3_psk";
    case WIFISCAN_AUTH_WPA2_WPA3_PSK: return "wpa2_wpa3_psk";
    default: return "other";
    }
}

wifiscan_act_t wifiscan_decide(bool running, bool have_last, int64_t now_ms, int64_t last_end_ms)
{
    if (running) return WIFISCAN_BUSY;
    /* a clock that went backwards counts as "just ended" */
    if (have_last && now_ms - last_end_ms < WIFISCAN_GAP_MS) return WIFISCAN_CACHED;
    return WIFISCAN_RUN;
}

bool wifiscan_timed_out(bool running, int64_t now_ms, int64_t start_ms)
{
    return running && now_ms - start_ms >= WIFISCAN_TIMEOUT_MS;
}

int64_t wifiscan_age_ms(bool have_last, int64_t now_ms, int64_t last_end_ms)
{
    if (!have_last) return -1;
    return now_ms > last_end_ms ? now_ms - last_end_ms : 0;
}

bool wifiscan_done_is_ours(bool scan_active, bool started, uint32_t status)
{
    return scan_active && started && status == 0;
}

wifiscan_conn_t wifiscan_connect_act(bool scan_active, bool sta_wanted, bool sta_connected)
{
    if (!sta_wanted || sta_connected) return WIFISCAN_CONN_SKIP;
    return scan_active ? WIFISCAN_CONN_DEFER : WIFISCAN_CONN_GO;
}

uint32_t wifiscan_done_status(bool has_data, uint32_t status) { return has_data ? status : 0; }
