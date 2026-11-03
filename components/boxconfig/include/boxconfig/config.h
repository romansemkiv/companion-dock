#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How Companion drives the decks . A change takes effect at the next start. */
typedef enum {
    BOX_SURFACE_SATELLITE = 0, /* JSON "satellite": the box is a Companion Satellite */
    BOX_SURFACE_DOCK = 1,      /* JSON "dock": the box acts as Network Docks, one per deck (default) */
} box_surface_mode_t;

typedef struct {
    char box_name[33];
    bool companion_auto;
    char companion_host[64];
    uint16_t companion_port;
    char companion_pin_id[64];
    /* "companionAdopt" (default true; a stored config without it too): with a settings PIN or password set,
     * Companion's Surfaces -> Discover may still point the box at itself: the Satellite REST API is served without
     * the PIN, and an unauthenticated POST applies only the Companion target (host, port). Off: the PIN for that too. */
    bool companion_adopt;
    bool eth_dhcp;
    char eth_ip[16], eth_mask[16], eth_gw[16], eth_dns[16];
    char wifi_ssid[33], wifi_pass[65];
    /* The backup Wi-Fi network: "wifiBackupSsid" / "wifiBackupPass"; empty = none. The box joins it
     * when the main one is out of reach (components/netmgr/wififo_logic.h). */
    char wifi_backup_ssid[33], wifi_backup_pass[65];
    /* "wifiEnabled" (default true; a stored config without it too): the Wi-Fi station (the main and
     * backup networks) runs. Off: no connect, retry or failover; the setup hotspot keeps its own settings. */
    bool wifi_enabled;
    bool ap_enabled;
    /* "apSecured": true (the default; a stored config without the key too), WPA2 with ap_pass
     * (8-63 chars); false, the hotspot is open and ap_pass is kept as it is, unused, for when the
     * password is switched on again. */
    bool ap_secured;
    char ap_pass[65];
    char admin_pass[65];
    bool mdns_enabled;
    box_surface_mode_t surface_mode; /* JSON key "surfaceMode" */
    /* The board's speaker and the status light (components/feedback). JSON keys in quotes. Defaults:
     * the speaker on at volume 50 with every sound on, the light on GPIO23 at
     * brightness 30. */
    bool speaker_enabled;   /* "speakerEnabled" */
    uint8_t speaker_volume; /* "speakerVolume": 0-100 */
    bool sound_ready;       /* "soundReady": the boot finished */
    bool sound_connected;   /* "soundConnected": Companion connected / drives a deck */
    bool sound_problem;     /* "soundProblem": Companion lost, a deck in trouble, the network lost */
    bool sound_deck_plug;   /* "soundDeckPlug": a deck plugged in or removed */
    bool sound_key_click;   /* "soundKeyClick": every key-down */
    bool led_enabled;       /* "ledEnabled": the light shows the status */
    bool led_flash;         /* "ledFlash": and flashes on each event (key clicks too), needs ledEnabled */
    int8_t led_pin;         /* "ledPin": its data GPIO, one boxcfg_led_pin_problem accepts */
    uint8_t led_brightness; /* "ledBrightness": 1-100 */
    /* "deckBrightness": 10-100 in steps of 10 (default 70): the decks' brightness while the box drives
     * them (its maintenance screens); Companion's own wins while it drives a deck. */
    uint8_t deck_brightness;
    /* "deckLimit": the Stream Decks the box takes at once, 2 (the default; a stored config without the key
     * too: the box's Power over Ethernet is sized for 2) or 4 (unlocked on the web page at the user's risk). No
     * other value. Read at start: a change takes effect at the next one (Device.SetConfig restarts the box). */
    uint8_t deck_limit;
    /* The box menu gesture made on a deck Companion drives (the default one, top-right then bottom-right, also for a
     * stored config without the keys): "gestureEnabled" (true) turns it on; "gestureKeys", two different corners, ["topRight","bottomRight"],
     * the first tapped, then both held (gesture_keys[]: BOXCFG_CORNER_*); "gestureTaps" (1-3, 1) the taps of the first;
     * "gestureHoldSeconds" (2, 3 or 5; 2) the hold. Applied at once (surfaces, deckproto/corner.h). */
    bool gesture_enabled;
    uint8_t gesture_keys[2];
    uint8_t gesture_taps;
    uint8_t gesture_hold_s;
} box_config_t;

#define BOXCFG_DECK_LIMIT 2        /* deckLimit's default */
#define BOXCFG_DECK_LIMIT_MAX 4    /* ... unlocked */

/* gestureKeys' corners, "topLeft", "topRight", "bottomLeft", "bottomRight" (deckproto/corner.h's order). */
enum { BOXCFG_CORNER_TOP_LEFT, BOXCFG_CORNER_TOP_RIGHT, BOXCFG_CORNER_BOTTOM_LEFT, BOXCFG_CORNER_BOTTOM_RIGHT };
/* Corner k's name in the JSON ("topLeft" ...); "topRight" for anything else. */
const char *boxcfg_corner_name(int k);

/* box_name = "CompanionBox-XXXX" (last two MAC bytes), auto-discovery on,
 * port 16622, DHCP on, AP on with a random 10-char WPA2 password (it can be switched off), mDNS on,
 * admin password empty (off), Network Dock mode (the value is stored, so a box keeps its mode
 * across updates, and only a fresh or factory-reset box starts as a Network Dock). */
void boxcfg_defaults(box_config_t *c, const uint8_t mac[6], uint32_t (*rand32)(void));

/* Hotspot SSID "CompanionBox-XXXX". */
void boxcfg_ap_ssid(const uint8_t mac[6], char out[33]);

/* The box's host name from its box name (DHCP option 12, mDNS, the web page's Host check: one rule): lower
 * case letters and digits, anything else '-', at most n - 1 characters. */
void boxcfg_hostname(const char *box_name, char *out, size_t n);

/* Partial update from a JSON object (camelCase field names). Unknown keys are
 * ignored; a known key with the wrong type or an invalid value fails. On
 * failure *c is unchanged and err names the offending key. apPass takes 0-63
 * chars, and must be 8-63 when the result has apSecured true (else it is kept,
 * unused; an unusable one is refused as "apPass" when the password is switched on). */
bool boxcfg_from_json(box_config_t *c, const char *json, char *err, size_t errlen);

/* boxcfg_from_json on a parsed JSON value (an object, else refused as "json"): for a caller that has the cJSON
 * already, with no print and parse again. */
struct cJSON;
bool boxcfg_from_cjson(box_config_t *c, const struct cJSON *root, char *err, size_t errlen);

/* Loading the stored config: field by field. Every known key that is valid is laid over *c;
 * an invalid or unknown one is skipped and named in `skipped` ("key: why; ...", never a value; may be
 * NULL). The static address counts as a whole: ethDhcp false is taken only with valid addresses.
 * Returns how many keys were skipped, or -1 (c unchanged) when json is not a JSON object. API writes
 * use boxcfg_from_json (all or nothing). */
int boxcfg_from_json_lenient(box_config_t *c, const char *json, char *skipped, size_t n);

/* How loading the stored config went. */
typedef enum {
    BOXCFG_LOAD_ABSENT,  /* nothing stored */
    BOXCFG_LOAD_CLEAN,   /* read and every field loaded */
    BOXCFG_LOAD_PARTIAL, /* read, some fields skipped (boxcfg_from_json_lenient > 0) */
    BOXCFG_LOAD_FAILED,  /* stored but unreadable, or not a JSON object */
} boxcfg_load_t;

/* Save the loaded config back at start? Only when nothing was stored (the random hotspot password
 * must stay), or a cleanly loaded one differs from what this firmware writes (same: it doesn't).
 * Never over a stored config that couldn't be read, parsed or loaded whole: that stays in NVS as it
 * is (an older image after a rollback or downgrade meets a newer firmware's config). */
bool boxcfg_should_store(boxcfg_load_t how, bool same);

/* Caller frees with free() (cJSON_free). wifiPass/adminPass are "" when
 * include_secrets is false. apPass is always included. */
char *boxcfg_to_json(const box_config_t *c, bool include_secrets);

/* The key a refusal from boxcfg_from_json / boxcfg_apply_satellite_config names ("companionPort",
 * "host", ...; "json" for unparsable input), from the err text they wrote. "" when err is empty. */
void boxcfg_err_key(const char *err, char *key, size_t n);

/* companion-satellite compatible /api/config. A posted host is a manual address here (companion_auto
 * off, pin cleared); webui_apply_satellite then pins it when a visible Companion instance has that
 * address (discovery_pin_posted). */
bool boxcfg_apply_satellite_config(box_config_t *c, const char *json, char *err, size_t n);
/* a and b point the box at the same Companion target, the whole of it: companionAuto, companionPinId, companionHost and
 * companionPort (an adopt that changes none of them writes nothing). */
bool boxcfg_same_target(const box_config_t *a, const box_config_t *b);
char *boxcfg_satellite_config_json(const box_config_t *c);

/* The status light's data pin (led_pins.c): a free header pin of the Waveshare
 * ESP32-P4-WIFI6-POE-ETH that this firmware doesn't use itself (not GPIO7/8,
 * the speaker codec's I2C bus, nor GPIO32, the service button). NULL when gpio
 * may carry it, else why not. */
const char *boxcfg_led_pin_problem(int gpio);
/* Those pins, ascending: writes at most max of them, returns how many. */
int boxcfg_led_pins(int *out, int max);

#ifdef ESP_PLATFORM
#include "esp_err.h"
/* Loads from NVS (namespace "boxcfg", key "cfg") over the defaults, field by field
 * (boxcfg_from_json_lenient): the stored JSON may be partial; an invalid or unknown field is
 * skipped and logged; unreadable or not JSON: defaults. Then saves the result once when nothing was stored, or when a cleanly loaded config
 * is not exactly boxcfg_to_json(c, true) (partial, e.g. written by tools/seed_config.py, or from a
 * firmware with fewer fields): the hotspot password the defaults draw at random then stays the
 * same across starts. Never over a stored config that couldn't be read, parsed or loaded whole
 * (boxcfg_should_store): that one is kept in NVS as it is (boxcfg_stored_kept) and the box runs on
 * what it could load. *saved (may be NULL) says whether it saved. A failed save is returned; *c is
 * loaded either way. */
esp_err_t boxcfg_load_and_store(box_config_t *c, bool *saved);
/* Writes c as the stored config (a deliberate save: it replaces a kept one). */
esp_err_t boxcfg_save(const box_config_t *c);
/* The stored config couldn't be read or loaded whole at start and is kept in NVS as it was.
 * Automatic saves (the supervisor's Companion pin) wait while this is true; a deliberate save
 * (settings changed on the page or the API) clears it. */
bool boxcfg_stored_kept(void);
/* Erases the "boxcfg" namespace. */
esp_err_t boxcfg_factory_reset(void);
#endif

#ifdef __cplusplus
}
#endif
