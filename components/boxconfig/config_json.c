#include "boxconfig/config.h"
#include "boxconfig_private.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define DEFAULT_PORT 16622
#define AP_PASS_LEN 10
#define DEFAULT_VOLUME 50
#define DEFAULT_LED_PIN 23 /* header pin 8, where the case's NeoPixel goes (GPIO26 before 0.2.1); the board has no RGB LED */
#define DEFAULT_LED_BRIGHTNESS 30
#define DEFAULT_DECK_BRIGHTNESS 70 /* a deck Companion dimmed doesn't jump to full when the box takes it */
static const char AP_ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";

void boxcfg_ap_ssid(const uint8_t mac[6], char out[33])
{
    snprintf(out, 33, "CompanionBox-%02X%02X", mac[4], mac[5]);
}

void boxcfg_hostname(const char *box_name, char *out, size_t n)
{
    if (!n) return;
    size_t i = 0;
    for (; box_name && box_name[i] && i + 1 < n; i++) {
        const unsigned char c = (unsigned char)box_name[i];
        out[i] = isalnum(c) ? (char)tolower(c) : '-';
    }
    out[i] = '\0';
}

void boxcfg_defaults(box_config_t *c, const uint8_t mac[6], uint32_t (*rand32)(void))
{
    memset(c, 0, sizeof *c);
    boxcfg_ap_ssid(mac, c->box_name);
    c->companion_auto = true;
    c->companion_port = DEFAULT_PORT;
    c->companion_adopt = true;
    c->eth_dhcp = true;
    c->wifi_enabled = true;
    c->ap_enabled = true;
    c->ap_secured = true; /* the hotspot has the random password below, shown on the decks; it can be opened */
    c->mdns_enabled = true;
    c->surface_mode = BOX_SURFACE_DOCK; /* a fresh or factory-reset box is a Network Dock */
    for (int i = 0; i < AP_PASS_LEN; i++)
        c->ap_pass[i] = AP_ALPHABET[rand32() % (sizeof AP_ALPHABET - 1)];
    c->speaker_enabled = true;
    c->speaker_volume = DEFAULT_VOLUME;
    c->sound_ready = c->sound_connected = c->sound_problem = c->sound_deck_plug = c->sound_key_click = true;
    c->led_enabled = false; /* optional hardware: off until it is fitted and switched on (Speaker & LED page) */
    c->led_flash = true;
    c->led_pin = DEFAULT_LED_PIN;
    c->led_brightness = DEFAULT_LED_BRIGHTNESS;
    c->key_enabled = false; /* optional hardware: an unwired pin with its pull-up reads released, but off is safest */
    c->key_pin = BOXCFG_KEY_PIN;
    c->key_short = BOXCFG_KEY_ACT_MENU;
    c->key_long = BOXCFG_KEY_ACT_FIND;
    c->deck_brightness = DEFAULT_DECK_BRIGHTNESS;
    c->deck_limit = BOXCFG_DECK_LIMIT;
    c->gesture_enabled = true; /* the default gesture */
    c->gesture_keys[0] = BOXCFG_CORNER_TOP_RIGHT;
    c->gesture_keys[1] = BOXCFG_CORNER_BOTTOM_RIGHT;
    c->gesture_taps = 1;
    c->gesture_hold_s = 2;
}

/* err = "<key>: <what>"; boxcfg_err_key reads the key back. */
static void set_err(char *err, size_t n, const char *key, const char *msg)
{
    if (err && n) snprintf(err, n, "%s: %s", key, msg);
}

void boxcfg_err_key(const char *err, char *key, size_t n)
{
    if (!key || !n) return;
    if (!err) err = "";
    const size_t len = strcspn(err, ":");
    snprintf(key, n, "%.*s", (int)len, err);
}

/* Reads a string key into dst (size cap). Returns true when ok (or absent). */
static bool get_str(const cJSON *root, const char *key, char *dst, size_t cap,
                    size_t minlen, size_t maxlen, char *err, size_t errlen)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!it) return true;
    if (!cJSON_IsString(it) || !it->valuestring) {
        set_err(err, errlen, key, "must be a string");
        return false;
    }
    size_t len = strlen(it->valuestring);
    if (len >= cap || len > maxlen || len < minlen) {
        set_err(err, errlen, key, "invalid length");
        return false;
    }
    memcpy(dst, it->valuestring, len + 1);
    return true;
}

static bool get_bool(const cJSON *root, const char *key, bool *dst, char *err, size_t errlen)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!it) return true;
    if (!cJSON_IsBool(it)) {
        set_err(err, errlen, key, "must be a boolean");
        return false;
    }
    *dst = cJSON_IsTrue(it);
    return true;
}

#define SURFACE_SATELLITE "satellite"
#define SURFACE_DOCK "dock"

static bool get_surface_mode(const cJSON *root, box_surface_mode_t *dst, char *err, size_t errlen)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "surfaceMode");
    if (!it) return true;
    if (cJSON_IsString(it) && it->valuestring && strcmp(it->valuestring, SURFACE_SATELLITE) == 0) {
        *dst = BOX_SURFACE_SATELLITE;
    } else if (cJSON_IsString(it) && it->valuestring && strcmp(it->valuestring, SURFACE_DOCK) == 0) {
        *dst = BOX_SURFACE_DOCK;
    } else {
        set_err(err, errlen, "surfaceMode", "must be \"satellite\" or \"dock\"");
        return false;
    }
    return true;
}

static bool valid_port(double v)
{
    return v >= 1 && v <= 65535 && v == (double)(long)v;
}

/* An integer key in lo..hi. Returns true when ok (or absent). */
static bool get_int(const cJSON *root, const char *key, int lo, int hi, int *dst, char *err, size_t errlen)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!it) return true;
    const double v = cJSON_IsNumber(it) ? it->valuedouble : lo - 1.0;
    if (!(v >= lo && v <= hi && v == (double)(long)v)) {
        char msg[40];
        snprintf(msg, sizeof msg, "must be an integer %d-%d", lo, hi);
        set_err(err, errlen, key, msg);
        return false;
    }
    *dst = (int)v;
    return true;
}

/* "ledPin: GPIOn is the service key's pin" (key false) or "keyPin: GPIOn is the status light's pin". */
static void pin_clash(bool key, int pin, char *err, size_t errlen)
{
    char msg[48];
    snprintf(msg, sizeof msg, key ? "GPIO%d is the status light's pin" : "GPIO%d is the service key's pin", pin);
    set_err(err, errlen, key ? "keyPin" : "ledPin", msg);
}

/* The speaker, the status light (components/feedback) and the service key (main/svcbtn.c). claims false: one field
 * of the stored config (boxcfg_from_json_lenient, which checks the light's and the key's pins as a pair itself,
 * once). */
static bool get_feedback(const cJSON *root, box_config_t *t, bool claims, char *err, size_t errlen)
{
    int vol = t->speaker_volume, pin = t->led_pin, bright = t->led_brightness, kpin = t->key_pin;
    bool ok = get_bool(root, "speakerEnabled", &t->speaker_enabled, err, errlen) &&
              get_int(root, "speakerVolume", 0, 100, &vol, err, errlen) &&
              get_bool(root, "soundReady", &t->sound_ready, err, errlen) &&
              get_bool(root, "soundConnected", &t->sound_connected, err, errlen) &&
              get_bool(root, "soundProblem", &t->sound_problem, err, errlen) &&
              get_bool(root, "soundDeckPlug", &t->sound_deck_plug, err, errlen) &&
              get_bool(root, "soundKeyClick", &t->sound_key_click, err, errlen) &&
              get_bool(root, "ledEnabled", &t->led_enabled, err, errlen) &&
              get_bool(root, "ledFlash", &t->led_flash, err, errlen) &&
              get_int(root, "ledPin", 0, 54, &pin, err, errlen) &&
              get_int(root, "ledBrightness", 1, 100, &bright, err, errlen) &&
              get_bool(root, "keyEnabled", &t->key_enabled, err, errlen) &&
              get_int(root, "keyPin", 0, 54, &kpin, err, errlen);
    /* another component's claim on a pin counts for a move onto it: not for a pin the request leaves as it is (the
     * Speaker & LED card sends both), and not at all when loading what is stored */
    const char *why = !ok ? NULL : claims && pin != t->led_pin ? boxcfg_led_pin_problem(pin) : boxcfg_pin_problem_base(pin);
    if (why) {
        set_err(err, errlen, "ledPin", why);
        ok = false;
    }
    why = !ok ? NULL : claims && kpin != t->key_pin ? boxcfg_key_pin_problem(kpin) : boxcfg_pin_problem_base(kpin);
    if (why) {
        set_err(err, errlen, "keyPin", why);
        ok = false;
    }
    const cJSON *ks = cJSON_GetObjectItemCaseSensitive(root, "keyShort");
    uint8_t act;
    if (ok && ks) {
        if (!(cJSON_IsString(ks) && boxcfg_key_act_parse(ks->valuestring, &act)) || act == BOXCFG_KEY_ACT_RESTART) {
            set_err(err, errlen, "keyShort", "must be menu, find, sound, light or none (restart: the long press only)");
            ok = false;
        } else {
            t->key_short = act;
        }
    }
    const cJSON *kl = cJSON_GetObjectItemCaseSensitive(root, "keyLong");
    if (ok && kl) {
        if (cJSON_IsString(kl) && boxcfg_key_act_parse(kl->valuestring, &act)) {
            t->key_long = act;
        } else {
            set_err(err, errlen, "keyLong", "must be menu, find, sound, light, restart or none");
            ok = false;
        }
    }
    /* checked on the result: the light and the key on one pin would fight over it. The key named in the
     * request is the one refused (the light when both or neither are). At load: stored_pin_clash. */
    if (ok && claims && pin == kpin) {
        const bool key = cJSON_GetObjectItemCaseSensitive(root, "keyPin") && !cJSON_GetObjectItemCaseSensitive(root, "ledPin");
        pin_clash(key, pin, err, errlen);
        ok = false;
    }
    t->speaker_volume = (uint8_t)vol;
    t->led_pin = (int8_t)pin;
    t->led_brightness = (uint8_t)bright;
    t->key_pin = (int8_t)kpin;
    return ok;
}

/* The decks' brightness while the box drives them: 10-100 in steps of 10. */
static bool get_deck_brightness(const cJSON *root, box_config_t *t, char *err, size_t errlen)
{
    int v = t->deck_brightness;
    if (!get_int(root, "deckBrightness", 10, 100, &v, err, errlen)) return false;
    if (v % 10) {
        set_err(err, errlen, "deckBrightness", "must be 10-100 in steps of 10");
        return false;
    }
    t->deck_brightness = (uint8_t)v;
    return true;
}

/* the Stream Decks at once: 2 or 4, nothing else. */
static bool get_deck_limit(const cJSON *root, box_config_t *t, char *err, size_t errlen)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "deckLimit");
    if (!v) return true;
    if (!cJSON_IsNumber(v) || (v->valuedouble != BOXCFG_DECK_LIMIT && v->valuedouble != BOXCFG_DECK_LIMIT_MAX)) {
        set_err(err, errlen, "deckLimit", "must be 2 or 4");
        return false;
    }
    t->deck_limit = (uint8_t)v->valuedouble;
    return true;
}

/* gestureKeys' corner names, BOXCFG_CORNER_* order. */
static const char *const CORNERS[] = {"topLeft", "topRight", "bottomLeft", "bottomRight"};

const char *boxcfg_corner_name(int k)
{
    return CORNERS[k >= 0 && k < (int)(sizeof CORNERS / sizeof CORNERS[0]) ? k : BOXCFG_CORNER_TOP_RIGHT];
}

static int corner_of(const cJSON *it)
{
    for (int i = 0; cJSON_IsString(it) && it->valuestring && i < (int)(sizeof CORNERS / sizeof CORNERS[0]); i++)
        if (!strcmp(it->valuestring, CORNERS[i])) return i;
    return -1;
}

/* The box menu gesture: gestureKeys two different corners, gestureTaps 1-3, gestureHoldSeconds 2, 3 or 5. */
static bool get_gesture(const cJSON *root, box_config_t *t, char *err, size_t errlen)
{
    if (!get_bool(root, "gestureEnabled", &t->gesture_enabled, err, errlen)) return false;
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "gestureKeys");
    if (k) {
        const int a = cJSON_GetArraySize(k) == 2 ? corner_of(cJSON_GetArrayItem(k, 0)) : -1;
        const int b = cJSON_GetArraySize(k) == 2 ? corner_of(cJSON_GetArrayItem(k, 1)) : -1;
        if (!cJSON_IsArray(k) || a < 0 || b < 0 || a == b) {
            set_err(err, errlen, "gestureKeys", "must be two different corners: topLeft, topRight, bottomLeft, bottomRight");
            return false;
        }
        t->gesture_keys[0] = (uint8_t)a;
        t->gesture_keys[1] = (uint8_t)b;
    }
    int taps = t->gesture_taps, hold = t->gesture_hold_s;
    if (!get_int(root, "gestureTaps", 1, 3, &taps, err, errlen)) return false;
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "gestureHoldSeconds");
    if (h && (!get_int(root, "gestureHoldSeconds", 2, 5, &hold, err, errlen) || hold == 4)) {
        set_err(err, errlen, "gestureHoldSeconds", "must be 2, 3 or 5");
        return false;
    }
    t->gesture_taps = (uint8_t)taps;
    t->gesture_hold_s = (uint8_t)hold;
    return true;
}

static bool valid_quad(const char *s)
{
    int parts = 0;
    while (1) {
        if (*s < '0' || *s > '9') return false;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (++digits > 3 || v > 255) return false;
            s++;
        }
        parts++;
        if (*s == '\0') break;
        if (*s != '.') return false;
        s++;
    }
    return parts == 4;
}

static bool validate_static_ip(const box_config_t *c, char *err, size_t errlen)
{
    if (c->eth_dhcp) return true;
    if (!valid_quad(c->eth_ip)) { set_err(err, errlen, "ethIp", "not a valid IPv4 address"); return false; }
    if (!valid_quad(c->eth_mask)) { set_err(err, errlen, "ethMask", "not a valid IPv4 address"); return false; }
    if (!valid_quad(c->eth_gw)) { set_err(err, errlen, "ethGw", "not a valid IPv4 address"); return false; }
    if (c->eth_dns[0] && !valid_quad(c->eth_dns)) { set_err(err, errlen, "ethDns", "not a valid IPv4 address"); return false; }
    return true;
}

/* The keys boxcfg_from_json reads (boxcfg_to_json writes them all). */
static const char *const KEYS[] = {"boxName", "companionAuto", "companionHost", "companionPort", "companionPinId", "companionAdopt",
                                   "ethDhcp", "ethIp", "ethMask", "ethGw", "ethDns", "wifiSsid", "wifiPass",
                                   "wifiBackupSsid", "wifiBackupPass", "wifiEnabled",
                                   "apEnabled", "apSecured", "apPass", "adminPass", "mdnsEnabled", "surfaceMode",
                                   "speakerEnabled", "speakerVolume", "soundReady", "soundConnected", "soundProblem",
                                   "soundDeckPlug", "soundKeyClick", "ledEnabled", "ledFlash", "ledPin", "ledBrightness",
                                   "keyEnabled", "keyPin", "keyShort", "keyLong", "deckBrightness", "deckLimit", "gestureEnabled", "gestureKeys", "gestureTaps",
                                   "gestureHoldSeconds"};

static bool known_key(const char *k)
{
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (!strcmp(KEYS[i], k)) return true;
    return false;
}

/* boxcfg_from_json on a parsed object. */
static bool from_root(box_config_t *c, const cJSON *root, bool claims, char *err, size_t errlen)
{
    box_config_t t = *c;
    bool ok =
        get_str(root, "boxName", t.box_name, sizeof t.box_name, 1, 32, err, errlen) &&
        get_bool(root, "companionAuto", &t.companion_auto, err, errlen) &&
        get_str(root, "companionHost", t.companion_host, sizeof t.companion_host, 0, 63, err, errlen) &&
        get_str(root, "companionPinId", t.companion_pin_id, sizeof t.companion_pin_id, 0, 63, err, errlen) &&
        get_bool(root, "companionAdopt", &t.companion_adopt, err, errlen) &&
        get_bool(root, "ethDhcp", &t.eth_dhcp, err, errlen) &&
        get_str(root, "ethIp", t.eth_ip, sizeof t.eth_ip, 0, 15, err, errlen) &&
        get_str(root, "ethMask", t.eth_mask, sizeof t.eth_mask, 0, 15, err, errlen) &&
        get_str(root, "ethGw", t.eth_gw, sizeof t.eth_gw, 0, 15, err, errlen) &&
        get_str(root, "ethDns", t.eth_dns, sizeof t.eth_dns, 0, 15, err, errlen) &&
        get_str(root, "wifiSsid", t.wifi_ssid, sizeof t.wifi_ssid, 0, 32, err, errlen) &&
        get_str(root, "wifiBackupSsid", t.wifi_backup_ssid, sizeof t.wifi_backup_ssid, 0, 32, err, errlen) &&
        get_bool(root, "wifiEnabled", &t.wifi_enabled, err, errlen) &&
        get_bool(root, "apEnabled", &t.ap_enabled, err, errlen) &&
        get_bool(root, "apSecured", &t.ap_secured, err, errlen) &&
        get_bool(root, "mdnsEnabled", &t.mdns_enabled, err, errlen);
    if (ok) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "companionPort");
        if (p) {
            if (!cJSON_IsNumber(p) || !valid_port(p->valuedouble)) {
                set_err(err, errlen, "companionPort", "must be an integer 1-65535");
                ok = false;
            } else {
                t.companion_port = (uint16_t)p->valuedouble;
            }
        }
    }
    if (ok) {
        /* wifiPass: "" or 8-63; apPass: 0-63, and 8-63 while apSecured; adminPass: "" or 1-64. */
        ok = get_str(root, "wifiPass", t.wifi_pass, sizeof t.wifi_pass, 0, 63, err, errlen) &&
             get_str(root, "wifiBackupPass", t.wifi_backup_pass, sizeof t.wifi_backup_pass, 0, 63, err, errlen) &&
             get_str(root, "apPass", t.ap_pass, sizeof t.ap_pass, 0, 63, err, errlen) &&
             get_str(root, "adminPass", t.admin_pass, sizeof t.admin_pass, 0, 64, err, errlen);
        if (ok && t.wifi_pass[0] && strlen(t.wifi_pass) < 8) {
            set_err(err, errlen, "wifiPass", "must be empty or 8-63 characters");
            ok = false;
        }
        if (ok && t.wifi_backup_pass[0] && strlen(t.wifi_backup_pass) < 8) {
            set_err(err, errlen, "wifiBackupPass", "must be empty or 8-63 characters");
            ok = false;
        }
        /* checked on the result: switching the password on needs a usable one, sent or stored */
        if (ok && t.ap_secured && strlen(t.ap_pass) < 8) {
            set_err(err, errlen, "apPass", "must be 8-63 characters");
            ok = false;
        }
    }
    if (ok) ok = get_surface_mode(root, &t.surface_mode, err, errlen);
    if (ok) ok = get_feedback(root, &t, claims, err, errlen);
    if (ok) ok = get_deck_brightness(root, &t, err, errlen);
    if (ok) ok = get_deck_limit(root, &t, err, errlen);
    if (ok) ok = get_gesture(root, &t, err, errlen);
    if (ok) ok = validate_static_ip(&t, err, errlen);
    if (ok) *c = t;
    return ok;
}

bool boxcfg_from_cjson(box_config_t *c, const cJSON *root, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!cJSON_IsObject(root)) {
        set_err(err, errlen, "json", "expected an object");
        return false;
    }
    return from_root(c, root, true, err, errlen);
}

bool boxcfg_from_json(box_config_t *c, const char *json, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    cJSON *root = cJSON_Parse(json ? json : "");
    if (!root) { set_err(err, errlen, "json", "parse error"); return false; }
    const bool ok = boxcfg_from_cjson(c, root, err, errlen);
    cJSON_Delete(root);
    return ok;
}

/* Appends "key: why" (or "key: why") to the skipped list. */
static void note_skipped(char *out, size_t n, const char *key, const char *why)
{
    if (!out || !n) return;
    const size_t len = strlen(out);
    if (len + 1 >= n) return;
    char k[24];
    boxcfg_err_key(why, k, sizeof k);
    if (!strcmp(k, key)) snprintf(out + len, n - len, "%s%s", len ? "; " : "", why);
    else snprintf(out + len, n - len, "%s%s: %s", len ? "; " : "", key, why);
}

char *boxcfg_to_json(const box_config_t *c, bool include_secrets)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "boxName", c->box_name);
    cJSON_AddBoolToObject(o, "companionAuto", c->companion_auto);
    cJSON_AddStringToObject(o, "companionHost", c->companion_host);
    cJSON_AddNumberToObject(o, "companionPort", c->companion_port);
    cJSON_AddStringToObject(o, "companionPinId", c->companion_pin_id);
    cJSON_AddBoolToObject(o, "companionAdopt", c->companion_adopt);
    cJSON_AddBoolToObject(o, "ethDhcp", c->eth_dhcp);
    cJSON_AddStringToObject(o, "ethIp", c->eth_ip);
    cJSON_AddStringToObject(o, "ethMask", c->eth_mask);
    cJSON_AddStringToObject(o, "ethGw", c->eth_gw);
    cJSON_AddStringToObject(o, "ethDns", c->eth_dns);
    cJSON_AddStringToObject(o, "wifiSsid", c->wifi_ssid);
    cJSON_AddStringToObject(o, "wifiPass", include_secrets ? c->wifi_pass : "");
    cJSON_AddStringToObject(o, "wifiBackupSsid", c->wifi_backup_ssid);
    cJSON_AddStringToObject(o, "wifiBackupPass", include_secrets ? c->wifi_backup_pass : "");
    cJSON_AddBoolToObject(o, "wifiEnabled", c->wifi_enabled);
    cJSON_AddBoolToObject(o, "apEnabled", c->ap_enabled);
    cJSON_AddBoolToObject(o, "apSecured", c->ap_secured);
    cJSON_AddStringToObject(o, "apPass", c->ap_pass);
    cJSON_AddStringToObject(o, "adminPass", include_secrets ? c->admin_pass : "");
    cJSON_AddBoolToObject(o, "mdnsEnabled", c->mdns_enabled);
    cJSON_AddStringToObject(o, "surfaceMode", c->surface_mode == BOX_SURFACE_DOCK ? SURFACE_DOCK : SURFACE_SATELLITE);
    cJSON_AddBoolToObject(o, "speakerEnabled", c->speaker_enabled);
    cJSON_AddNumberToObject(o, "speakerVolume", c->speaker_volume);
    cJSON_AddBoolToObject(o, "soundReady", c->sound_ready);
    cJSON_AddBoolToObject(o, "soundConnected", c->sound_connected);
    cJSON_AddBoolToObject(o, "soundProblem", c->sound_problem);
    cJSON_AddBoolToObject(o, "soundDeckPlug", c->sound_deck_plug);
    cJSON_AddBoolToObject(o, "soundKeyClick", c->sound_key_click);
    cJSON_AddBoolToObject(o, "ledEnabled", c->led_enabled);
    cJSON_AddBoolToObject(o, "ledFlash", c->led_flash);
    cJSON_AddNumberToObject(o, "ledPin", c->led_pin);
    cJSON_AddNumberToObject(o, "keyPin", c->key_pin); /* the two pins side by side */
    cJSON_AddNumberToObject(o, "ledBrightness", c->led_brightness);
    cJSON_AddBoolToObject(o, "keyEnabled", c->key_enabled);
    const char *ks = boxcfg_key_act_name(c->key_short), *kl = boxcfg_key_act_name(c->key_long);
    cJSON_AddStringToObject(o, "keyShort", ks ? ks : "menu");
    cJSON_AddStringToObject(o, "keyLong", kl ? kl : "find");
    cJSON_AddNumberToObject(o, "deckBrightness", c->deck_brightness);
    cJSON_AddNumberToObject(o, "deckLimit", c->deck_limit);
    cJSON_AddBoolToObject(o, "gestureEnabled", c->gesture_enabled);
    cJSON *keys = cJSON_AddArrayToObject(o, "gestureKeys");
    for (int i = 0; keys && i < 2; i++)
        cJSON_AddItemToArray(keys, cJSON_CreateString(boxcfg_corner_name(c->gesture_keys[i])));
    cJSON_AddNumberToObject(o, "gestureTaps", c->gesture_taps);
    cJSON_AddNumberToObject(o, "gestureHoldSeconds", c->gesture_hold_s);
    /* an add that found no memory left its key out: stored like that, the next start would fill the key with its
     * default (the Wi-Fi password, a static address gone). All of KEYS, or nothing. */
    if (cJSON_GetArraySize(o) != (int)(sizeof KEYS / sizeof KEYS[0]) || cJSON_GetArraySize(keys) != 2) {
        cJSON_Delete(o);
        return NULL;
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

bool boxcfg_apply_satellite_config(box_config_t *c, const char *json, char *err, size_t n)
{
    if (err && n) err[0] = '\0';
    cJSON *root = cJSON_Parse(json ? json : "");
    if (!root) { set_err(err, n, "json", "parse error"); return false; }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_err(err, n, "json", "expected an object");
        return false;
    }
    box_config_t t = *c;
    bool ok = true;

    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(root, "protocol");
    if (proto && (!cJSON_IsString(proto) || strcmp(proto->valuestring, "tcp") != 0)) {
        set_err(err, n, "protocol", "must be \"tcp\"");
        ok = false;
    }
    if (ok) {
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "host");
        if (h) {
            ok = get_str(root, "host", t.companion_host, sizeof t.companion_host, 0, 63, err, n);
            if (ok) {
                t.companion_auto = false;
                t.companion_pin_id[0] = '\0';
            }
        }
    }
    if (ok) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "port");
        if (p) {
            double v = -1;
            if (cJSON_IsNumber(p)) {
                v = p->valuedouble;
            } else if (cJSON_IsString(p) && p->valuestring[0]) {
                char *end = NULL;
                v = strtod(p->valuestring, &end);
                if (*end) v = -1;
            }
            if (!valid_port(v)) {
                set_err(err, n, "port", "must be an integer 1-65535");
                ok = false;
            } else {
                t.companion_port = (uint16_t)v;
            }
        }
    }
    if (ok) ok = get_str(root, "installationName", t.box_name, sizeof t.box_name, 1, 32, err, n);
    if (ok) ok = get_bool(root, "mdnsEnabled", &t.mdns_enabled, err, n);
    cJSON_Delete(root);
    if (ok) *c = t;
    return ok;
}

bool boxcfg_same_target(const box_config_t *a, const box_config_t *b)
{
    return a->companion_auto == b->companion_auto && !strcmp(a->companion_pin_id, b->companion_pin_id) &&
           !strcmp(a->companion_host, b->companion_host) && a->companion_port == b->companion_port;
}

char *boxcfg_satellite_config_json(const box_config_t *c)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "protocol", "tcp");
    cJSON_AddStringToObject(o, "host", c->companion_host);
    cJSON_AddNumberToObject(o, "port", c->companion_port);
    cJSON_AddStringToObject(o, "wsAddress", ""); /* required by ConfigData; the box speaks TCP only */
    cJSON_AddStringToObject(o, "installationName", c->box_name);
    cJSON_AddBoolToObject(o, "mdnsEnabled", c->mdns_enabled);
    cJSON_AddBoolToObject(o, "httpEnabled", true);
    cJSON_AddNumberToObject(o, "httpPort", 80);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

/* A stored ledPin or keyPin that is usable on its own: an integer GPIO that may carry the light or the key. It must
 * take exactly what get_feedback takes for the pin at load (claims false). */
static bool stored_pin(const cJSON *root, const char *key, int *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(it)) return false;
    const double v = it->valuedouble;
    if (!(v >= 0 && v <= 54 && v == (double)(long)v) || boxcfg_pin_problem_base((int)v)) return false;
    *out = (int)v;
    return true;
}

/* The light's and the key's pins as the stored config has them, checked once as a pair: which of the two (if
 * any) is skipped because they are the same pin. Each is its stored one when that is usable, else the one *c has;
 * with no keyPin stored, the key's is the pin boxcfg_stored_legacy then gives it (never a default the config didn't
 * choose: the key's moved to GPIO21 in 0.2.9, a free pin for the light before). The same pin: the light's is
 * skipped (as an API write refuses it), unless the pin it keeps is that one too; then the key's. */
static void stored_pin_clash(const cJSON *root, const box_config_t *c, bool *led_out, bool *key_out)
{
    int led = c->led_pin, key = c->key_pin;
    const bool led_stored = stored_pin(root, "ledPin", &led), key_stored = stored_pin(root, "keyPin", &key);
    if (!cJSON_GetObjectItemCaseSensitive(root, "keyPin") && led != BOXCFG_KEY_PIN_LEGACY) key = BOXCFG_KEY_PIN_LEGACY;
    *led_out = *key_out = false;
    if (led != key) return;
    if (led_stored && (c->led_pin != key || !key_stored)) *led_out = true;
    else if (key_stored) *key_out = true;
}

int boxcfg_from_json_lenient(box_config_t *c, const char *json, char *skipped, size_t n)
{
    if (skipped && n) skipped[0] = '\0';
    cJSON *root = cJSON_Parse(json ? json : "");
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    int bad = 0;
    const int8_t led_was = c->led_pin, key_was = c->key_pin;
    bool led_clash, key_clash;
    stored_pin_clash(root, c, &led_clash, &key_clash);
    /* apSecured first: apPass is checked against it (8-63 only while secured), and the default is secured
     *, so a stored open hotspot's short or empty password must find it off already. ethDhcp last:
     * false is checked against the addresses, which must be in place by then. */
    for (int pass = 0; pass < 3; pass++) {
        const cJSON *it;
        cJSON_ArrayForEach(it, root) {
            const int when = !strcmp(it->string, "apSecured") ? 0 : !strcmp(it->string, "ethDhcp") ? 2 : 1;
            if (when != pass) continue;
            if (!known_key(it->string)) {
                note_skipped(skipped, n, it->string, "unknown key");
                bad++;
                continue;
            }
            /* each field on its own; the light's and the key's pins also as the pair stored_pin_clash checked */
            const bool led = !strcmp(it->string, "ledPin"), key = !strcmp(it->string, "keyPin");
            box_config_t t = *c;
            cJSON *one = cJSON_CreateObject();
            cJSON *dup = cJSON_Duplicate(it, true);
            char err[96] = "";
            bool ok = false;
            if (one && dup && cJSON_AddItemToObject(one, it->string, dup)) {
                dup = NULL; /* owned by one */
                ok = from_root(&t, one, false, err, sizeof err);
            } else {
                snprintf(err, sizeof err, "%s: no memory", it->string);
            }
            cJSON_Delete(dup);
            cJSON_Delete(one);
            if (ok && ((led && led_clash) || (key && key_clash))) {
                pin_clash(key, led ? t.led_pin : t.key_pin, err, sizeof err);
                ok = false;
            }
            if (ok) *c = t;
            if (!ok) {
                note_skipped(skipped, n, it->string, err);
                bad++;
            }
        }
    }
    /* a pin stored twice (stored_pin_clash sees the first) may still put the two on one pin: as there, the light
     * keeps the pin it had, or the key when that is the same pin. With no keyPin, boxcfg_stored_legacy moves the
     * key off the light's pin. */
    if (cJSON_GetObjectItemCaseSensitive(root, "keyPin") && c->led_pin == c->key_pin) {
        const bool key = led_was == c->key_pin;
        char err[64];
        pin_clash(key, c->led_pin, err, sizeof err);
        note_skipped(skipped, n, key ? "keyPin" : "ledPin", err);
        if (key) c->key_pin = key_was;
        else c->led_pin = led_was;
        bad++;
    }
    cJSON_Delete(root);
    return bad;
}

bool boxcfg_stored_legacy(box_config_t *c, const char *json)
{
    cJSON *root = cJSON_Parse(json ? json : "");
    bool changed = false;
    if (cJSON_IsObject(root) && !cJSON_GetObjectItemCaseSensitive(root, "keyEnabled") && !c->key_enabled) {
        c->key_enabled = true;
        changed = true;
    }
    /* no keyPin either: the key was wired to its old pin then, unless the light sits there */
    if (cJSON_IsObject(root) && !cJSON_GetObjectItemCaseSensitive(root, "keyPin") && c->led_pin != BOXCFG_KEY_PIN_LEGACY &&
        c->key_pin != BOXCFG_KEY_PIN_LEGACY) {
        c->key_pin = BOXCFG_KEY_PIN_LEGACY;
        changed = true;
    }
    cJSON_Delete(root);
    return changed;
}

bool boxcfg_should_store(boxcfg_load_t how, bool same)
{
    switch (how) {
    case BOXCFG_LOAD_ABSENT: return true;
    case BOXCFG_LOAD_CLEAN: return !same;
    default: return false; /* PARTIAL, FAILED: the stored config stays as it is */
    }
}
