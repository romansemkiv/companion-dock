/* The deck maintenance screens' actions and the settings they
 * show (the surfaces hooks, webui.h). Actions run on a worker of their own, off the surfaces and USB
 * tasks (they write flash, restart Wi-Fi or the box), each through the code path its RPC uses: the RPC
 * itself, called in-process as a trusted caller (rpc_call), or the C call the RPC makes. With a settings
 * PIN set, the locked ones (mode switch, Hotspot off, Restart, Sync now, Boot other) do nothing, even if a
 * race let one through the deck's UI, unless the deck they came from was unlocked with the PIN
 *. The decks' PIN checks run here too, off the surfaces and USB tasks. Each ends with a short notice
 * on the decks. */
#include <stdio.h>
#include <string.h>

#include "boxconfig/config.h"
#include "cJSON.h"
#include "deckui/pin.h"
#include "deckui/screens.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "netmgr/netmgr.h"
#include "rpc/rpc.h"
#include "satclient/satclient.h"
#include "surfaces/surfaces.h"
#include "webui/webui.h"
#include "webui_priv.h"

static const char *TAG = "deckact";

#define DA_STACK 8192 /* internal: config saves write flash; the hotspot blocks while Wi-Fi restarts (as svcbtn's) */
#define DA_PRIO 4
#define DA_DEPTH 4

#define DA_PIN (-1) /* the PIN check: its digits wait in s_pin, never in the queue */

typedef struct {
    int action;
    char arg[64];
    bool unlocked; /* the deck it came from was unlocked with the PIN */
} da_item_t;

static QueueHandle_t s_q;
/* The one PIN waiting for the worker (s_pin_mux): kept out of the queue's storage, wiped once taken. */
static portMUX_TYPE s_pin_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_pin[16];
static bool s_pin_due;
static int s_pin_slot;
static uint32_t s_pin_ticket;
_Static_assert(sizeof s_pin > DUI_PIN_MAX, "s_pin holds the longest PIN and its end");

/* A wipe the compiler keeps (a memset of a buffer about to die may be dropped). */
static void wipe(void *p, size_t n)
{
    volatile char *v = p;
    while (n--) *v++ = 0;
}

/* One RPC, in-process as a trusted caller: params are consumed. The notice says how it went: done in
 * green or, for something now under way (busy), amber; refused in red with its two-word reason (dui_refusal). */
static bool call(const char *method, cJSON *params, const char *done, bool busy)
{
    const rpc_caller_t who = {.src = "deck", .trusted = true};
    rpc_err_t err = {0};
    cJSON *r = rpc_call(method, params, &who, &err);
    cJSON_Delete(params);
    if (!r) {
        ESP_LOGW(TAG, "%s refused: %s", method, err.msg);
        surfaces_toast(SURF_TOAST_REFUSED, dui_refusal(err.code, err.msg));
        return false;
    }
    cJSON_Delete(r);
    if (done) surfaces_toast(busy ? SURF_TOAST_BUSY : SURF_TOAST_DONE, done);
    return true;
}

static cJSON *obj1(const char *key, cJSON *v)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, key, v);
    return o;
}

/* The PIN waiting in s_pin, checked as the client "deck"; the answer to the deck's pad. */
static void check_pin(void)
{
    char pin[sizeof s_pin];
    taskENTER_CRITICAL(&s_pin_mux);
    memcpy(pin, s_pin, sizeof pin);
    for (size_t i = 0; i < sizeof s_pin; i++) ((volatile char *)s_pin)[i] = 0;
    const int slot = s_pin_slot;
    const uint32_t ticket = s_pin_ticket;
    s_pin_due = false;
    taskEXIT_CRITICAL(&s_pin_mux);
    pin[sizeof pin - 1] = '\0';
    int wait = 0;
    const auth_pin_result_t r = auth_check_pin(pin, &wait);
    wipe(pin, sizeof pin);
    const int res = r == AUTH_PIN_OK ? SURF_PIN_OK : r == AUTH_PIN_WRONG ? SURF_PIN_WRONG
                    : r == AUTH_PIN_LOCKED ? SURF_PIN_LOCKED : SURF_PIN_NONE;
    surfaces_pin_result(slot, ticket, res, wait);
}

/* Ethernet static with set v (validated: dui_ipset_valid), through Eth.SetConfig's own path (its
 * validation and apply); the notice "Static <ip>". */
static void eth_static(const dui_ipset_t *v)
{
    if (!dui_ipset_valid(v)) {
        ESP_LOGW(TAG, "deck: static address refused: not a valid set");
        surfaces_toast(SURF_TOAST_REFUSED, "Not valid");
        return;
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddBoolToObject(p, "ethDhcp", false);
    cJSON_AddStringToObject(p, "ethIp", v->ip);
    cJSON_AddStringToObject(p, "ethMask", v->mask);
    cJSON_AddStringToObject(p, "ethGw", v->gw);
    cJSON_AddStringToObject(p, "ethDns", v->dns);
    char done[24];
    snprintf(done, sizeof done, "Static %s", v->ip);
    ESP_LOGW(TAG, "deck: Ethernet static %s/%s gw %s dns %s", v->ip, v->mask, v->gw, v->dns[0] ? v->dns : "-");
    call("Eth.SetConfig", p, done, false);
}

/* the editor's "ip mask gw dns" (dns "-": none) into v; false when it isn't 4 words. */
static bool parse_set(const char *arg, dui_ipset_t *v)
{
    memset(v, 0, sizeof *v);
    char *f[4] = {v->ip, v->mask, v->gw, v->dns};
    const char *p = arg;
    for (int i = 0; i < 4; i++) {
        while (*p == ' ') p++;
        size_t n = 0;
        while (p[n] && p[n] != ' ') n++;
        if (!n || n > 15) return false;
        memcpy(f[i], p, n);
        f[i][n] = '\0';
        p += n;
    }
    if (!strcmp(v->dns, "-")) v->dns[0] = '\0';
    return true;
}

static void run(const da_item_t *it)
{
    const int a = it->action;
    if (a == DA_PIN) {
        check_pin();
        return;
    }
    if (!dui_action_allowed(a, auth_enabled(), it->unlocked)) { /* unless unlocked with the PIN */
        ESP_LOGW(TAG, "deck action %d refused: a settings PIN is set (unlock the deck with it)", a);
        surfaces_toast(SURF_TOAST_REFUSED, "PIN needed");
        return;
    }
    if (a >= DUI_A_PICK0 && a <= DUI_A_PICK5) { /* Companion.Choose */
        ESP_LOGI(TAG, "deck: Companion %s chosen", it->arg);
        call("Companion.Choose", obj1("id", cJSON_CreateString(it->arg)), "Connecting", true);
        return;
    }
    switch (a) {
    case DUI_A_MODE_SAT:
    case DUI_A_MODE_DOCK: /* saved, then the box restarts into it (the RPC's breadcrumb) */
        ESP_LOGW(TAG, "deck: surface mode %s", a == DUI_A_MODE_DOCK ? "dock" : "satellite");
        call("Companion.SetConfig", obj1("mode", cJSON_CreateString(a == DUI_A_MODE_DOCK ? "dock" : "satellite")),
             "Restarting", true);
        break;
    case DUI_A_RETRY: /* offered only while not connected */
        ESP_LOGI(TAG, "deck: retry the Companion connection now");
        satclient_retry();
        surfaces_toast(SURF_TOAST_BUSY, "Retrying");
        break;
    case DUI_A_AP_ON: /* Always on: the setting on and kept, as the web page's (no 15-minute force) */
        ESP_LOGW(TAG, "deck: hotspot always on");
        call("Ap.SetConfig", obj1("apEnabled", cJSON_CreateTrue()), "Hotspot on", false);
        break;
    case DUI_A_AP_OFF: /* the setting off, and any force over */
        ESP_LOGW(TAG, "deck: hotspot off");
        if (call("Ap.SetConfig", obj1("apEnabled", cJSON_CreateFalse()), "Hotspot off", false)) netmgr_ap_force(0);
        break;
    case DUI_A_DIMMER:
    case DUI_A_BRIGHTER: {
        webui_cfg_lock();
        int b = webui_cfg()->deck_brightness + (a == DUI_A_BRIGHTER ? 10 : -10);
        webui_cfg_unlock();
        b = b < 10 ? 10 : b > 100 ? 100 : b;
        call("Device.SetConfig", obj1("deckBrightness", cJSON_CreateNumber(b)), NULL, false);
        break;
    }
    case DUI_A_SOUND: {
        webui_cfg_lock();
        const bool on = !webui_cfg()->speaker_enabled;
        webui_cfg_unlock();
        call("Feedback.SetConfig", obj1("speakerEnabled", cJSON_CreateBool(on)), on ? "Sound on" : "Sound off", false);
        break;
    }
    case DUI_A_FLASH: { /* the status light's flashes */
        webui_cfg_lock();
        const bool on = !webui_cfg()->led_flash;
        webui_cfg_unlock();
        call("Feedback.SetConfig", obj1("ledFlash", cJSON_CreateBool(on)), on ? "Flashes on" : "Flashes off", false);
        break;
    }
    case DUI_A_USE_MAIN:
    case DUI_A_USE_BACKUP: /* the other Wi-Fi network now */
        ESP_LOGW(TAG, "deck: use the %s Wi-Fi network", a == DUI_A_USE_MAIN ? "main" : "backup");
        call("Wifi.Use", obj1("net", cJSON_CreateString(a == DUI_A_USE_MAIN ? "main" : "backup")), "Switching", true);
        break;
    case DUI_A_WIFI_OFF: /* the station off; the setup hotspot keeps its own settings */
    case DUI_A_WIFI_ON:
        ESP_LOGW(TAG, "deck: Wi-Fi %s", a == DUI_A_WIFI_ON ? "on" : "off (the hotspot stays)");
        call("Wifi.SetConfig", obj1("wifiEnabled", cJSON_CreateBool(a == DUI_A_WIFI_ON)),
             a == DUI_A_WIFI_ON ? "Joining Wi-Fi" : "Wi-Fi off", a == DUI_A_WIFI_ON);
        break;
    case DUI_A_ETH_DHCP:
        ESP_LOGW(TAG, "deck: Ethernet by DHCP");
        call("Eth.SetConfig", obj1("ethDhcp", cJSON_CreateTrue()), "DHCP", false);
        break;
    case DUI_A_ETH_STATIC: { /* the saved set when valid, else the lease now, else nothing to use */
        dui_ipset_t saved, lease, v;
        webui_cfg_lock();
        const box_config_t *c = webui_cfg();
        snprintf(saved.ip, sizeof saved.ip, "%s", c->eth_ip);
        snprintf(saved.mask, sizeof saved.mask, "%s", c->eth_mask);
        snprintf(saved.gw, sizeof saved.gw, "%s", c->eth_gw);
        snprintf(saved.dns, sizeof saved.dns, "%s", c->eth_dns);
        webui_cfg_unlock();
        netmgr_eth_lease(lease.ip, lease.mask, lease.gw, lease.dns);
        const int from = dui_eth_static_pick(&saved, &lease, &v);
        if (from == DUI_ETH_FROM_NONE) {
            ESP_LOGW(TAG, "deck: Make static: no saved set and no lease");
            surfaces_toast(SURF_TOAST_REFUSED, "No address");
            break;
        }
        ESP_LOGI(TAG, "deck: Make static from the %s", from == DUI_ETH_FROM_SAVED ? "saved set" : "DHCP lease");
        eth_static(&v);
        break;
    }
    case DUI_A_ETH_SAVE: { /* the editor's set */
        dui_ipset_t v;
        if (!parse_set(it->arg, &v)) {
            ESP_LOGW(TAG, "deck: static address from the editor not understood");
            surfaces_toast(SURF_TOAST_REFUSED, "Not valid");
            break;
        }
        eth_static(&v);
        break;
    }
    case DUI_A_COMP_SAVE: { /* the Companion's IP from the deck, through Companion.SetConfig itself (its
                             * checks, the save, the reconnect, the pinned instance as for any manual host) */
        if (!dui_ipv4_parse(it->arg, NULL)) {
            ESP_LOGW(TAG, "deck: Companion IP from the editor not understood");
            surfaces_toast(SURF_TOAST_REFUSED, "Not valid");
            break;
        }
        ESP_LOGW(TAG, "deck: Companion IP %s (manual)", it->arg);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(p, "companionAuto", false);
        cJSON_AddStringToObject(p, "companionHost", it->arg);
        call("Companion.SetConfig", p, "Saved", false);
        break;
    }
    case DUI_A_COMP_AUTO: /* auto-discovery again */
        ESP_LOGW(TAG, "deck: Companion found automatically again");
        call("Companion.SetConfig", obj1("companionAuto", cJSON_CreateTrue()), "Auto find on", false);
        break;
    case DUI_A_RESTART:
        ESP_LOGW(TAG, "deck: restart");
        call("Device.Restart", cJSON_CreateObject(), "Restarting", true);
        break;
    case DUI_A_SYNC: /* Ota.Sync without force: never over a newer image */
        ESP_LOGI(TAG, "deck: sync the recovery slot now");
        call("Ota.Sync", cJSON_CreateObject(), "Syncing", true);
        break;
    case DUI_A_BOOT_OTHER:
        ESP_LOGW(TAG, "deck: boot the other slot");
        call("Ota.BootOther", cJSON_CreateObject(), "Restarting", true);
        break;
    default: ESP_LOGW(TAG, "deck action %d unknown", a); break;
    }
}

static void worker(void *arg)
{
    (void)arg;
    da_item_t it;
    UBaseType_t least = DA_STACK; /* the smallest stack margin logged so far */
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) continue;
        run(&it);
        const UBaseType_t left = uxTaskGetStackHighWaterMark(NULL);
        if (left < least) { /* the first action, then whenever one went deeper */
            least = left;
            ESP_LOGI(TAG, "deck action %d done: stack never used %u bytes", it.action, (unsigned)left);
        }
    }
}

esp_err_t deckact_start(void)
{
    if (s_q) return ESP_OK;
    s_q = xQueueCreate(DA_DEPTH, sizeof(da_item_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "deckact", DA_STACK, NULL, DA_PRIO, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void webui_deck_action(int action, const char *arg, bool unlocked)
{
    if (!s_q) return;
    da_item_t it = {.action = action, .unlocked = unlocked};
    snprintf(it.arg, sizeof it.arg, "%s", arg ? arg : "");
    if (xQueueSend(s_q, &it, 0) != pdTRUE) {
        ESP_LOGW(TAG, "deck action %d dropped: one is still running", action);
        surfaces_toast(SURF_TOAST_REFUSED, "Still busy");
    }
}

bool webui_deck_pin_check(int slot, uint32_t ticket, const char *pin)
{
    /* the digits measured and copied before the lock; under it only a fixed-size copy (no libc
     * formatting inside the spinlock section) */
    char tmp[sizeof s_pin] = {0};
    size_t n = 0;
    if (pin)
        while (n < DUI_PIN_MAX && pin[n]) n++;
    if (n) memcpy(tmp, pin, n);
    bool busy;
    taskENTER_CRITICAL(&s_pin_mux);
    busy = !s_q || s_pin_due;
    if (!busy) {
        memcpy(s_pin, tmp, sizeof s_pin);
        s_pin_slot = slot;
        s_pin_ticket = ticket;
        s_pin_due = true;
    }
    taskEXIT_CRITICAL(&s_pin_mux);
    wipe(tmp, sizeof tmp);
    if (busy) return false; /* answered by the caller at once, never posted back from its task */
    const da_item_t it = {.action = DA_PIN};
    if (xQueueSend(s_q, &it, 0) != pdTRUE) { /* the worker is busy with actions: try again in a moment */
        taskENTER_CRITICAL(&s_pin_mux);
        for (size_t i = 0; i < sizeof s_pin; i++) ((volatile char *)s_pin)[i] = 0;
        s_pin_due = false;
        taskEXIT_CRITICAL(&s_pin_mux);
        return false;
    }
    return true;
}

int webui_deck_pin_wait_s(void) { return auth_deck_wait_s(); }

_Static_assert((int)BOXCFG_CORNER_TOP_LEFT == (int)CORNER_TOP_LEFT && (int)BOXCFG_CORNER_TOP_RIGHT == (int)CORNER_TOP_RIGHT &&
                   (int)BOXCFG_CORNER_BOTTOM_LEFT == (int)CORNER_BOTTOM_LEFT &&
                   (int)BOXCFG_CORNER_BOTTOM_RIGHT == (int)CORNER_BOTTOM_RIGHT,
               "gestureKeys: boxconfig's corners in deckproto's order");

void webui_deck_push_settings(void)
{
    box_config_t *c = webui_cfg();
    if (!c) return;
    surf_settings_t st;
    memset(&st, 0, sizeof st);
    webui_cfg_lock();
    snprintf(st.box_name, sizeof st.box_name, "%s", c->box_name);
    snprintf(st.wifi_main, sizeof st.wifi_main, "%s", c->wifi_ssid);
    snprintf(st.wifi_backup, sizeof st.wifi_backup, "%s", c->wifi_backup_ssid);
    st.brightness = c->deck_brightness;
    st.sound = c->speaker_enabled;
    st.led_flash = c->led_flash;
    st.companion_auto = c->companion_auto;
    snprintf(st.companion_host, sizeof st.companion_host, "%s", c->companion_host);
    st.companion_port = c->companion_port;
    st.eth_dhcp = c->eth_dhcp;
    snprintf(st.eth_ip, sizeof st.eth_ip, "%s", c->eth_ip);
    snprintf(st.eth_mask, sizeof st.eth_mask, "%s", c->eth_mask);
    snprintf(st.eth_gw, sizeof st.eth_gw, "%s", c->eth_gw);
    snprintf(st.eth_dns, sizeof st.eth_dns, "%s", c->eth_dns);
    st.gesture = (corner_cfg_t){.on = c->gesture_enabled, .first = c->gesture_keys[0], .second = c->gesture_keys[1],
                                .taps = c->gesture_taps, .hold_ms = (uint16_t)(c->gesture_hold_s * 1000u)};
    webui_cfg_unlock();
    st.locked = auth_enabled();
    st.pin = auth_pin_set(); /* the decks can be unlocked (a text password of before: not) */
    surfaces_set_settings(&st);
}
