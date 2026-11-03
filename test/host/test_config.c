#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "boxconfig/config.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t MAC[6] = {0x02, 0x00, 0x00, 0x00, 0xA1, 0xB2};
static uint32_t counter;
static uint32_t fake_rand(void) { return counter++ * 7u + 3u; }

static box_config_t fresh(void)
{
    box_config_t c;
    boxcfg_defaults(&c, MAC, fake_rand);
    return c;
}

static void test_defaults(void)
{
    box_config_t c = fresh();
    TEST_ASSERT_EQUAL_STRING("CompanionBox-A1B2", c.box_name);
    TEST_ASSERT_EQUAL_UINT16(16622, c.companion_port);
    TEST_ASSERT_TRUE(c.companion_auto);
    TEST_ASSERT_TRUE(c.eth_dhcp);
    TEST_ASSERT_TRUE(c.ap_enabled);
    TEST_ASSERT_TRUE(c.ap_secured); /* the hotspot has a password unless it is switched off */
    TEST_ASSERT_TRUE(c.mdns_enabled);
    TEST_ASSERT_EQUAL_STRING("", c.admin_pass);
    TEST_ASSERT_EQUAL_INT(BOX_SURFACE_DOCK, c.surface_mode); /* a fresh or reset box is a Network Dock */
    TEST_ASSERT_EQUAL_UINT(10, strlen(c.ap_pass));
    for (size_t i = 0; i < 10; i++)
        TEST_ASSERT_NOT_NULL(strchr("abcdefghjkmnpqrstuvwxyz23456789", c.ap_pass[i]));
    char ssid[33];
    boxcfg_ap_ssid(MAC, ssid);
    TEST_ASSERT_EQUAL_STRING("CompanionBox-A1B2", ssid);
}

static void test_json_partial_update(void)
{
    box_config_t c = fresh(), before = c;
    char err[96];
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"companionPort\":17000,\"unknown\":1}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT16(17000, c.companion_port);
    before.companion_port = 17000;
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
}

static void test_json_invalid_leaves_unchanged(void)
{
    box_config_t c = fresh(), before = c;
    char err[96];
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"boxName\":\"x\",\"companionPort\":70000}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "companionPort"));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
}

static void test_json_wrong_type_and_syntax(void)
{
    box_config_t c = fresh(), before = c;
    char err[96];
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"apEnabled\":\"yes\"}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "apEnabled"));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"boxName\":5}", err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "not json", err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "[1]", err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
}

static void test_json_string_limits(void)
{
    box_config_t c = fresh(), before = c;
    char err[96];
    char big[128];
    memset(big, 'a', 33); big[33] = 0;
    char js[256];
    snprintf(js, sizeof js, "{\"boxName\":\"%s\"}", big);
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, js, err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"boxName\":\"\"}", err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"apSecured\":true,\"apPass\":\"short\"}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "apPass"));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"wifiPass\":\"1234567\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"wifiPass\":\"\",\"adminPass\":\"x\",\"companionPinId\":\"\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("x", c.admin_pass);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"wifiPass\":\"12345678\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("12345678", c.wifi_pass);
}

static void test_json_static_ip_validation(void)
{
    box_config_t c = fresh(), before = c;
    char err[96];
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"ethDhcp\":false,\"ethIp\":\"10.0.0.300\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
    const char *ok = "{\"ethDhcp\":false,\"ethIp\":\"10.0.0.5\",\"ethMask\":\"255.255.255.0\",\"ethGw\":\"10.0.0.1\",\"ethDns\":\"\"}";
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, ok, err, sizeof err));
    TEST_ASSERT_FALSE(c.eth_dhcp);
    /* validation applies to the resulting config */
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"ethGw\":\"1.2.3\"}", err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"ethDns\":\"1.2.3.4.5\"}", err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"ethIp\":\"1.2.3.x\"}", err, sizeof err));
    /* DHCP on: static fields not validated */
    box_config_t d = fresh();
    TEST_ASSERT_TRUE(boxcfg_from_json(&d, "{\"ethIp\":\"junk\"}", err, sizeof err));
}

static void test_to_json_masks_secrets(void)
{
    box_config_t c = fresh();
    strcpy(c.wifi_pass, "wifisecret");
    strcpy(c.admin_pass, "adm");
    char *open = boxcfg_to_json(&c, false);
    cJSON *o = cJSON_Parse(open);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(o, "wifiPass")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(o, "adminPass")->valuestring);
    TEST_ASSERT_EQUAL_STRING(c.ap_pass, cJSON_GetObjectItem(o, "apPass")->valuestring);
    cJSON_Delete(o);
    free(open);
    char *full = boxcfg_to_json(&c, true);
    o = cJSON_Parse(full);
    TEST_ASSERT_EQUAL_STRING("wifisecret", cJSON_GetObjectItem(o, "wifiPass")->valuestring);
    TEST_ASSERT_EQUAL_STRING("adm", cJSON_GetObjectItem(o, "adminPass")->valuestring);
    cJSON_Delete(o);
    /* round trip */
    box_config_t r = fresh();
    memset(&r, 0, sizeof r);
    char err[96];
    TEST_ASSERT_TRUE(boxcfg_from_json(&r, full, err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
    free(full);
}

static void test_satellite_config_post(void)
{
    box_config_t c = fresh();
    char err[96];
    strcpy(c.companion_pin_id, "pin");
    TEST_ASSERT_TRUE(boxcfg_apply_satellite_config(&c, "{\"host\":\"10.1.2.3\",\"port\":16622}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("10.1.2.3", c.companion_host);
    TEST_ASSERT_FALSE(c.companion_auto);
    TEST_ASSERT_EQUAL_STRING("", c.companion_pin_id);
    TEST_ASSERT_TRUE(boxcfg_apply_satellite_config(&c, "{\"protocol\":\"tcp\",\"port\":\"17001\",\"installationName\":\"Rack\",\"mdnsEnabled\":false}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT16(17001, c.companion_port);
    TEST_ASSERT_EQUAL_STRING("Rack", c.box_name);
    TEST_ASSERT_FALSE(c.mdns_enabled);
    box_config_t before = c;
    TEST_ASSERT_FALSE(boxcfg_apply_satellite_config(&c, "{\"protocol\":\"ws\"}", err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_apply_satellite_config(&c, "{\"host\":\"h\",\"mdnsEnabled\":\"no\"}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "mdnsEnabled"));
    TEST_ASSERT_FALSE(boxcfg_apply_satellite_config(&c, "{\"host\":\"h\",\"port\":0}", err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_apply_satellite_config(&c, "{\"host\":\"h\",\"port\":\"12x\"}", err, sizeof err));
    TEST_ASSERT_FALSE(boxcfg_apply_satellite_config(&c, "{\"installationName\":\"\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
}

static void test_err_key(void)
{
    box_config_t c = fresh();
    char err[96], key[24];
    TEST_ASSERT_FALSE(boxcfg_apply_satellite_config(&c, "{\"host\":\"h\",\"port\":0}", err, sizeof err));
    boxcfg_err_key(err, key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("port", key);
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"companionPort\":\"x\"}", err, sizeof err));
    boxcfg_err_key(err, key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("companionPort", key);
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{", err, sizeof err));
    boxcfg_err_key(err, key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("json", key);
    boxcfg_err_key("", key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("", key);
}

static void test_surface_mode(void)
{
    box_config_t c = fresh();
    char err[96];
    TEST_ASSERT_EQUAL_INT(BOX_SURFACE_DOCK, c.surface_mode); /* default */
    char *s = boxcfg_to_json(&c, false);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"surfaceMode\":\"dock\""));
    free(s);

    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"surfaceMode\":\"satellite\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(BOX_SURFACE_SATELLITE, c.surface_mode);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"surfaceMode\":\"dock\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(BOX_SURFACE_DOCK, c.surface_mode);
    s = boxcfg_to_json(&c, true);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"surfaceMode\":\"dock\""));
    box_config_t r = fresh(); /* round trip */
    TEST_ASSERT_TRUE(boxcfg_from_json(&r, s, err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
    free(s);

    box_config_t before = c;
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"surfaceMode\":\"usb\"}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "surfaceMode"));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"surfaceMode\":1}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "surfaceMode"));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);

    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"surfaceMode\":\"satellite\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(BOX_SURFACE_SATELLITE, c.surface_mode);
}

static void test_satellite_config_get(void)
{
    box_config_t c = fresh();
    char *s = boxcfg_satellite_config_json(&c);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"httpPort\":80"));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"installationName\":\"CompanionBox-A1B2\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"protocol\":\"tcp\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"port\":16622"));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"mdnsEnabled\":true"));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"httpEnabled\":true"));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"host\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"wsAddress\":\"\""));
    free(s);
}

/* ---- Loading never destroys the stored config ---- */

#define HEX64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* A config written by a newer firmware (wider fields, new keys): the valid fields load, the rest is
 * skipped and named. */
static void test_lenient_keeps_valid_fields(void)
{
    box_config_t c = fresh(), d = fresh();
    char skipped[160];
    const int n = boxcfg_from_json_lenient(&c,
        "{\"boxName\":\"Rack\",\"companionPort\":70000,\"wifiSsid\":\"Net\",\"wifiPass\":\"" HEX64 "\","
        "\"surfaceMode\":\"triple\",\"foo\":1,\"apPass\":\"keepthis1\"}", skipped, sizeof skipped);
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_STRING("Rack", c.box_name);
    TEST_ASSERT_EQUAL_STRING("Net", c.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("keepthis1", c.ap_pass);
    TEST_ASSERT_EQUAL_UINT16(d.companion_port, c.companion_port);
    TEST_ASSERT_EQUAL_STRING("", c.wifi_pass);
    TEST_ASSERT_EQUAL_INT(d.surface_mode, c.surface_mode); /* skipped: the default stays */
    TEST_ASSERT_NOT_NULL(strstr(skipped, "companionPort"));
    TEST_ASSERT_NOT_NULL(strstr(skipped, "wifiPass"));
    TEST_ASSERT_NOT_NULL(strstr(skipped, "surfaceMode"));
    TEST_ASSERT_NOT_NULL(strstr(skipped, "foo"));
    TEST_ASSERT_NULL(strstr(skipped, HEX64)); /* never a value: wifiPass is a secret */
}

/* The static address is checked as a whole, whatever the key order: an invalid one leaves DHCP on. */
static void test_lenient_static_ip(void)
{
    box_config_t c = fresh();
    char skipped[160];
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&c,
        "{\"ethDhcp\":false,\"ethIp\":\"10.0.0.2\",\"ethMask\":\"255.255.255.0\",\"ethGw\":\"10.0.0.1\"}",
        skipped, sizeof skipped));
    TEST_ASSERT_FALSE(c.eth_dhcp);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", c.eth_ip);
    c = fresh();
    TEST_ASSERT_EQUAL_INT(1, boxcfg_from_json_lenient(&c,
        "{\"ethDhcp\":false,\"ethIp\":\"abc\",\"ethMask\":\"255.255.255.0\",\"ethGw\":\"10.0.0.1\"}",
        skipped, sizeof skipped));
    TEST_ASSERT_TRUE(c.eth_dhcp);
    TEST_ASSERT_NOT_NULL(strstr(skipped, "eth"));
}

/* Not a JSON object: nothing loaded, -1. */
static void test_lenient_not_json(void)
{
    box_config_t c = fresh(), before = c;
    char skipped[64];
    TEST_ASSERT_EQUAL_INT(-1, boxcfg_from_json_lenient(&c, "{\"boxName\":\"Ra", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_INT(-1, boxcfg_from_json_lenient(&c, "[1,2]", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_INT(-1, boxcfg_from_json_lenient(&c, NULL, skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
}

/* What this firmware writes loads back whole, with nothing skipped (keeps the key list in step). */
static void test_lenient_round_trip(void)
{
    box_config_t c = fresh();
    strcpy(c.wifi_ssid, "Net");
    strcpy(c.wifi_pass, "secret123");
    strcpy(c.admin_pass, "admin");
    c.eth_dhcp = false;
    strcpy(c.eth_ip, "10.0.0.2");
    strcpy(c.eth_mask, "255.255.255.0");
    strcpy(c.eth_gw, "10.0.0.1");
    c.surface_mode = BOX_SURFACE_DOCK;
    c.deck_brightness = 60;
    c.deck_limit = 4;
    c.gesture_enabled = false;
    c.gesture_keys[0] = BOXCFG_CORNER_BOTTOM_LEFT;
    c.gesture_keys[1] = BOXCFG_CORNER_TOP_LEFT;
    c.gesture_taps = 3;
    c.gesture_hold_s = 5;
    char *full = boxcfg_to_json(&c, true);
    box_config_t r = fresh();
    char skipped[64];
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&r, full, skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_STRING("", skipped);
    TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
    free(full);
}

/* ledFlash: a stored config without it keeps the default (on); with it, false loads and nothing is skipped. */
static void test_lenient_led_flash(void)
{
    box_config_t c = fresh();
    char skipped[64];
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&c, "{\"ledEnabled\":false}", skipped, sizeof skipped));
    TEST_ASSERT_TRUE(c.led_flash);
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&c, "{\"ledFlash\":false,\"ledPin\":27}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_STRING("", skipped);
    TEST_ASSERT_FALSE(c.led_flash);
    TEST_ASSERT_EQUAL_INT8(27, c.led_pin);
}

/* deckBrightness (the decks while the box drives them): 10-100 in steps of 10, default 70 (a
 * dimmed deck doesn't jump to full when the box takes it); loaded leniently, round-trips. */
static void test_deck_brightness(void)
{
    box_config_t c = fresh();
    char err[96], skipped[64];
    TEST_ASSERT_EQUAL_UINT8(70, c.deck_brightness);
    char *s = boxcfg_to_json(&c, false);
    cJSON *o = cJSON_Parse(s);
    TEST_ASSERT_EQUAL_INT(70, cJSON_GetObjectItem(o, "deckBrightness")->valueint);
    cJSON_Delete(o);
    free(s);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"deckBrightness\":10}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(10, c.deck_brightness);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"deckBrightness\":70}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(70, c.deck_brightness);
    static const char *const BAD[] = {"{\"deckBrightness\":0}", "{\"deckBrightness\":5}", "{\"deckBrightness\":75}",
                                      "{\"deckBrightness\":110}", "{\"deckBrightness\":\"70\"}", "{\"deckBrightness\":70.5}"};
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        TEST_ASSERT_FALSE_MESSAGE(boxcfg_from_json(&c, BAD[i], err, sizeof err), BAD[i]);
        char key[24];
        boxcfg_err_key(err, key, sizeof key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("deckBrightness", key, BAD[i]);
        TEST_ASSERT_EQUAL_UINT8(70, c.deck_brightness);
    }
    box_config_t l = fresh(); /* the stored config: known to the lenient loader, an old one without it keeps 70 */
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&l, "{\"deckBrightness\":40}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_STRING("", skipped);
    TEST_ASSERT_EQUAL_UINT8(40, l.deck_brightness);
    l = fresh();
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&l, "{\"ledEnabled\":true}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_UINT8(70, l.deck_brightness);
    TEST_ASSERT_EQUAL_INT(1, boxcfg_from_json_lenient(&l, "{\"deckBrightness\":45}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_UINT8(70, l.deck_brightness);
}

/* deckLimit: the Stream Decks at once, 2 (the default, and for a stored config without it) or 4; nothing
 * else, by the API or from the stored config (the lenient loader skips it and keeps 2); round-trips. */
static void test_deck_limit(void)
{
    box_config_t c = fresh();
    char err[96], skipped[64];
    TEST_ASSERT_EQUAL_UINT8(2, c.deck_limit);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_DECK_LIMIT, c.deck_limit);
    char *s = boxcfg_to_json(&c, false);
    cJSON *o = cJSON_Parse(s);
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItem(o, "deckLimit")->valueint);
    cJSON_Delete(o);
    free(s);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"deckLimit\":4}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(4, c.deck_limit);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"deckLimit\":2}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(2, c.deck_limit);
    static const char *const BAD[] = {"{\"deckLimit\":0}", "{\"deckLimit\":1}", "{\"deckLimit\":3}", "{\"deckLimit\":5}",
                                      "{\"deckLimit\":6}", "{\"deckLimit\":\"4\"}", "{\"deckLimit\":4.5}",
                                      "{\"deckLimit\":true}", "{\"deckLimit\":-2}"};
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        TEST_ASSERT_FALSE_MESSAGE(boxcfg_from_json(&c, BAD[i], err, sizeof err), BAD[i]);
        char key[24];
        boxcfg_err_key(err, key, sizeof key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("deckLimit", key, BAD[i]);
        TEST_ASSERT_EQUAL_UINT8(2, c.deck_limit);
    }
    box_config_t l = fresh(); /* the stored config: 4 loads; an old one without the key keeps 2; a bad one is skipped */
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&l, "{\"deckLimit\":4}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_STRING("", skipped);
    TEST_ASSERT_EQUAL_UINT8(4, l.deck_limit);
    l = fresh();
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&l, "{\"ledEnabled\":true}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_UINT8(2, l.deck_limit);
    TEST_ASSERT_EQUAL_INT(1, boxcfg_from_json_lenient(&l, "{\"deckLimit\":6}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_UINT8(2, l.deck_limit);
}

/* API writes stay all or nothing. */
static void test_strict_still_all_or_nothing(void)
{
    box_config_t c = fresh(), before = c;
    char err[96];
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"boxName\":\"Rack\",\"wifiPass\":\"" HEX64 "\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
}

/* Saved at start only when nothing was stored, or when a stored config that loaded cleanly differs
 * from what this firmware writes; never over one that couldn't be read, parsed or loaded whole. */
static void test_store_decision(void)
{
    TEST_ASSERT_TRUE(boxcfg_should_store(BOXCFG_LOAD_ABSENT, false));
    TEST_ASSERT_TRUE(boxcfg_should_store(BOXCFG_LOAD_CLEAN, false));
    TEST_ASSERT_FALSE(boxcfg_should_store(BOXCFG_LOAD_CLEAN, true));
    TEST_ASSERT_FALSE(boxcfg_should_store(BOXCFG_LOAD_PARTIAL, false));
    TEST_ASSERT_FALSE(boxcfg_should_store(BOXCFG_LOAD_FAILED, false));
    TEST_ASSERT_FALSE(boxcfg_should_store(BOXCFG_LOAD_FAILED, true));
}

/* The speaker and the status light (components/feedback): on, volume 50 %, every sound on, the LED on
 * GPIO23 at 30 %. */
static void test_feedback_defaults(void)
{
    box_config_t c = fresh();
    TEST_ASSERT_TRUE(c.speaker_enabled);
    TEST_ASSERT_EQUAL_UINT8(50, c.speaker_volume);
    TEST_ASSERT_TRUE(c.sound_ready);
    TEST_ASSERT_TRUE(c.sound_connected);
    TEST_ASSERT_TRUE(c.sound_problem);
    TEST_ASSERT_TRUE(c.sound_deck_plug);
    TEST_ASSERT_TRUE(c.sound_key_click);
    TEST_ASSERT_TRUE(c.led_enabled);
    TEST_ASSERT_TRUE(c.led_flash);
    TEST_ASSERT_EQUAL_INT8(23, c.led_pin); /* the case's NeoPixel pin */
    TEST_ASSERT_EQUAL_UINT8(30, c.led_brightness);
    char *s = boxcfg_to_json(&c, false);
    cJSON *o = cJSON_Parse(s);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(o, "speakerEnabled")));
    TEST_ASSERT_EQUAL_INT(50, cJSON_GetObjectItem(o, "speakerVolume")->valueint);
    static const char *const SOUNDS[] = {"soundReady", "soundConnected", "soundProblem", "soundDeckPlug", "soundKeyClick"};
    for (size_t i = 0; i < sizeof SOUNDS / sizeof *SOUNDS; i++)
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItem(o, SOUNDS[i])), SOUNDS[i]);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(o, "ledEnabled")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(o, "ledFlash")));
    TEST_ASSERT_EQUAL_INT(23, cJSON_GetObjectItem(o, "ledPin")->valueint);
    TEST_ASSERT_EQUAL_INT(30, cJSON_GetObjectItem(o, "ledBrightness")->valueint);
    cJSON_Delete(o);
    free(s);
}

static void test_feedback_json(void)
{
    box_config_t c = fresh();
    char err[96];
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"speakerEnabled\":false,\"speakerVolume\":0,\"soundReady\":false,"
                                          "\"soundConnected\":false,\"soundProblem\":false,\"soundDeckPlug\":false,"
                                          "\"soundKeyClick\":false,\"ledEnabled\":false,\"ledFlash\":false,\"ledPin\":27,\"ledBrightness\":100}",
                                      err, sizeof err));
    TEST_ASSERT_FALSE(c.speaker_enabled);
    TEST_ASSERT_EQUAL_UINT8(0, c.speaker_volume);
    TEST_ASSERT_FALSE(c.sound_ready || c.sound_connected || c.sound_problem || c.sound_deck_plug || c.sound_key_click);
    TEST_ASSERT_FALSE(c.led_enabled);
    TEST_ASSERT_FALSE(c.led_flash);
    TEST_ASSERT_EQUAL_INT8(27, c.led_pin);
    TEST_ASSERT_EQUAL_UINT8(100, c.led_brightness);
    char *s = boxcfg_to_json(&c, true); /* round trip */
    box_config_t r = fresh();
    TEST_ASSERT_TRUE(boxcfg_from_json(&r, s, err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
    free(s);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"speakerVolume\":100,\"ledBrightness\":1,\"ledPin\":0}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(100, c.speaker_volume);
    TEST_ASSERT_EQUAL_UINT8(1, c.led_brightness);
    TEST_ASSERT_EQUAL_INT8(0, c.led_pin);

    /* refused, naming the key; the config stays as it was */
    static const char *const BAD[][2] = {
        {"{\"speakerEnabled\":1}", "speakerEnabled"},  {"{\"speakerVolume\":101}", "speakerVolume"},
        {"{\"speakerVolume\":-1}", "speakerVolume"},   {"{\"speakerVolume\":50.5}", "speakerVolume"},
        {"{\"speakerVolume\":\"50\"}", "speakerVolume"}, {"{\"soundKeyClick\":\"on\"}", "soundKeyClick"},
        {"{\"soundDeckPlug\":null}", "soundDeckPlug"}, {"{\"ledEnabled\":0}", "ledEnabled"}, {"{\"ledFlash\":1}", "ledFlash"},
        {"{\"ledBrightness\":0}", "ledBrightness"},    {"{\"ledBrightness\":101}", "ledBrightness"},
        {"{\"ledPin\":32}", "ledPin"},                 {"{\"ledPin\":7}", "ledPin"},
        {"{\"ledPin\":9}", "ledPin"},                  {"{\"ledPin\":-1}", "ledPin"},
        {"{\"ledPin\":55}", "ledPin"},                 {"{\"ledPin\":26.5}", "ledPin"},
        {"{\"ledPin\":\"26\"}", "ledPin"},
    };
    box_config_t before = c;
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        TEST_ASSERT_FALSE_MESSAGE(boxcfg_from_json(&c, BAD[i][0], err, sizeof err), BAD[i][0]);
        char key[24];
        boxcfg_err_key(err, key, sizeof key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(BAD[i][1], key, BAD[i][0]);
        TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
    }
    /* why a pin is refused */
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"ledPin\":32}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "service button"));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"ledPin\":8}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "speaker"));
}

/* The LED's data pin: a free header pin of the board that this firmware
 * doesn't use itself (GPIO7/8 the codec's I2C bus, GPIO32 the service button). */
static void test_led_pins(void)
{
    static const int WANT[] = {0, 1, 2, 3, 4, 5, 20, 21, 22, 23, 26, 27, 33, 46, 47, 48};
    int pins[64];
    const int n = boxcfg_led_pins(pins, 64);
    TEST_ASSERT_EQUAL_INT(sizeof WANT / sizeof *WANT, n);
    TEST_ASSERT_EQUAL_INT_ARRAY(WANT, pins, n);
    TEST_ASSERT_EQUAL_INT(3, boxcfg_led_pins(pins, 3)); /* cut to max */
    for (int g = -2; g <= 60; g++) {
        bool listed = false;
        for (size_t i = 0; i < sizeof WANT / sizeof *WANT; i++) listed |= WANT[i] == g;
        const char *why = boxcfg_led_pin_problem(g);
        if (listed) TEST_ASSERT_NULL_MESSAGE(why, "a free pin is refused");
        else TEST_ASSERT_NOT_NULL_MESSAGE(why, "a taken or missing pin is accepted");
    }
    TEST_ASSERT_NOT_NULL(strstr(boxcfg_led_pin_problem(32), "service button"));
    TEST_ASSERT_NOT_NULL(strstr(boxcfg_led_pin_problem(7), "speaker"));
    TEST_ASSERT_NOT_NULL(strstr(boxcfg_led_pin_problem(53), "speaker"));
    TEST_ASSERT_NOT_NULL(strstr(boxcfg_led_pin_problem(28), "Ethernet"));
}

/* ---- The switch: the hotspot has a password unless it is switched off ---- */

static void test_ap_secured_json(void)
{
    box_config_t c = fresh();
    char err[96];
    char *s = boxcfg_to_json(&c, false);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"apSecured\":true")); /* the default */
    free(s);

    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"apSecured\":false}", err, sizeof err)); /* the open option */
    TEST_ASSERT_FALSE(c.ap_secured);
    s = boxcfg_to_json(&c, false);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"apSecured\":false"));
    free(s);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"apSecured\":true}", err, sizeof err));
    TEST_ASSERT_TRUE(c.ap_secured);

    /* strict round trip, both states */
    for (int sec = 0; sec < 2; sec++) {
        c.ap_secured = sec;
        s = boxcfg_to_json(&c, true);
        box_config_t r = fresh();
        r.ap_secured = !sec;
        TEST_ASSERT_TRUE(boxcfg_from_json(&r, s, err, sizeof err));
        TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
        free(s);
    }

    box_config_t before = c;
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"apSecured\":\"yes\"}", err, sizeof err));
    char key[24];
    boxcfg_err_key(err, key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("apSecured", key);
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"apSecured\":1}", err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
}

/* apPass is checked (8-63) only while the password is on; an open hotspot keeps whatever is stored. */
static void test_ap_pass_validated_only_when_secured(void)
{
    box_config_t c = fresh();
    char err[96], key[24];
    const char *const first = "abcdefghjk";
    strcpy(c.ap_pass, first);

    /* open: switching the password off keeps the stored one, unused */
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"apSecured\":false}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING(first, c.ap_pass);
    /* open: any 0-63 value is taken as it is and not used */
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"apPass\":\"short\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("short", c.ap_pass);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"apPass\":\"\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("", c.ap_pass);
    char big[80], js[128];
    memset(big, 'a', 64);
    big[64] = '\0';
    snprintf(js, sizeof js, "{\"apPass\":\"%s\"}", big);
    box_config_t before = c;
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, js, err, sizeof err)); /* 64: too long whatever the state */
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);

    /* switching it on with an unusable stored one: refused, naming apPass (the field to fill in) */
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"apSecured\":true}", err, sizeof err));
    boxcfg_err_key(err, key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("apPass", key);
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"apSecured\":true,\"apPass\":\"1234567\"}", err, sizeof err));
    boxcfg_err_key(err, key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("apPass", key);
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);

    /* on, with 8 and with 63 characters */
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"apSecured\":true,\"apPass\":\"12345678\"}", err, sizeof err));
    TEST_ASSERT_TRUE(c.ap_secured);
    TEST_ASSERT_EQUAL_STRING("12345678", c.ap_pass);
    big[63] = '\0';
    snprintf(js, sizeof js, "{\"apPass\":\"%s\"}", big);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, js, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING(big, c.ap_pass);

    /* on: a short one is refused */
    before = c;
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"apPass\":\"short\"}", err, sizeof err));
    boxcfg_err_key(err, key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("apPass", key);
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);

    /* off and on again: the same password comes back */
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"apSecured\":false}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING(big, c.ap_pass);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"apSecured\":true}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING(big, c.ap_pass);
}

/* A box updated from a firmware without apSecured: its stored config loads whole and the hotspot has
 * its old password (a missing key reads as true). */
static void test_ap_old_stored_config_secured(void)
{
    box_config_t c = fresh();
    char skipped[64];
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&c,
        "{\"boxName\":\"Rack\",\"apEnabled\":true,\"apPass\":\"oldpass99\",\"surfaceMode\":\"dock\"}",
        skipped, sizeof skipped));
    TEST_ASSERT_TRUE(c.ap_secured);
    TEST_ASSERT_EQUAL_STRING("oldpass99", c.ap_pass);
}

/* A stored secured config with an unusable password (only a corrupted one has it) loads secured with
 * the valid random default, never with the bad one: apPass is skipped and named. */
static void test_ap_stored_secured_bad_pass(void)
{
    static const char *const J[] = {"{\"apSecured\":true,\"apPass\":\"short\"}", "{\"apPass\":\"\",\"apSecured\":true}"};
    for (size_t i = 0; i < sizeof J / sizeof J[0]; i++) {
        box_config_t c = fresh();
        const box_config_t def = c;
        char skipped[96];
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, boxcfg_from_json_lenient(&c, J[i], skipped, sizeof skipped), J[i]);
        TEST_ASSERT_TRUE_MESSAGE(c.ap_secured, J[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(def.ap_pass, c.ap_pass, J[i]);
        TEST_ASSERT_EQUAL_UINT(10, strlen(c.ap_pass));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(skipped, "apPass"), J[i]);
    }
}

/* A box whose stored config switched the password off stays open after the update, whatever it stored
 * as the (unused) password and in whichever key order (the bench box, saved by cfa7928). */
static void test_ap_stored_open_stays_open(void)
{
    char skipped[96];
    static const char *const J[] = {
        "{\"apEnabled\":true,\"apSecured\":false,\"apPass\":\"abcdefghjk\"}",
        "{\"apPass\":\"short\",\"apSecured\":false}", /* a short one, before the switch */
        "{\"apPass\":\"\",\"apEnabled\":true,\"apSecured\":false}",
    };
    for (size_t i = 0; i < sizeof J / sizeof J[0]; i++) {
        box_config_t c = fresh();
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, boxcfg_from_json_lenient(&c, J[i], skipped, sizeof skipped), J[i]);
        TEST_ASSERT_FALSE_MESSAGE(c.ap_secured, J[i]);
    }
    box_config_t c = fresh();
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&c, J[1], skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_STRING("short", c.ap_pass);
}

/* The lenient loader takes apSecured both ways, in either key order. */
static void test_ap_lenient_round_trip(void)
{
    char skipped[64];
    for (int sec = 0; sec < 2; sec++) {
        box_config_t c = fresh();
        c.ap_secured = sec;
        strcpy(c.ap_pass, "storedpass1");
        char *full = boxcfg_to_json(&c, true);
        box_config_t r = fresh();
        TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&r, full, skipped, sizeof skipped));
        TEST_ASSERT_EQUAL_STRING("", skipped);
        TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
        free(full);
    }
    box_config_t r = fresh();
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&r, "{\"apPass\":\"storedpass1\",\"apSecured\":true}",
                                                      skipped, sizeof skipped));
    TEST_ASSERT_TRUE(r.ap_secured);
    TEST_ASSERT_EQUAL_STRING("storedpass1", r.ap_pass);
}

/* ---- the backup Wi-Fi network ---- */

static void test_backup_defaults_empty(void)
{
    box_config_t c = fresh();
    TEST_ASSERT_EQUAL_STRING("", c.wifi_backup_ssid);
    TEST_ASSERT_EQUAL_STRING("", c.wifi_backup_pass);
}

static void test_backup_json_round_trip_and_secret(void)
{
    box_config_t c = fresh(), r = fresh();
    char err[96];
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"wifiBackupSsid\":\"Phone\",\"wifiBackupPass\":\"hotspot99\"}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("Phone", c.wifi_backup_ssid);
    TEST_ASSERT_EQUAL_STRING("hotspot99", c.wifi_backup_pass);
    char *full = boxcfg_to_json(&c, true);
    TEST_ASSERT_TRUE(boxcfg_from_json(&r, full, err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
    free(full);
    char *open = boxcfg_to_json(&c, false); /* the password is a secret like the main one */
    cJSON *o = cJSON_Parse(open);
    TEST_ASSERT_EQUAL_STRING("Phone", cJSON_GetObjectItem(o, "wifiBackupSsid")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(o, "wifiBackupPass")->valuestring);
    cJSON_Delete(o);
    free(open);
}

static void test_backup_validation(void)
{
    box_config_t c = fresh(), before = c;
    char err[96];
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"wifiBackupPass\":\"1234567\"}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "wifiBackupPass"));
    TEST_ASSERT_NOT_NULL(strstr(err, "must be empty or 8-63 characters"));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"wifiBackupSsid\":\"123456789012345678901234567890123\"}", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "wifiBackupSsid"));
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"wifiBackupSsid\":5}", err, sizeof err));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof c);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"wifiBackupPass\":\"\"}", err, sizeof err));
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"wifiBackupPass\":\"12345678\"}", err, sizeof err));
}

/* the lenient load knows the new keys (an unknown key is skipped, so a forgotten list entry would
 * silently drop the stored backup at every boot). */
static void test_backup_lenient_old_config_without_keys(void)
{
    box_config_t c = fresh();
    char skipped[96];
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&c, "{\"boxName\":\"Rack\",\"wifiSsid\":\"Net\"}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_STRING("", skipped);
    TEST_ASSERT_EQUAL_STRING("", c.wifi_backup_ssid);
    TEST_ASSERT_EQUAL_STRING("", c.wifi_backup_pass);
}

static void test_backup_lenient_with_keys(void)
{
    box_config_t c = fresh();
    char skipped[96];
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&c, "{\"wifiBackupSsid\":\"Phone\",\"wifiBackupPass\":\"hotspot99\"}", skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_STRING("", skipped);
    TEST_ASSERT_EQUAL_STRING("Phone", c.wifi_backup_ssid);
    TEST_ASSERT_EQUAL_STRING("hotspot99", c.wifi_backup_pass);
}

static void test_backup_lenient_bad_password_skipped(void)
{
    box_config_t c = fresh();
    char skipped[160];
    TEST_ASSERT_EQUAL_INT(1, boxcfg_from_json_lenient(&c, "{\"wifiBackupSsid\":\"Phone\",\"wifiBackupPass\":\"short\"}", skipped, sizeof skipped));
    TEST_ASSERT_NOT_NULL(strstr(skipped, "wifiBackupPass"));
    TEST_ASSERT_NOT_NULL(strstr(skipped, "must be empty or 8-63 characters"));
    TEST_ASSERT_NULL(strstr(skipped, "short\"")); /* never a value */
    TEST_ASSERT_EQUAL_STRING("Phone", c.wifi_backup_ssid);
    TEST_ASSERT_EQUAL_STRING("", c.wifi_backup_pass);
}

static void test_backup_lenient_round_trip(void)
{
    box_config_t c = fresh(), r = fresh();
    strcpy(c.wifi_backup_ssid, "Phone");
    strcpy(c.wifi_backup_pass, "hotspot99");
    char *full = boxcfg_to_json(&c, true);
    char skipped[64];
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&r, full, skipped, sizeof skipped));
    TEST_ASSERT_EQUAL_STRING("", skipped);
    TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
    free(full);
}

/* wifiEnabled: on by default, written and read back, a stored config of before (without it) loads as on,
 * and only a bool is taken. */
static void test_wifi_enabled(void)
{
    box_config_t c = fresh();
    TEST_ASSERT_TRUE(c.wifi_enabled);
    char err[96];
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"wifiEnabled\":false}", err, sizeof err));
    TEST_ASSERT_FALSE(c.wifi_enabled);
    char *js = boxcfg_to_json(&c, true); /* the round trip, as the box stores it */
    TEST_ASSERT_NOT_NULL(strstr(js, "\"wifiEnabled\":false"));
    box_config_t d = fresh();
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&d, js, NULL, 0));
    TEST_ASSERT_FALSE(d.wifi_enabled);
    free(js);
    box_config_t old = fresh(); /* a stored config of before: no wifiEnabled */
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&old, "{\"wifiSsid\":\"Studio\",\"apEnabled\":true}", NULL, 0));
    TEST_ASSERT_TRUE(old.wifi_enabled);
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"wifiEnabled\":0}", err, sizeof err)); /* a bool only */
    TEST_ASSERT_FALSE(c.wifi_enabled);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"wifiEnabled\":true}", err, sizeof err));
    TEST_ASSERT_TRUE(c.wifi_enabled);
}

/* *.SetConfig prints its object and parses it again (set_section), so a
 * number 1 ulp from an integer (2 below a power of two) prints as that integer and is taken, rounded; 2 ulps away it
 * prints as itself and is refused. This is the path set_section takes: the object printed, then boxcfg_from_json. The
 * direct boxcfg_from_cjson (Companion.Choose only, whose port is an mDNS integer) refuses any of them: why set_section
 * doesn't use it. */
static void test_setconfig_near_integers(void)
{
    static const struct {
        const char *key;
        double v;
        const char *ok; /* u = -2..+2 ulps: '1' taken (as v), '0' refused */
    } P[] = {
        {"companionPort", 16700, "01110"}, {"companionPort", 1, "11110"},  {"speakerVolume", 40, "01110"},
        {"speakerVolume", 50, "01110"},    {"ledBrightness", 64, "11110"}, {"deckLimit", 4, "11110"},
        {"deckLimit", 2, "11110"},        {"gestureTaps", 2, "11110"},    {"gestureHoldSeconds", 5, "01110"},
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        for (int u = -2; u <= 2; u++) {
            double v = P[i].v;
            for (int j = 0; j < (u < 0 ? -u : u); j++) v = nextafter(v, u < 0 ? -INFINITY : INFINITY);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, P[i].key, v);
            char *json = cJSON_PrintUnformatted(o); /* set_section */
            box_config_t a = fresh(), b = a;
            char err[96], what[96];
            snprintf(what, sizeof what, "%s %.17g (%+d ulp): %s", P[i].key, v, u, json);
            const bool want = P[i].ok[u + 2] == '1';
            TEST_ASSERT_EQUAL_MESSAGE(want, boxcfg_from_json(&a, json, err, sizeof err), what);
            if (want) {
                const double got = !strcmp(P[i].key, "companionPort") ? a.companion_port
                                   : !strcmp(P[i].key, "speakerVolume") ? a.speaker_volume
                                   : !strcmp(P[i].key, "ledBrightness") ? a.led_brightness
                                   : !strcmp(P[i].key, "gestureTaps")   ? a.gesture_taps
                                   : !strcmp(P[i].key, "gestureHoldSeconds") ? a.gesture_hold_s
                                                                         : a.deck_limit;
                TEST_ASSERT_EQUAL_INT_MESSAGE((int)P[i].v, (int)got, what); /* rounded to the integer */
            }
            TEST_ASSERT_EQUAL_MESSAGE(u == 0, boxcfg_from_cjson(&b, o, err, sizeof err), what);
            free(json);
            cJSON_Delete(o);
        }
}

/* The host name rule (DHCP, mDNS, the Host check): lower case letters and digits, anything else '-', at most
 * n - 1 characters; a 32-char box name whole in the 33 bytes netmgr and discovery give it. */
static void test_hostname(void)
{
    char h[40];
    boxcfg_hostname("CompanionBox-8D1C", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("companionbox-8d1c", h);
    boxcfg_hostname("Rack 2 / Left", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("rack-2---left", h);
    boxcfg_hostname("caf\xc3\xa9_\x01X", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("caf----x", h); /* UTF-8 bytes, '_' and controls: '-' each */
    boxcfg_hostname("abcdefgh", h, 5);
    TEST_ASSERT_EQUAL_STRING("abcd", h);
    char h33[33];
    boxcfg_hostname("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", h33, sizeof h33);
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz012345", h33);
    h[0] = 'x';
    boxcfg_hostname("abc", h, 0); /* no room: nothing written */
    TEST_ASSERT_EQUAL_CHAR('x', h[0]);
    boxcfg_hostname(NULL, h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("", h);
}

/* the whole Companion target: auto, the pinned id, the host and the port; nothing else counts. */
static void test_same_target(void)
{
    const box_config_t a = fresh();
    box_config_t b = a;
    TEST_ASSERT_TRUE(boxcfg_same_target(&a, &b));
    b.companion_auto = !a.companion_auto;
    TEST_ASSERT_FALSE(boxcfg_same_target(&a, &b));
    b = a;
    strcpy(b.companion_pin_id, "id-a"); /* the pin, the same host and port: a change */
    TEST_ASSERT_FALSE(boxcfg_same_target(&a, &b));
    b = a;
    strcpy(b.companion_host, "10.0.0.5");
    TEST_ASSERT_FALSE(boxcfg_same_target(&a, &b));
    b = a;
    b.companion_port++;
    TEST_ASSERT_FALSE(boxcfg_same_target(&a, &b));
    b = a;
    strcpy(b.box_name, "Other"); /* anything else: the same target */
    b.mdns_enabled = !a.mdns_enabled;
    b.deck_limit = 4;
    TEST_ASSERT_TRUE(boxcfg_same_target(&a, &b));
    /* auto with a leftover host, no pin; Companion at that host adopts (adopting pins it) */
    box_config_t before = a, after;
    before.companion_auto = true;
    strcpy(before.companion_host, "10.0.0.5");
    before.companion_pin_id[0] = '\0';
    after = before;
    char err[64];
    TEST_ASSERT_TRUE(boxcfg_apply_satellite_config(&after, "{\"host\":\"10.0.0.5\",\"port\":16622}", err, sizeof err));
    after.companion_auto = true; /* discovery_pin_posted: the visible instance at that host */
    strcpy(after.companion_pin_id, "id-a");
    TEST_ASSERT_FALSE(boxcfg_same_target(&before, &after)); /* applied, not skipped */
    TEST_ASSERT_TRUE(boxcfg_same_target(&after, &after));   /* the same Companion again: quiet */
}

/* companionAdopt, true by default and for a stored config without it; a bool only; stored and read back. */
static void test_companion_adopt(void)
{
    box_config_t c = fresh();
    TEST_ASSERT_TRUE(c.companion_adopt);
    char err[96];
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"companionAdopt\":false}", err, sizeof err));
    TEST_ASSERT_FALSE(c.companion_adopt);
    char *js = boxcfg_to_json(&c, true);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"companionAdopt\":false"));
    box_config_t d = fresh();
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&d, js, NULL, 0));
    TEST_ASSERT_FALSE(d.companion_adopt);
    free(js);
    box_config_t old = fresh(); /* a stored config of before: no companionAdopt */
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&old, "{\"companionHost\":\"10.0.0.5\",\"adminPass\":\"\"}", NULL, 0));
    TEST_ASSERT_TRUE(old.companion_adopt);
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "{\"companionAdopt\":1}", err, sizeof err)); /* a bool only */
    char key[32];
    boxcfg_err_key(err, key, sizeof key);
    TEST_ASSERT_EQUAL_STRING("companionAdopt", key);
    TEST_ASSERT_FALSE(c.companion_adopt);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"companionAdopt\":true}", err, sizeof err));
    TEST_ASSERT_TRUE(c.companion_adopt);
}

/* the box menu gesture. gestureEnabled (default true), gestureKeys (two different corners, the first tapped;
 * default ["topRight","bottomRight"]), gestureTaps (1-3, default 1), gestureHoldSeconds (2, 3 or 5, default 2): the default
 * gesture for a fresh box and for a stored config without them; anything else refused (the API) or skipped (the
 * stored config); round-trips. */
static void test_gesture(void)
{
    box_config_t c = fresh();
    TEST_ASSERT_TRUE(c.gesture_enabled);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_TOP_RIGHT, c.gesture_keys[0]);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_BOTTOM_RIGHT, c.gesture_keys[1]);
    TEST_ASSERT_EQUAL_UINT8(1, c.gesture_taps);
    TEST_ASSERT_EQUAL_UINT8(2, c.gesture_hold_s);
    TEST_ASSERT_EQUAL(0, BOXCFG_CORNER_TOP_LEFT);
    TEST_ASSERT_EQUAL(1, BOXCFG_CORNER_TOP_RIGHT);
    TEST_ASSERT_EQUAL(2, BOXCFG_CORNER_BOTTOM_LEFT);
    TEST_ASSERT_EQUAL(3, BOXCFG_CORNER_BOTTOM_RIGHT);
    TEST_ASSERT_EQUAL_STRING("topLeft", boxcfg_corner_name(BOXCFG_CORNER_TOP_LEFT)); /* Device.GetConfig's words */
    TEST_ASSERT_EQUAL_STRING("topRight", boxcfg_corner_name(BOXCFG_CORNER_TOP_RIGHT));
    TEST_ASSERT_EQUAL_STRING("bottomLeft", boxcfg_corner_name(BOXCFG_CORNER_BOTTOM_LEFT));
    TEST_ASSERT_EQUAL_STRING("bottomRight", boxcfg_corner_name(BOXCFG_CORNER_BOTTOM_RIGHT));
    TEST_ASSERT_EQUAL_STRING("topRight", boxcfg_corner_name(4)); /* not a corner: the default first key */
    TEST_ASSERT_EQUAL_STRING("topRight", boxcfg_corner_name(-1));
    char *s = boxcfg_to_json(&c, false);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"gestureEnabled\":true,\"gestureKeys\":[\"topRight\",\"bottomRight\"],"
                                   "\"gestureTaps\":1,\"gestureHoldSeconds\":2"));
    free(s);
    char err[96], key[32], skipped[160];
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"gestureEnabled\":false,\"gestureKeys\":[\"bottomLeft\",\"topLeft\"],"
                                          "\"gestureTaps\":3,\"gestureHoldSeconds\":5}", err, sizeof err));
    TEST_ASSERT_FALSE(c.gesture_enabled);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_BOTTOM_LEFT, c.gesture_keys[0]);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_TOP_LEFT, c.gesture_keys[1]);
    TEST_ASSERT_EQUAL_UINT8(3, c.gesture_taps);
    TEST_ASSERT_EQUAL_UINT8(5, c.gesture_hold_s);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"gestureHoldSeconds\":3,\"gestureTaps\":2}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(3, c.gesture_hold_s);
    TEST_ASSERT_EQUAL_UINT8(2, c.gesture_taps);
    TEST_ASSERT_TRUE(boxcfg_from_json(&c, "{\"gestureKeys\":[\"topRight\",\"topLeft\"]}", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_TOP_RIGHT, c.gesture_keys[0]);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_TOP_LEFT, c.gesture_keys[1]);
    s = boxcfg_to_json(&c, true); /* stored and read back whole */
    box_config_t r = fresh();
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&r, s, skipped, sizeof skipped));
    TEST_ASSERT_FALSE(r.gesture_enabled);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_TOP_RIGHT, r.gesture_keys[0]);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_TOP_LEFT, r.gesture_keys[1]);
    TEST_ASSERT_EQUAL_UINT8(2, r.gesture_taps);
    TEST_ASSERT_EQUAL_UINT8(3, r.gesture_hold_s);
    free(s);
    static const struct {
        const char *json, *key;
    } BAD[] = {
        {"{\"gestureEnabled\":1}", "gestureEnabled"},
        {"{\"gestureKeys\":\"topRight\"}", "gestureKeys"},
        {"{\"gestureKeys\":[]}", "gestureKeys"},
        {"{\"gestureKeys\":[\"topRight\"]}", "gestureKeys"},
        {"{\"gestureKeys\":[\"topRight\",\"bottomRight\",\"topLeft\"]}", "gestureKeys"},
        {"{\"gestureKeys\":[\"topRight\",\"topRight\"]}", "gestureKeys"},
        {"{\"gestureKeys\":[\"topRight\",\"middle\"]}", "gestureKeys"},
        {"{\"gestureKeys\":[\"TopRight\",\"bottomRight\"]}", "gestureKeys"},
        {"{\"gestureKeys\":[1,3]}", "gestureKeys"},
        {"{\"gestureKeys\":null}", "gestureKeys"},
        {"{\"gestureTaps\":0}", "gestureTaps"},
        {"{\"gestureTaps\":4}", "gestureTaps"},
        {"{\"gestureTaps\":1.5}", "gestureTaps"},
        {"{\"gestureTaps\":\"2\"}", "gestureTaps"},
        {"{\"gestureHoldSeconds\":1}", "gestureHoldSeconds"},
        {"{\"gestureHoldSeconds\":4}", "gestureHoldSeconds"},
        {"{\"gestureHoldSeconds\":6}", "gestureHoldSeconds"},
        {"{\"gestureHoldSeconds\":2.5}", "gestureHoldSeconds"},
        {"{\"gestureHoldSeconds\":\"2\"}", "gestureHoldSeconds"},
        {"{\"gestureHoldSeconds\":true}", "gestureHoldSeconds"},
    };
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        box_config_t b = fresh();
        const box_config_t was = b;
        TEST_ASSERT_FALSE_MESSAGE(boxcfg_from_json(&b, BAD[i].json, err, sizeof err), BAD[i].json);
        boxcfg_err_key(err, key, sizeof key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(BAD[i].key, key, BAD[i].json);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&was, &b, sizeof b, BAD[i].json); /* unchanged */
        box_config_t l = was; /* the stored config: skipped, the default kept */
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, boxcfg_from_json_lenient(&l, BAD[i].json, skipped, sizeof skipped), BAD[i].json);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&was, &l, sizeof l, BAD[i].json);
    }
    box_config_t old = fresh(); /* a stored config of before: the default gesture */
    TEST_ASSERT_EQUAL_INT(0, boxcfg_from_json_lenient(&old, "{\"deckLimit\":4,\"ledEnabled\":true}", NULL, 0));
    TEST_ASSERT_TRUE(old.gesture_enabled);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_TOP_RIGHT, old.gesture_keys[0]);
    TEST_ASSERT_EQUAL_UINT8(BOXCFG_CORNER_BOTTOM_RIGHT, old.gesture_keys[1]);
    TEST_ASSERT_EQUAL_UINT8(1, old.gesture_taps);
    TEST_ASSERT_EQUAL_UINT8(2, old.gesture_hold_s);
}

/* ---- boxcfg_from_cjson on an object = boxcfg_from_json on its print, for the values drawn here (not for a
 * number 1 or 2 ulps from an integer: test_setconfig_near_integers; *.SetConfig keeps the print and parse) ---- */

static uint32_t rng = 0xC2B2AE35u;
static uint32_t rnd(void) /* xorshift32: the same cases every run */
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static const char *const CFG_KEYS[] = {
    "boxName", "companionAuto", "companionHost", "companionPort", "companionPinId", "ethDhcp", "ethIp", "ethMask",
    "ethGw", "ethDns", "wifiSsid", "wifiPass", "wifiBackupSsid", "wifiBackupPass", "wifiEnabled", "apEnabled",
    "apSecured", "apPass", "adminPass", "mdnsEnabled", "surfaceMode", "speakerEnabled", "speakerVolume", "soundReady",
    "soundConnected", "soundProblem", "soundDeckPlug", "soundKeyClick", "ledEnabled", "ledFlash", "ledPin",
    "ledBrightness", "deckBrightness", "deckLimit", "gestureEnabled", "gestureKeys", "gestureTaps", "gestureHoldSeconds",
    "unknownKey", "BOXNAME"};
#define NCFG_KEYS (int)(sizeof CFG_KEYS / sizeof CFG_KEYS[0])

static cJSON *rand_cfg_value(void)
{
    static const char *const S[] = {"", "Box", "192.168.1.20", "255.255.255.0", "dock", "satellite", "12345678",
                                    "a \"q\" \\ \n\t\x01", "caf\xc3\xa9 \xff", "x.local", "1234567"};
    static const double N[] = {0, -0.0, 1, 2, 4, 10, 16622, 65535, 65536, 100, 101, 54, 21, -1, 0.5, 1e300};
    char buf[80];
    switch (rnd() % 8) {
    case 0: return cJSON_CreateString(S[rnd() % (sizeof S / sizeof S[0])]);
    case 1: {
        const size_t n = rnd() % 70;
        for (size_t i = 0; i < n; i++) buf[i] = (char)('a' + rnd() % 26);
        buf[n] = '\0';
        return cJSON_CreateString(buf);
    }
    case 2: return cJSON_CreateNumber(N[rnd() % (sizeof N / sizeof N[0])]);
    case 3: return cJSON_CreateNumber(rnd() % 3 ? (double)(rnd() % 200) : (rnd() % 2 ? INFINITY : -INFINITY));
    case 4: return cJSON_CreateBool(rnd() % 2);
    case 5: return cJSON_CreateNull();
    case 6: return cJSON_CreateArray();
    default: return cJSON_CreateObject();
    }
}

static void test_from_cjson_matches_print_and_parse(void)
{
    int ok_n = 0, bad_n = 0;
    for (int it = 0; it < 20000; it++) {
        cJSON *o = cJSON_CreateObject();
        const int n = (int)(rnd() % 6);
        for (int i = 0; i < n; i++) cJSON_AddItemToObject(o, CFG_KEYS[rnd() % NCFG_KEYS], rand_cfg_value());
        char *json = cJSON_PrintUnformatted(o);
        TEST_ASSERT_NOT_NULL(json);
        box_config_t a = fresh(), b = a;
        char ea[96] = "x", eb[96] = "y";
        const bool ra = boxcfg_from_cjson(&a, o, ea, sizeof ea), rb = boxcfg_from_json(&b, json, eb, sizeof eb);
        TEST_ASSERT_EQUAL(rb, ra);
        TEST_ASSERT_EQUAL_STRING(eb, ea);
        char *ja = boxcfg_to_json(&a, true), *jb = boxcfg_to_json(&b, true);
        TEST_ASSERT_EQUAL_STRING(jb, ja);
        ra ? ok_n++ : bad_n++;
        free(ja);
        free(jb);
        cJSON_free(json);
        cJSON_Delete(o);
    }
    TEST_ASSERT_TRUE(ok_n > 3000 && bad_n > 3000); /* both outcomes well covered */
    box_config_t c = fresh();
    char err[64];
    cJSON *arr = cJSON_CreateArray();
    TEST_ASSERT_FALSE(boxcfg_from_cjson(&c, arr, err, sizeof err)); /* not an object: as from_json's text */
    char ej[64];
    TEST_ASSERT_FALSE(boxcfg_from_json(&c, "[]", ej, sizeof ej));
    TEST_ASSERT_EQUAL_STRING(ej, err);
    TEST_ASSERT_FALSE(boxcfg_from_cjson(&c, NULL, err, sizeof err));
    cJSON_Delete(arr);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_json_partial_update);
    RUN_TEST(test_json_invalid_leaves_unchanged);
    RUN_TEST(test_json_wrong_type_and_syntax);
    RUN_TEST(test_json_string_limits);
    RUN_TEST(test_json_static_ip_validation);
    RUN_TEST(test_to_json_masks_secrets);
    RUN_TEST(test_satellite_config_post);
    RUN_TEST(test_satellite_config_get);
    RUN_TEST(test_err_key);
    RUN_TEST(test_surface_mode);
    RUN_TEST(test_lenient_keeps_valid_fields);
    RUN_TEST(test_lenient_static_ip);
    RUN_TEST(test_lenient_not_json);
    RUN_TEST(test_lenient_round_trip);
    RUN_TEST(test_lenient_led_flash);
    RUN_TEST(test_deck_brightness);
    RUN_TEST(test_deck_limit);
    RUN_TEST(test_strict_still_all_or_nothing);
    RUN_TEST(test_store_decision);
    RUN_TEST(test_feedback_defaults);
    RUN_TEST(test_feedback_json);
    RUN_TEST(test_led_pins);
    RUN_TEST(test_ap_secured_json);
    RUN_TEST(test_ap_pass_validated_only_when_secured);
    RUN_TEST(test_ap_old_stored_config_secured);
    RUN_TEST(test_ap_stored_open_stays_open);
    RUN_TEST(test_ap_stored_secured_bad_pass);
    RUN_TEST(test_ap_lenient_round_trip);
    RUN_TEST(test_backup_defaults_empty);
    RUN_TEST(test_backup_json_round_trip_and_secret);
    RUN_TEST(test_backup_validation);
    RUN_TEST(test_backup_lenient_old_config_without_keys);
    RUN_TEST(test_backup_lenient_with_keys);
    RUN_TEST(test_backup_lenient_bad_password_skipped);
    RUN_TEST(test_backup_lenient_round_trip);
    RUN_TEST(test_wifi_enabled);
    RUN_TEST(test_from_cjson_matches_print_and_parse);
    RUN_TEST(test_companion_adopt);
    RUN_TEST(test_gesture);
    RUN_TEST(test_setconfig_near_integers);
    RUN_TEST(test_same_target);
    RUN_TEST(test_hostname);
    return UNITY_END();
}
