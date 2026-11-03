#pragma once
/* Wi-Fi network scan, the pure part (scan_logic.c, host-tested): tidying a scan's records, the
 * authentication names and the "run, busy or answer from the last scan" decision. The radio work is
 * netmgr_wifi_scan (netmgr.h). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFISCAN_MAX 30      /* networks in an answer */
#define WIFISCAN_RAW_MAX 64  /* records read from the driver before duplicates are dropped */
#define WIFISCAN_GAP_MS 3000 /* a new scan no sooner than this after the last one ended */
#define WIFISCAN_TIMEOUT_MS 10000 /* no SCAN_DONE by then: the scan is given up and the radio mode restored */

/* The authentication modes by their ESP-IDF wifi_auth_mode_t values (wifi.c asserts them). */
enum {
    WIFISCAN_AUTH_OPEN = 0,
    WIFISCAN_AUTH_WEP = 1,
    WIFISCAN_AUTH_WPA_PSK = 2,
    WIFISCAN_AUTH_WPA2_PSK = 3,
    WIFISCAN_AUTH_WPA_WPA2_PSK = 4,
    WIFISCAN_AUTH_WPA2_ENTERPRISE = 5,
    WIFISCAN_AUTH_WPA3_PSK = 6,
    WIFISCAN_AUTH_WPA2_WPA3_PSK = 7,
};

typedef struct {
    char ssid[33];
    int8_t rssi; /* dBm */
    uint8_t channel;
    uint8_t auth; /* WIFISCAN_AUTH_* or another wifi_auth_mode_t value */
} wifiscan_ap_t;

/* In place: drops hidden (empty) names, keeps the strongest record of each name, sorts strongest
 * first (equal signal: by name) and keeps at most max. Returns the new count. */
size_t wifiscan_tidy(wifiscan_ap_t *aps, size_t n, size_t max);

/* "open", "wep", "wpa_psk", "wpa2_psk", "wpa_wpa2_psk", "wpa2_enterprise", "wpa3_psk",
 * "wpa2_wpa3_psk"; "other" for anything else. */
const char *wifiscan_auth_name(int auth);

typedef enum {
    WIFISCAN_RUN,    /* scan now */
    WIFISCAN_BUSY,   /* one is running: the caller is told it is scanning, nothing queues */
    WIFISCAN_CACHED, /* the last one ended less than WIFISCAN_GAP_MS ago: answer with its results */
} wifiscan_act_t;

/* running: a scan is under way. have_last: an earlier scan has ended, at last_end_ms. */
wifiscan_act_t wifiscan_decide(bool running, bool have_last, int64_t now_ms, int64_t last_end_ms);

/* A scan that started at start_ms has had no SCAN_DONE for WIFISCAN_TIMEOUT_MS. */
bool wifiscan_timed_out(bool running, int64_t now_ms, int64_t start_ms);

/* Age of the last results in ms; -1 when there are none. Never negative otherwise. */
int64_t wifiscan_age_ms(bool have_last, int64_t now_ms, int64_t last_end_ms);

/* A WIFI_EVENT_SCAN_DONE ends our scan only when one of ours is active, esp_wifi_scan_start has
 * returned OK for it (started) and the driver reports success (status 0): the station's own connect
 * scans raise the same event. */
bool wifiscan_done_is_ours(bool scan_active, bool started, uint32_t status);

/* The status of a SCAN_DONE event: the payload's when it has one, else 0 (success; esp_wifi_remote may
 * deliver the event without data, and ours is told apart by the scan being confirmed started). */
uint32_t wifiscan_done_status(bool has_data, uint32_t status);

typedef enum {
    WIFISCAN_CONN_GO,    /* esp_wifi_connect now */
    WIFISCAN_CONN_DEFER, /* a scan is using the radio: connect when it is over */
    WIFISCAN_CONN_SKIP,  /* nothing to connect */
} wifiscan_conn_t;

/* The station's connect (first attempt or the 5 s retry) while a scan of ours may be active. */
wifiscan_conn_t wifiscan_connect_act(bool scan_active, bool sta_wanted, bool sta_connected);

#ifdef __cplusplus
}
#endif
