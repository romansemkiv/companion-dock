/* The box's RPC methods (Auth.* live in auth.c), the status they share, and config changes.
 *
 * Config changes: a *.SetConfig takes the config lock, copies the config, lays the
 * params over the copy with boxcfg_from_json (dry_run stops here), saves the copy and writes it back,
 * unlocks, and only then signals on_config_changed: the supervisor applies network and discovery
 * settings itself. The lock is never held across network I/O, Wi-Fi calls or a wait on the
 * supervisor (the speaker and light settings too: feedback_apply). The one setting that needs a
 * restart is the surface mode (Companion.SetConfig "mode"):
 * when the saved mode differs from the one this run uses, the reply says so and the box
 * restarts a second later (as Device.Restart).
 *
 * Secrets: Ap.GetConfig returns the hotspot password, because the page shows it (the
 * decks and the serial console show it too), behind the settings password when one is set.
 * The Wi-Fi station passwords (main and backup) and the settings password's verifier never leave the
 * box: Wifi.GetConfig reports only whether each is stored.
 *
 * Key style: config keys are boxconfig's camelCase names (the same in GetConfig and SetConfig), but
 * for Companion's "mode", which is boxconfig's "surfaceMode" (the stored config and the seed tool keep
 * that name); status and result keys are snake_case. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boxconfig/config.h"
#include "breadcrumb/breadcrumb.h"
#include "c6fw/c6fw.h"
#include "discovery/discovery.h"
#include "elgdock/elgdock.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "feedback/feedback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netmgr/netmgr.h"
#include "rpc/refusals.h"
#include "rpc/rpc.h"
#include "satclient/satclient.h"
#include "slotsync/slotsync.h"
#include "notify_logic.h"
#include "surfaces/surfaces.h"
#include "usbdeck/usbdeck.h"
#include "web_logic.h"
#include "webui/webui.h"
#include "webui_priv.h"

static const char *TAG = "webui";

#define MAX_INSTANCES 8
#define MAX_DECKS SURF_MAX_DECKS
#define MAX_PROBLEMS 8
#define LOG_CHUNK 8192       /* Log.Get: bytes per call */
#define RESTART_DELAY_MS 1000
#define RESTART_STACK 8192   /* internal: the factory reset erases flash; esp_restart and its log
                              * lines run on it */

static box_config_t *s_cfg; /* webui_cfg_lock */
static box_surface_mode_t s_mode; /* this run's surface mode (main read it at boot) */
static int s_deck_limit;          /* this run's deck limit (deckLimit as main read it at boot) */
static void (*s_changed)(void);
static void (*s_lock_fn)(void);
static void (*s_unlock_fn)(void);
static atomic_bool s_restarting;
static const char *s_restart_why; /* set once s_restarting is taken: the breadcrumb's reason */

void webui_set_cfg_lock(void (*lock)(void), void (*unlock)(void))
{
    s_lock_fn = lock;
    s_unlock_fn = unlock;
}

void webui_cfg_lock(void) { s_lock_fn(); }
void webui_cfg_unlock(void) { s_unlock_fn(); }
box_config_t *webui_cfg(void) { return s_cfg; }

static void signal_changed(void)
{
    if (s_changed) s_changed();
}

static atomic_uint s_cfg_rev; /* Sys.GetStatus.rev.cfg: the settings saves since the start */

void webui_settings_saved(void)
{
    atomic_fetch_add(&s_cfg_rev, 1u);
    notify_soon();
}

/* {"restart_required": false[, "dry_run": true]}: settings apply without a restart. */
static cJSON *cfg_result(bool dry)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "restart_required", false);
    if (dry) cJSON_AddBoolToObject(r, "dry_run", true);
    return r;
}

/* Lays src (what it is: the function's) over c; false (err set, c unchanged) when refused. */
typedef bool (*cfg_apply_fn)(box_config_t *c, const void *src, char *err, size_t n);

/* a parsed object (no print and parse again: Companion.Choose), the config's JSON text (*.SetConfig: printed and
 * parsed again: a number 1 or 2 ulps from an integer prints as the integer, and is taken as it), and
 * the Satellite API's JSON text. */
static bool apply_cjson(box_config_t *c, const void *src, char *err, size_t n) { return boxcfg_from_cjson(c, src, err, n); }
static bool apply_json_text(box_config_t *c, const void *src, char *err, size_t n) { return boxcfg_from_json(c, src, err, n); }
static bool apply_satellite_text(box_config_t *c, const void *src, char *err, size_t n)
{
    return boxcfg_apply_satellite_config(c, src, err, n);
}
/* Runs on the changed copy, under the lock, after fn accepted the change and before it is saved: CFG_HOOK_APPLY (save
 * it), CFG_HOOK_SKIP (accepted, but nothing to write: no save, no signal) or CFG_HOOK_REFUSE (refused: msg "limit"). */
enum { CFG_HOOK_APPLY, CFG_HOOK_SKIP, CFG_HOOK_REFUSE };
typedef int (*cfg_hook_fn)(box_config_t *c, void *ctx);

/* The config change sequence above. false when refused (msg = what was wrong, *save_err ESP_OK) or
 * when saving failed (*save_err set); the config is then unchanged. */
static bool apply_cfg_hook(cfg_apply_fn fn, cfg_hook_fn hook, void *hook_ctx, const void *src, bool dry,
                           char *msg, size_t n, esp_err_t *save_err)
{
    *save_err = ESP_OK;
    box_config_t *t = malloc(sizeof *t);
    if (!t) {
        snprintf(msg, n, "no memory");
        *save_err = ESP_ERR_NO_MEM;
        return false;
    }
    webui_cfg_lock();
    *t = *s_cfg;
    bool ok = fn(t, src, msg, n);
    const int hk = ok && hook ? hook(t, hook_ctx) : CFG_HOOK_APPLY;
    if (hk == CFG_HOOK_REFUSE) {
        snprintf(msg, n, "limit: too many changes");
        ok = false;
    }
    const bool write = ok && !dry && hk == CFG_HOOK_APPLY;
    if (write) {
        *save_err = boxcfg_save(t);
        if (*save_err == ESP_OK) *s_cfg = *t;
    }
    webui_cfg_unlock();
    free(t);
    if (write && *save_err != ESP_OK) {
        snprintf(msg, n, "saving the settings failed: %s", esp_err_to_name(*save_err));
        return false;
    }
    if (write) {
        webui_settings_saved(); /* the open pages read it again */
        webui_deck_push_settings(); /* the decks' screens show some of it */
        signal_changed();
    }
    return ok;
}

static bool apply_cfg(cfg_apply_fn fn, const void *src, bool dry, char *msg, size_t n, esp_err_t *save_err)
{
    return apply_cfg_hook(fn, NULL, NULL, src, dry, msg, n, save_err);
}

static companion_instance_t *instances(int *n);

static bool in_list(const char *const *list, const char *key)
{
    for (; list && *list; list++)
        if (!strcmp(*list, key)) return true;
    return false;
}

/* *.SetConfig: only the section's keys (and dry_run) are taken; the read-only keys its GetConfig
 * returns are ignored, anything else is refused. */
static cJSON *set_section(const cJSON *p, const char *const *keys, const char *const *ro, rpc_err_t *err)
{
    const cJSON *dr = cJSON_GetObjectItemCaseSensitive(p, "dry_run");
    if (dr && !cJSON_IsBool(dr)) /* {"dry_run":1} must not apply for real */
        return rpc_fail(err, RPC_E_INVALID_ARG, "dry_run must be true or false");
    const bool dry = cJSON_IsTrue(dr);
    cJSON *f = cJSON_CreateObject();
    const cJSON *it;
    cJSON_ArrayForEach(it, p) {
        if (!strcmp(it->string, "dry_run") || in_list(ro, it->string)) continue;
        if (!in_list(keys, it->string)) {
            cJSON_Delete(f);
            return rpc_fail(err, RPC_E_INVALID_ARG, "unknown key %s", it->string);
        }
        cJSON_AddItemToObject(f, it->string, cJSON_Duplicate(it, true));
    }
    char *json = cJSON_PrintUnformatted(f); /* printed and parsed again: see apply_json_text */
    cJSON_Delete(f);
    if (!json) return rpc_fail(err, RPC_E_UNAVAILABLE, RPC_MSG_NO_MEMORY);
    char msg[96];
    esp_err_t se;
    const bool ok = apply_cfg(apply_json_text, json, dry, msg, sizeof msg, &se);
    cJSON_free(json);
    if (!ok) return rpc_fail(err, se == ESP_OK ? RPC_E_INVALID_ARG : RPC_E_UNAVAILABLE, "%s", msg);
    return cfg_result(dry);
}

/* Companion's Discover -> setup: a posted host that a visible
 * _companion-satellite-tcp instance has (and its port, when one was posted) pins that instance, so
 * the box follows it when its address changes; any other host is a manual address. */
typedef struct {
    const companion_instance_t *inst;
    int n;
    bool host, port; /* posted */
    bool pinned;
    char id[sizeof s_cfg->companion_pin_id];
    bool anon;          /* it came without the PIN (companionAdopt) */
    wg_adopt_t step;    /* what the limiter made of it (anon) */
    bool log_refusal;   /* ... refused, and logged this time (at most once a minute) */
    char to_host[sizeof s_cfg->companion_host];
    uint16_t to_port;
} posted_t;

static wg_bucket_t s_adopt; /* the unauthenticated adopt writes' budget (config lock) */

static int pin_posted(box_config_t *t, void *ctx)
{
    posted_t *p = ctx;
    if (p->host) {
        p->pinned = discovery_pin_posted(t, p->inst, p->n, t->companion_host, p->port ? t->companion_port : 0);
        if (p->pinned) strlcpy(p->id, t->companion_pin_id, sizeof p->id);
    }
    strlcpy(p->to_host, t->companion_host, sizeof p->to_host);
    p->to_port = t->companion_port;
    if (p->anon) { /* the same target writes nothing; a change spends a token, and none left refuses it */
        /* the whole effective target after the step (auto, pin, host, port), against the config before
         * (under the config lock, before the save: s_cfg is still it) */
        const bool moved = !boxcfg_same_target(t, s_cfg);
        const int64_t now = esp_timer_get_time() / 1000;
        p->step = wg_adopt_step(&s_adopt, moved, now);
        if (p->step == WG_ADOPT_QUIET) return CFG_HOOK_SKIP;
        if (p->step == WG_ADOPT_LIMIT) {
            p->log_refusal = wg_adopt_log_due(&s_adopt, now);
            return CFG_HOOK_REFUSE;
        }
    }
    return CFG_HOOK_APPLY;
}

bool webui_apply_satellite(const char *json, bool has_host, bool has_port, const char *adopted_by, char *err, size_t n)
{
    char msg[96];
    esp_err_t se;
    posted_t ps = {0};
    ps.host = has_host; /* the caller had the object: no parse here only to see these */
    ps.port = has_port;
    ps.anon = adopted_by != NULL;
    companion_instance_t *inst = ps.host ? instances(&ps.n) : NULL; /* before the config lock */
    ps.inst = inst;
    const bool ok = apply_cfg_hook(apply_satellite_text, pin_posted, &ps, json, false, msg, sizeof msg, &se);
    free(inst);
    if (ok && ps.host && !(ps.anon && ps.step == WG_ADOPT_QUIET)) {
        if (ps.pinned) ESP_LOGI(TAG, "Discover -> setup: pinned to Companion %s", ps.id);
        else ESP_LOGI(TAG, "Companion address set by hand (no Companion seen by mDNS there)");
    }
    if (ok && ps.anon && ps.step == WG_ADOPT_APPLY) { /* without the PIN; a request that changes nothing: quiet */
        ESP_LOGI(TAG, "adopted by %s: Companion %s:%u", adopted_by, ps.to_host, (unsigned)ps.to_port);
        surfaces_toast(SURF_TOAST_DONE, "Adopted");
    }
    if (!ok && ps.anon && ps.step == WG_ADOPT_LIMIT && ps.log_refusal)
        ESP_LOGW(TAG, "adopt by %s refused: more than %d changes in a row, one more a minute (429)", adopted_by,
                 WG_ADOPT_BURST);
    if (ok) return true;
    if (se != ESP_OK) {
        snprintf(err, n, "save");
        ESP_LOGE(TAG, "%s", msg);
        return false;
    }
    boxcfg_err_key(msg, err, n);
    return false;
}

/* ---- restart and factory reset ---- */

static void restart_task(void *arg)
{
    const bool reset = (intptr_t)arg != 0;
    const char *why = s_restart_why;
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS)); /* the reply goes out first */
    if (reset) {
        /* held until the restart: no config or password save can land after the erase */
        webui_cfg_lock();
        const esp_err_t e = boxcfg_factory_reset();
        if (e != ESP_OK) ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(e));
        else ESP_LOGW(TAG, "factory reset: settings erased");
    }
    ESP_LOGW(TAG, "restarting (%s)", why);
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the log line out */
    breadcrumb_restart(why);
}

esp_err_t webui_restart_later(bool factory_reset, const char *why)
{
    if (atomic_exchange(&s_restarting, true)) return ESP_ERR_INVALID_STATE;
    s_restart_why = why;
    /* before the task exists, so its "restarting" record always comes after this one; a crash
     * meanwhile shows the restart as pending */
    breadcrumb_scheduled(why);
    if (xTaskCreate(restart_task, "restart", RESTART_STACK, (void *)(intptr_t)factory_reset, 6, NULL) != pdPASS) {
        breadcrumb_clear();
        atomic_store(&s_restarting, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- status pieces ---- */

static const char *route_name(net_route_t r)
{
    return r == NET_ROUTE_ETH ? "eth" : r == NET_ROUTE_WIFI ? "wifi" : "none";
}

static const char *satc_name(satc_state_t s)
{
    switch (s) {
    case SATC_CONNECTING: return "connecting";
    case SATC_CONNECTED: return "connected";
    case SATC_TOO_OLD: return "too_old";
    default: return "idle";
    }
}

static const char *sel_name(sel_result_t s)
{
    return s == SEL_CONNECT ? "connect" : s == SEL_AMBIGUOUS ? "ambiguous" : "none";
}

#define MODE_SATELLITE "satellite" /* boxconfig's surfaceMode values */
#define MODE_DOCK "dock"

static const char *mode_name(box_surface_mode_t m) { return m == BOX_SURFACE_DOCK ? MODE_DOCK : MODE_SATELLITE; }

/* The Network Docks (dock mode; [] in Satellite mode, where elgdock never starts):
 * [{dock, port, listening, model, serial, companion, peer}]. */
static cJSON *docks_json(void)
{
    cJSON *a = cJSON_CreateArray();
    elgdock_status_t *st = calloc(ELGDOCK_MAX, sizeof *st);
    const int n = st ? elgdock_status(st, ELGDOCK_MAX) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "dock", st[i].dock);
        cJSON_AddNumberToObject(o, "port", st[i].port);
        cJSON_AddBoolToObject(o, "listening", st[i].listening);
        cJSON_AddStringToObject(o, "model", st[i].model);
        cJSON_AddStringToObject(o, "serial", st[i].serial);
        cJSON_AddBoolToObject(o, "companion", st[i].companion);
        cJSON_AddStringToObject(o, "peer", st[i].peer);
        cJSON_AddItemToArray(a, o);
    }
    free(st);
    return a;
}

/* discovery_instances into a heap array (caller frees); *n = count. */
static companion_instance_t *instances(int *n)
{
    companion_instance_t *a = calloc(MAX_INSTANCES, sizeof *a);
    *n = a ? discovery_instances(a, MAX_INSTANCES) : 0;
    return a;
}

cJSON *webui_companion_status(void)
{
    const satc_state_t st = satclient_state();
    sat_caps_t caps;
    satclient_caps(&caps);
    char host[64];
    uint16_t port = 0;
    satclient_get_target(host, sizeof host, &port);
    int n = 0;
    companion_instance_t *inst = instances(&n);

    char h[64], pin[64];
    uint16_t p = 0;
    webui_cfg_lock();
    const bool autosel = s_cfg->companion_auto;
    char pinned[sizeof s_cfg->companion_pin_id];
    strlcpy(pinned, s_cfg->companion_pin_id, sizeof pinned);
    const sel_result_t sel = discovery_select(s_cfg, inst, n, h, sizeof h, &p, pin, sizeof pin); /* pure */
    webui_cfg_unlock();
    free(inst);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "state", satc_name(st));
    const bool known = st == SATC_CONNECTED || st == SATC_TOO_OLD;
    cJSON_AddStringToObject(r, "version", known ? caps.companion_version : "");
    char api[24] = "";
    if (known) snprintf(api, sizeof api, "%d.%d.%d", caps.major, caps.minor, caps.patch);
    cJSON_AddStringToObject(r, "api_version", api);
    cJSON_AddStringToObject(r, "host", host);
    cJSON_AddNumberToObject(r, "port", port);
    cJSON_AddBoolToObject(r, "auto", autosel);
    cJSON_AddStringToObject(r, "pinned_id", pinned);
    cJSON_AddStringToObject(r, "selection", sel_name(sel));
    cJSON_AddNumberToObject(r, "instances", n);
    return r;
}

cJSON *webui_decks(void)
{
    cJSON *a = cJSON_CreateArray();
    usbdeck_info_t *d = calloc(MAX_DECKS, sizeof *d);
    const int n = d ? usbdeck_list(d, MAX_DECKS) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char id[48];
        snprintf(id, sizeof id, "streamdeck:%s", d[i].serial);
        cJSON_AddStringToObject(o, "device_id", id);
        cJSON_AddStringToObject(o, "model", d[i].model);
        cJSON_AddStringToObject(o, "serial", d[i].serial);
        cJSON_AddStringToObject(o, "fw", d[i].fw);
        cJSON_AddBoolToObject(o, "high_speed", d[i].high_speed);
        cJSON_AddNumberToObject(o, "hub_depth", d[i].hub_depth);
        cJSON_AddNumberToObject(o, "port", d[i].port);
        bool screen;
        cJSON_AddBoolToObject(o, "menu", surfaces_deck_menu(d[i].serial, &screen)); /* the box menu open on it */
        cJSON_AddBoolToObject(o, "screen", screen);                                 /* ... and whether it can be */
        cJSON_AddBoolToObject(o, "ignored", surfaces_deck_ignored(d[i].serial));     /* over the deck limit */
        cJSON_AddItemToArray(a, o);
    }
    free(d);
    return a;
}

/* "main" / "backup" / "none": the Wi-Fi network joined (netmgr_status_t.sta_net). */
static const char *sta_net_name(uint8_t n)
{
    return n == NETMGR_STA_MAIN ? "main" : n == NETMGR_STA_BACKUP ? "backup" : "none";
}

/* Sys.GetStatus.net. eth_addr / sta_addr, each connection's own address (ip: the route's); eth_dhcp, Ethernet's
 * addressing as the settings say; ap_ip, the hotspot's while it is on (the web page's network signs); wifi_set, a Wi-Fi
 * network is set up (the sign is grey, not red, without one while another connection routes, as the deck's).
 * radio_down (the Wi-Fi chip doesn't answer) and sta_not_found (not joined, and the network being tried wasn't
 * found on its last attempt), for the Wi-Fi card's state. */
static cJSON *net_json(const netmgr_status_t *ns, bool eth_dhcp, bool wifi_set)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "route", route_name(ns->route));
    cJSON_AddStringToObject(o, "ip", ns->ip);
    cJSON_AddStringToObject(o, "eth_addr", ns->eth_addr);
    cJSON_AddStringToObject(o, "sta_addr", ns->sta_addr);
    cJSON_AddBoolToObject(o, "eth_link", ns->eth_link);
    cJSON_AddBoolToObject(o, "eth_dhcp", eth_dhcp);
    cJSON_AddBoolToObject(o, "wifi_connected", ns->wifi_connected);
    cJSON_AddBoolToObject(o, "wifi_enabled", ns->wifi_enabled);
    cJSON_AddBoolToObject(o, "wifi_set", wifi_set);
    cJSON_AddBoolToObject(o, "radio_down", ns->radio_down); /* the Wi-Fi card's state */
    cJSON_AddBoolToObject(o, "sta_not_found", /* ... the network being tried wasn't found on its last attempt */
                          ns->sta_trying != NETMGR_STA_NONE && (ns->sta_not_found & (1u << (ns->sta_trying - 1))));
    cJSON_AddStringToObject(o, "sta_net", sta_net_name(ns->sta_net));
    cJSON_AddStringToObject(o, "sta_ssid", ns->sta_ssid);
    cJSON_AddNumberToObject(o, "sta_rssi", ns->sta_rssi);
    cJSON_AddBoolToObject(o, "ap_on", ns->ap_on);
    cJSON_AddBoolToObject(o, "ap_forced", ns->ap_forced);
    cJSON_AddStringToObject(o, "ap_ssid", ns->ap_ssid);
    cJSON_AddStringToObject(o, "ap_ip", ns->ap_ip);
    cJSON_AddNumberToObject(o, "ap_clients", ns->ap_clients);
    return o;
}

static cJSON *message_json(void)
{
    char text[SURF_MSG_MAX + 1];
    uint32_t left = 0;
    const bool on = surfaces_message_active(text, sizeof text, &left);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "active", on);
    cJSON_AddStringToObject(o, "text", on ? text : "");
    cJSON_AddNumberToObject(o, "remaining_s", on ? left : 0);
    return o;
}

bool webui_pending_verify(void)
{
    /* within a run the running image goes from pending to confirmed (or stays), never back (only the
     * bootloader sets pending, at start): once otadata reads anything else, it isn't read (mapped) again */
    static atomic_bool s_confirmed;
    if (atomic_load(&s_confirmed)) return false;
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return false; /* not cached: read again next time */
    if (st != ESP_OTA_IMG_PENDING_VERIFY) atomic_store(&s_confirmed, true);
    return st == ESP_OTA_IMG_PENDING_VERIFY;
}

static cJSON *firmware_json(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "project", d->project_name);
    cJSON_AddStringToObject(o, "version", d->version);
    cJSON_AddStringToObject(o, "idf", d->idf_ver);
    char b[40];
    snprintf(b, sizeof b, "%s %s", d->date, d->time);
    cJSON_AddStringToObject(o, "build", b);
    cJSON_AddStringToObject(o, "partition", run ? run->label : "");
    cJSON_AddBoolToObject(o, "pending_verify", webui_pending_verify());
    return o;
}

/* Sys.GetStatus.rev.found: the Companions Companion.List gives, and the one pinned (its "pinned" flags). */
static uint32_t found_rev(void)
{
    char pin[sizeof s_cfg->companion_pin_id];
    webui_cfg_lock();
    strlcpy(pin, s_cfg->companion_pin_id, sizeof pin);
    webui_cfg_unlock();
    uint32_t h = notify_fnv1a(NOTIFY_FNV_SEED, pin, strlen(pin) + 1);
    int n = 0;
    companion_instance_t *inst = instances(&n);
    for (int i = 0; i < n; i++) {
        h = notify_fnv1a(h, inst[i].id, strlen(inst[i].id) + 1);
        h = notify_fnv1a(h, inst[i].host, strlen(inst[i].host) + 1);
        h = notify_fnv1a(h, &inst[i].port, sizeof inst[i].port);
        h = notify_fnv1a(h, inst[i].version, strlen(inst[i].version) + 1);
    }
    free(inst);
    return h;
}

/* Sys.GetStatus.rev.slots: what Ota.GetStatus says about the slots (the sync, the other slot, pending verify). */
static uint32_t slots_rev(void)
{
    slotsync_status_t s;
    slotsync_get(&s);
    const int32_t num[] = {(int32_t)s.state, s.pct, s.other_foreign, s.other_valid, s.flash_busy, webui_pending_verify()};
    uint32_t h = notify_fnv1a(NOTIFY_FNV_SEED, num, sizeof num);
    const char *const str[] = {s.running, s.other, s.other_project, s.other_version, s.other_date, s.other_time};
    for (size_t i = 0; i < sizeof str / sizeof str[0]; i++) h = notify_fnv1a(h, str[i], strlen(str[i]) + 1);
    return h;
}

/* Sys.GetStatus -> {name, uptime_s, mode, net, companion, dock, decks, deckLimit, decksIgnored, problems, message,
 * maintenance, firmware, c6, heap, lastReset, rev}. rev: what an open page reads again when it changes: cfg, the
 * settings saves since the start (any *.SetConfig, Companion.Choose, the settings PIN; 0 after a restart), found and
 * slots, fingerprints of Companion.List and of Ota.GetStatus's slots. deckLimit: this run's; decksIgnored: the decks attached over it
 * (each one also in decks with ignored true, until it is unplugged). maintenance: the decks' maintenance screen is on demand. mode: this run's surface mode; dock: the Network Docks (docks_json); lastReset:
 * this start's reset reason with the last deliberate restart's, as one line (breadcrumb_summary:
 * "software restart: mode switch", "panic, last deliberate restart: mode switch pending"). */
box_surface_mode_t webui_run_mode(void) { return s_mode; }

static cJSON *m_sys_status(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    cJSON *r = cJSON_CreateObject();
    char name[sizeof s_cfg->box_name];
    webui_cfg_lock();
    strlcpy(name, s_cfg->box_name, sizeof name);
    const bool eth_dhcp = s_cfg->eth_dhcp, wifi_set = s_cfg->wifi_ssid[0] != '\0';
    webui_cfg_unlock();
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddNumberToObject(r, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(r, "mode", mode_name(s_mode));
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    cJSON_AddItemToObject(r, "net", net_json(&ns, eth_dhcp, wifi_set));
    cJSON_AddItemToObject(r, "companion", webui_companion_status());
    cJSON_AddItemToObject(r, "dock", docks_json());
    cJSON_AddItemToObject(r, "decks", webui_decks());
    cJSON_AddNumberToObject(r, "deckLimit", s_deck_limit); /* this run's, as main gave surfaces */
    cJSON_AddNumberToObject(r, "decksIgnored", surfaces_decks_ignored());
    char (*pr)[96] = calloc(MAX_PROBLEMS, sizeof *pr);
    const int np = pr ? surfaces_problem_list(pr, MAX_PROBLEMS) : 0;
    cJSON *pa = cJSON_AddArrayToObject(r, "problems");
    for (int i = 0; i < np; i++) cJSON_AddItemToArray(pa, cJSON_CreateString(pr[i]));
    free(pr);
    cJSON_AddItemToObject(r, "message", message_json());
    cJSON_AddBoolToObject(r, "maintenance", surfaces_maint_on()); /* the screen on demand */
    char md_serial[33], md_model[33]; /* the deck it is on, or would open on */
    if (surfaces_maint_deck(md_serial, md_model)) {
        cJSON *md = cJSON_AddObjectToObject(r, "maintenanceDeck");
        cJSON_AddStringToObject(md, "serial", md_serial);
        cJSON_AddStringToObject(md, "model", md_model);
    } else {
        cJSON_AddNullToObject(r, "maintenanceDeck");
    }
    cJSON_AddItemToObject(r, "firmware", firmware_json());
    char run[24], carried[24];
    c6fw_get_versions(run, sizeof run, carried, sizeof carried);
    cJSON *c6 = cJSON_AddObjectToObject(r, "c6");
    cJSON_AddStringToObject(c6, "running", run);
    cJSON_AddStringToObject(c6, "carried", carried);
    cJSON *heap = cJSON_AddObjectToObject(r, "heap");
    cJSON_AddNumberToObject(heap, "internal_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(r, "lastReset", breadcrumb_summary());
    cJSON *rev = cJSON_AddObjectToObject(r, "rev");
    cJSON_AddNumberToObject(rev, "cfg", (double)atomic_load(&s_cfg_rev));
    cJSON_AddNumberToObject(rev, "found", (double)found_rev());
    cJSON_AddNumberToObject(rev, "slots", (double)slots_rev());
    return r;
}

/* ---- Device.* ---- */

/* Device.GetInfo -> {name, id, mac, model, hw, project, version, idf, build, auth_en, lastReset}.
 * lastReset: {reason: this start's reset reason ("power-on", "software restart", "panic", ...),
 * breadcrumb: what the last deliberate restart recorded ("mode switch", "OTA", ...; "" when none was
 * under way), summary: the line Sys.GetStatus.lastReset gives}. */
static cJSON *m_device_info(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    const esp_app_desc_t *d = esp_app_get_description();
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_ETH);
    char mac[18], b[40], name[sizeof s_cfg->box_name];
    snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    webui_cfg_lock();
    strlcpy(name, s_cfg->box_name, sizeof name);
    webui_cfg_unlock();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddStringToObject(r, "id", rpc_device_id());
    cJSON_AddStringToObject(r, "mac", mac);
    cJSON_AddStringToObject(r, "model", "Companion Box");
    cJSON_AddStringToObject(r, "hw", "Waveshare ESP32-P4-WIFI6-POE-ETH");
    cJSON_AddStringToObject(r, "project", d->project_name);
    cJSON_AddStringToObject(r, "version", d->version);
    cJSON_AddStringToObject(r, "idf", d->idf_ver);
    snprintf(b, sizeof b, "%s %s", d->date, d->time);
    cJSON_AddStringToObject(r, "build", b);
    cJSON_AddBoolToObject(r, "auth_en", auth_enabled());
    cJSON *lr = cJSON_AddObjectToObject(r, "lastReset");
    cJSON_AddStringToObject(lr, "reason", breadcrumb_reset_reason());
    cJSON_AddStringToObject(lr, "breadcrumb", breadcrumb_last());
    cJSON_AddStringToObject(lr, "summary", breadcrumb_summary());
    return r;
}

static cJSON *restart_result(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "restarting", true);
    cJSON_AddNumberToObject(r, "delay_ms", RESTART_DELAY_MS);
    return r;
}

static cJSON *restart_call(bool factory_reset, rpc_err_t *err)
{
    const esp_err_t e = webui_restart_later(factory_reset, factory_reset ? BC_WHY_FACTORY_RESET : BC_WHY_API_RESTART);
    if (e == ESP_ERR_INVALID_STATE) return rpc_fail(err, RPC_E_BAD_STATE, RPC_MSG_RESTARTING);
    if (e != ESP_OK) return rpc_fail(err, RPC_E_UNAVAILABLE, "no memory for the restart task: try again");
    ESP_LOGW(TAG, "%s requested over the API", factory_reset ? "factory reset" : "restart");
    return restart_result();
}

static cJSON *m_restart(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    return restart_call(false, err);
}

static cJSON *m_factory_reset(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    return restart_call(true, err);
}

static cJSON *m_list_methods(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "methods", rpc_list_methods());
    return r;
}

/* Device.Identify: "Find my box": the speaker beeps for 3 s and the status light blinks white for
 * 30 s, whatever the speaker and light settings say -> {seconds: 30, speaker: "ok" | why it can't}. */
static cJSON *m_identify(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    feedback_identify();
    const char *why = feedback_speaker_error();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "seconds", 30);
    cJSON_AddStringToObject(r, "speaker", why ? why : "ok");
    return r;
}

static const char *const DEVICE_KEYS[] = {"boxName", "mdnsEnabled", "deckBrightness", "deckLimit", "companionAdopt",
                                           "gestureEnabled", "gestureKeys", "gestureTaps", "gestureHoldSeconds", NULL};

/* Device.GetConfig -> {boxName, mdnsEnabled, deckBrightness, deckLimit, companionAdopt, gestureEnabled, gestureKeys,
 * gestureTaps, gestureHoldSeconds}: deckBrightness (10-100, steps of 10) is the decks' brightness while the box drives
 * them (its maintenance screens); deckLimit the decks taken at once, 2 or 4, as saved; companionAdopt
 * whether Companion's Discover may set the Companion target through the Satellite REST API without the settings PIN
 * (web.c); gesture* the box menu gesture on the decks: on, its two corners (the first tapped), the taps, the
 * hold in seconds. */
static cJSON *m_device_get_config(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    cJSON *r = cJSON_CreateObject();
    webui_cfg_lock();
    cJSON_AddStringToObject(r, "boxName", s_cfg->box_name);
    cJSON_AddBoolToObject(r, "mdnsEnabled", s_cfg->mdns_enabled);
    cJSON_AddNumberToObject(r, "deckBrightness", s_cfg->deck_brightness);
    cJSON_AddNumberToObject(r, "deckLimit", s_cfg->deck_limit);
    cJSON_AddBoolToObject(r, "companionAdopt", s_cfg->companion_adopt);
    cJSON_AddBoolToObject(r, "gestureEnabled", s_cfg->gesture_enabled);
    cJSON *gk = cJSON_AddArrayToObject(r, "gestureKeys");
    for (int i = 0; gk && i < 2; i++) cJSON_AddItemToArray(gk, cJSON_CreateString(boxcfg_corner_name(s_cfg->gesture_keys[i])));
    cJSON_AddNumberToObject(r, "gestureTaps", s_cfg->gesture_taps);
    cJSON_AddNumberToObject(r, "gestureHoldSeconds", s_cfg->gesture_hold_s);
    webui_cfg_unlock();
    return r;
}

/* A saved setting that applies at the next start and differs from this run's (the surface mode; the deck limit):
 * restart_required in r; unless dry_run, the reply goes out and the box restarts a second later, its breadcrumb
 * why (restarting true, delay_ms). what names it in the log. */
static cJSON *restart_to_apply(cJSON *r, const char *why, const char *what)
{
    cJSON_ReplaceItemInObject(r, "restart_required", cJSON_CreateTrue());
    if (cJSON_IsTrue(cJSON_GetObjectItem(r, "dry_run"))) return r;
    const esp_err_t e = webui_restart_later(false, why);
    const bool restarting = e == ESP_OK || e == ESP_ERR_INVALID_STATE; /* else no memory for the task */
    cJSON_AddBoolToObject(r, "restarting", restarting);
    if (restarting) {
        cJSON_AddNumberToObject(r, "delay_ms", RESTART_DELAY_MS);
        ESP_LOGW(TAG, "%s saved: restarting into it", what);
    } else {
        ESP_LOGE(TAG, "%s saved, but the restart couldn't be started: restart the box", what);
    }
    return r;
}

/* Device.SetConfig {boxName?, mdnsEnabled?, deckBrightness?, deckLimit?, companionAdopt?, gestureEnabled?, gestureKeys?,
 * gestureTaps?, gestureHoldSeconds?, dry_run?}: a deckLimit (2 or 4) other than this run's is saved, then the box
 * restarts into it as a surface mode change does (restart_to_apply); the gesture applies at once
 * (webui_deck_push_settings). */
static cJSON *m_device_set_config(const cJSON *p, rpc_err_t *err)
{
    cJSON *r = set_section(p, DEVICE_KEYS, NULL, err);
    const cJSON *dl = cJSON_GetObjectItemCaseSensitive(p, "deckLimit");
    if (!r || !dl || (int)dl->valuedouble == s_deck_limit) return r; /* boxconfig took it: 2 or 4 */
    char what[24];
    snprintf(what, sizeof what, "deck limit %d", (int)dl->valuedouble);
    return restart_to_apply(r, BC_WHY_DECK_LIMIT, what);
}

/* ---- Companion.* ---- */

/* "mode" in the API is boxconfig's "surfaceMode" (the key the stored config and seed tool use). */
static const char *const COMPANION_KEYS[] = {"companionAuto", "companionHost", "companionPort", "companionPinId",
                                             "surfaceMode", NULL};

/* Companion.GetConfig -> {companionAuto, companionHost, companionPort, companionPinId, mode}: mode is
 * the saved one ("satellite" | "dock"; Sys.GetStatus says which this run uses). */
static cJSON *m_companion_get_config(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    cJSON *r = cJSON_CreateObject();
    webui_cfg_lock();
    cJSON_AddBoolToObject(r, "companionAuto", s_cfg->companion_auto);
    cJSON_AddStringToObject(r, "companionHost", s_cfg->companion_host);
    cJSON_AddNumberToObject(r, "companionPort", s_cfg->companion_port);
    cJSON_AddStringToObject(r, "companionPinId", s_cfg->companion_pin_id);
    cJSON_AddStringToObject(r, "mode", mode_name(s_cfg->surface_mode));
    webui_cfg_unlock();
    return r;
}

/* Companion.SetConfig {companionAuto?, companionHost?, companionPort?, companionPinId?, mode?,
 * dry_run?} -> {restart_required[, restarting, delay_ms][, dry_run]}. A mode that differs from this
 * run's is saved, the reply goes out and the box restarts a second later: restart_required and
 * restarting true. With dry_run nothing is saved and nothing restarts. */
static cJSON *m_companion_set_config(const cJSON *p, rpc_err_t *err)
{
    cJSON *q = cJSON_CreateObject();
    const cJSON *it, *mode = NULL;
    cJSON_ArrayForEach(it, p) {
        if (!strcmp(it->string, "surfaceMode")) { /* the API's name for it is "mode" */
            cJSON_Delete(q);
            return rpc_fail(err, RPC_E_INVALID_ARG, "unknown key surfaceMode");
        }
        if (!strcmp(it->string, "mode")) {
            if (!cJSON_IsString(it) || (strcmp(it->valuestring, MODE_SATELLITE) && strcmp(it->valuestring, MODE_DOCK))) {
                cJSON_Delete(q);
                return rpc_fail(err, RPC_E_INVALID_ARG, "mode must be \"" MODE_SATELLITE "\" or \"" MODE_DOCK "\"");
            }
            mode = it;
            cJSON_AddItemToObject(q, "surfaceMode", cJSON_Duplicate(it, true));
        } else {
            cJSON_AddItemToObject(q, it->string, cJSON_Duplicate(it, true));
        }
    }
    cJSON *r = set_section(q, COMPANION_KEYS, NULL, err);
    cJSON_Delete(q);
    if (!r || !mode) return r;
    const box_surface_mode_t want = strcmp(mode->valuestring, MODE_DOCK) ? BOX_SURFACE_SATELLITE : BOX_SURFACE_DOCK;
    if (want == s_mode) return r; /* this run's mode already */
    char what[40];
    snprintf(what, sizeof what, "surface mode \"%s\"", mode_name(want));
    return restart_to_apply(r, BC_WHY_MODE_SWITCH, what);
}

/* Companion.GetStatus -> webui_companion_status() (the Satellite connection; idle in dock mode, where
 * the Satellite client never starts) + mode (this run's) + in dock mode dock: per dock, whether
 * Companion has its deck's connection (docks_json). */
static cJSON *m_companion_status(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    cJSON *r = webui_companion_status();
    cJSON_AddStringToObject(r, "mode", mode_name(s_mode));
    if (s_mode == BOX_SURFACE_DOCK) cJSON_AddItemToObject(r, "dock", docks_json());
    return r;
}

/* Companion.List -> [{id, host, port, version, pinned}] (seen by mDNS in the last 30 s) */
static cJSON *m_companion_list(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    int n = 0;
    companion_instance_t *inst = instances(&n);
    char pin[sizeof s_cfg->companion_pin_id];
    webui_cfg_lock();
    strlcpy(pin, s_cfg->companion_pin_id, sizeof pin);
    webui_cfg_unlock();
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", inst[i].id);
        cJSON_AddStringToObject(o, "host", inst[i].host);
        cJSON_AddNumberToObject(o, "port", inst[i].port);
        cJSON_AddStringToObject(o, "version", inst[i].version);
        cJSON_AddBoolToObject(o, "pinned", pin[0] && !strcmp(pin, inst[i].id));
        cJSON_AddItemToArray(a, o);
    }
    free(inst);
    return a;
}

/* Companion.Choose {id}: automatic mode, pinned to that instance (it must be visible now). */
static cJSON *m_companion_choose(const cJSON *p, rpc_err_t *err)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(p, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0]) return rpc_fail(err, RPC_E_INVALID_ARG, "id is required");
    int n = 0;
    companion_instance_t *inst = instances(&n);
    int k = -1;
    for (int i = 0; i < n && k < 0; i++)
        if (!strcmp(inst[i].id, id->valuestring)) k = i;
    if (k < 0) {
        free(inst);
        return rpc_fail(err, RPC_E_NOT_FOUND, "no Companion with id %.40s is visible", id->valuestring);
    }
    cJSON *c = cJSON_CreateObject();
    cJSON_AddBoolToObject(c, "companionAuto", true);
    cJSON_AddStringToObject(c, "companionPinId", inst[k].id);
    cJSON_AddStringToObject(c, "companionHost", inst[k].host);
    cJSON_AddNumberToObject(c, "companionPort", inst[k].port);
    free(inst);
    char msg[96];
    esp_err_t se;
    const bool ok = apply_cfg(apply_cjson, c, false, msg, sizeof msg, &se); /* c itself */
    cJSON_Delete(c);
    if (!ok) return rpc_fail(err, se == ESP_OK ? RPC_E_INVALID_ARG : RPC_E_UNAVAILABLE, "%s", msg);
    ESP_LOGI(TAG, "Companion %s chosen", id->valuestring);
    return cfg_result(false);
}

/* ---- Eth.* / Wifi.* / Ap.* ---- */

static const char *const ETH_KEYS[] = {"ethDhcp", "ethIp", "ethMask", "ethGw", "ethDns", NULL};
static const char *const WIFI_KEYS[] = {"wifiSsid", "wifiPass", "wifiBackupSsid", "wifiBackupPass", "wifiEnabled", NULL};
static const char *const WIFI_RO[] = {"wifiHasPass", "wifiBackupHasPass", NULL};
static const char *const AP_KEYS[] = {"apEnabled", "apSecured", "apPass", NULL};
static const char *const AP_RO[] = {"apSsid", NULL};

static cJSON *m_eth_get_config(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    cJSON *r = cJSON_CreateObject();
    webui_cfg_lock();
    cJSON_AddBoolToObject(r, "ethDhcp", s_cfg->eth_dhcp);
    cJSON_AddStringToObject(r, "ethIp", s_cfg->eth_ip);
    cJSON_AddStringToObject(r, "ethMask", s_cfg->eth_mask);
    cJSON_AddStringToObject(r, "ethGw", s_cfg->eth_gw);
    cJSON_AddStringToObject(r, "ethDns", s_cfg->eth_dns);
    webui_cfg_unlock();
    return r;
}

static cJSON *m_eth_set_config(const cJSON *p, rpc_err_t *err) { return set_section(p, ETH_KEYS, NULL, err); }

static cJSON *m_eth_status(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    webui_cfg_lock();
    const bool dhcp = s_cfg->eth_dhcp;
    webui_cfg_unlock();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "link", ns.eth_link);
    cJSON_AddBoolToObject(r, "dhcp", dhcp);
    cJSON_AddBoolToObject(r, "routed", ns.route == NET_ROUTE_ETH);
    cJSON_AddStringToObject(r, "ip", ns.route == NET_ROUTE_ETH ? ns.ip : "");
    return r;
}

/* Wifi.GetConfig -> {wifiSsid, wifiHasPass, wifiBackupSsid, wifiBackupHasPass, wifiEnabled}: never a password itself */
static cJSON *m_wifi_get_config(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    cJSON *r = cJSON_CreateObject();
    webui_cfg_lock();
    cJSON_AddStringToObject(r, "wifiSsid", s_cfg->wifi_ssid);
    cJSON_AddBoolToObject(r, "wifiHasPass", s_cfg->wifi_pass[0] != '\0');
    cJSON_AddStringToObject(r, "wifiBackupSsid", s_cfg->wifi_backup_ssid);
    cJSON_AddBoolToObject(r, "wifiBackupHasPass", s_cfg->wifi_backup_pass[0] != '\0');
    cJSON_AddBoolToObject(r, "wifiEnabled", s_cfg->wifi_enabled);
    webui_cfg_unlock();
    return r;
}

/* Wifi.SetConfig {wifiSsid?, wifiPass?, wifiBackupSsid?, wifiBackupPass?, wifiEnabled?}: a missing password keeps the
 * stored one. A backup name left empty means no backup. */
static cJSON *m_wifi_set_config(const cJSON *p, rpc_err_t *err) { return set_section(p, WIFI_KEYS, WIFI_RO, err); }

/* Wifi.Scan {results?}: the networks in range. Without params it starts a scan and returns at once
 * (it takes 3-6 s): {scanning: true} when one is under way (started now, or already running), or the
 * last scan's answer with cached: true when that ended less than 3 s ago. With {results: true} it
 * starts nothing and returns the latest answer. Answer: {results: [{ssid, rssi, auth, channel}], scanning,
 * ageMs, cached?, error?}: one entry per name, strongest first, at most WIFISCAN_MAX; ageMs is -1 and
 * results empty before the first scan; error says why the last scan failed (while not scanning). While
 * the hotspot is on its clients may stall for the scan. */
static cJSON *m_wifi_scan(const cJSON *p, rpc_err_t *err)
{
    const cJSON *want = cJSON_GetObjectItemCaseSensitive(p, "results");
    if (want && !cJSON_IsBool(want)) return rpc_fail(err, RPC_E_INVALID_ARG, "results must be true or false");
    bool cached = false;
    if (!cJSON_IsTrue(want)) {
        char why[100];
        const netmgr_scan_t st = netmgr_wifi_scan_start(why, sizeof why);
        if (st == NETMGR_SCAN_NO_WIFI || st == NETMGR_SCAN_FAILED) return rpc_fail(err, RPC_E_UNAVAILABLE, "%s", why);
        if (st == NETMGR_SCAN_STARTED || st == NETMGR_SCAN_SCANNING) {
            cJSON *r = cJSON_CreateObject();
            if (!r) return rpc_fail(err, RPC_E_UNAVAILABLE, RPC_MSG_NO_MEMORY);
            cJSON_AddBoolToObject(r, "scanning", true);
            return r;
        }
        cached = true;
    }
    netmgr_scan_results_t *sr = calloc(1, sizeof *sr);
    if (!sr) return rpc_fail(err, RPC_E_UNAVAILABLE, RPC_MSG_NO_MEMORY);
    netmgr_wifi_scan_results(sr);
    cJSON *r = cJSON_CreateObject();
    cJSON *a = r ? cJSON_AddArrayToObject(r, "results") : NULL;
    if (!a) {
        cJSON_Delete(r);
        free(sr);
        return rpc_fail(err, RPC_E_UNAVAILABLE, RPC_MSG_NO_MEMORY);
    }
    for (size_t i = 0; i < sr->n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "ssid", sr->aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", sr->aps[i].rssi);
        cJSON_AddStringToObject(o, "auth", wifiscan_auth_name(sr->aps[i].auth));
        cJSON_AddNumberToObject(o, "channel", sr->aps[i].channel);
        cJSON_AddItemToArray(a, o);
    }
    cJSON_AddBoolToObject(r, "scanning", sr->scanning);
    cJSON_AddNumberToObject(r, "ageMs", (double)sr->age_ms);
    if (cached) cJSON_AddBoolToObject(r, "cached", true);
    if (sr->error[0]) cJSON_AddStringToObject(r, "error", sr->error);
    free(sr);
    return r;
}

/* Wifi.GetStatus -> {configured, ssid, connected, routed, ip, c6_fw, net, sta_ssid, sta_rssi, backup}:
 * ssid is the main network's configured name; net says which network is joined ("main", "backup" or
 * "none"), sta_ssid its name ("" when none) and sta_rssi its signal in dBm (0 when none); backup: a
 * backup network is configured. */
/* Wifi.Use {net: "main" | "backup"} -> {net}: the station joins that network now, and keeps it as the one
 * in use (stays while it works; after a loss it first). */
static cJSON *m_wifi_use(const cJSON *p, rpc_err_t *err)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(p, "net");
    const bool main = cJSON_IsString(n) && !strcmp(n->valuestring, "main");
    const bool backup = cJSON_IsString(n) && !strcmp(n->valuestring, "backup");
    if (!main && !backup) return rpc_fail(err, RPC_E_INVALID_ARG, "net must be \"main\" or \"backup\"");
    const esp_err_t e = netmgr_wifi_use(main ? NETMGR_STA_MAIN : NETMGR_STA_BACKUP);
    if (e == ESP_ERR_NOT_FOUND) return rpc_fail(err, RPC_E_UNAVAILABLE, RPC_MSG_NO_BACKUP);
    if (e != ESP_OK) return rpc_fail(err, RPC_E_UNAVAILABLE, RPC_MSG_WIFI_OFF);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "net", main ? "main" : "backup");
    return r;
}

static cJSON *m_wifi_status(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    char ssid[sizeof s_cfg->wifi_ssid];
    webui_cfg_lock();
    strlcpy(ssid, s_cfg->wifi_ssid, sizeof ssid);
    const bool has_backup = s_cfg->wifi_backup_ssid[0] != '\0';
    webui_cfg_unlock();
    char run[24], carried[24];
    c6fw_get_versions(run, sizeof run, carried, sizeof carried);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "configured", ssid[0] != '\0');
    cJSON_AddStringToObject(r, "ssid", ssid);
    cJSON_AddBoolToObject(r, "connected", ns.wifi_connected);
    cJSON_AddBoolToObject(r, "routed", ns.route == NET_ROUTE_WIFI);
    cJSON_AddStringToObject(r, "ip", ns.route == NET_ROUTE_WIFI ? ns.ip : "");
    cJSON_AddStringToObject(r, "c6_fw", run);
    cJSON_AddStringToObject(r, "net", sta_net_name(ns.sta_net));
    cJSON_AddStringToObject(r, "sta_ssid", ns.sta_ssid);
    cJSON_AddNumberToObject(r, "sta_rssi", ns.sta_rssi);
    cJSON_AddBoolToObject(r, "backup", has_backup);
    cJSON_AddBoolToObject(r, "enabled", ns.wifi_enabled); /* false: the station is off by the setting */
    return r;
}

/* Ap.GetConfig -> {apEnabled, apSecured, apSsid, apPass}: the hotspot password is shown on the page
 *, as on the decks and the serial console. apSecured true is the default;
 * false (the switch): the hotspot is open and apPass is kept, unused, for when it is switched on. */
static cJSON *m_ap_get_config(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    cJSON *r = cJSON_CreateObject();
    webui_cfg_lock();
    cJSON_AddBoolToObject(r, "apEnabled", s_cfg->ap_enabled);
    cJSON_AddBoolToObject(r, "apSecured", s_cfg->ap_secured);
    cJSON_AddStringToObject(r, "apPass", s_cfg->ap_pass);
    webui_cfg_unlock();
    cJSON_AddStringToObject(r, "apSsid", ns.ap_ssid);
    return r;
}

/* Ap.SetConfig {apEnabled?, apSecured?, apPass?}: apPass must be 8-63 while apSecured (boxcfg) */
static cJSON *m_ap_set_config(const cJSON *p, rpc_err_t *err) { return set_section(p, AP_KEYS, AP_RO, err); }

static cJSON *m_ap_status(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "on", ns.ap_on);
    cJSON_AddBoolToObject(r, "forced", ns.ap_forced);
    cJSON_AddStringToObject(r, "ssid", ns.ap_ssid);
    cJSON_AddNumberToObject(r, "clients", ns.ap_clients);
    cJSON_AddStringToObject(r, "ip", ns.ap_on ? WEBUI_AP_IP : "");
    return r;
}

/* ---- Deck.* ---- */

static cJSON *m_deck_list(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    return webui_decks();
}

/* Deck.ShowMessage {text, seconds?: 0 = until cleared} */
static cJSON *m_deck_show(const cJSON *p, rpc_err_t *err)
{
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(p, "text");
    const cJSON *sec = cJSON_GetObjectItemCaseSensitive(p, "seconds");
    if (!cJSON_IsString(text)) return rpc_fail(err, RPC_E_INVALID_ARG, "text is required");
    double s = 0;
    if (sec) {
        if (!cJSON_IsNumber(sec)) return rpc_fail(err, RPC_E_INVALID_ARG, "seconds must be a number");
        s = sec->valuedouble;
    }
    if (s < 0 || s > SURF_MSG_MAX_S || s != (double)(uint32_t)s)
        return rpc_fail(err, RPC_E_INVALID_ARG, "seconds must be a whole number 0-%d (0 = until cleared)", SURF_MSG_MAX_S);
    const esp_err_t e = surfaces_show_message(text->valuestring, (uint32_t)s);
    if (e == ESP_ERR_INVALID_ARG)
        return rpc_fail(err, RPC_E_INVALID_ARG, "text must be 1-%d printable ASCII characters (newlines allowed)",
                        SURF_MSG_MAX);
    if (e != ESP_OK) return rpc_fail(err, RPC_E_UNAVAILABLE, "%s", esp_err_to_name(e));
    return message_json();
}

static cJSON *m_deck_clear(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    surfaces_clear_message();
    return message_json();
}

/* Deck.ShowMaintenance {on, serial?} -> {on, open: [serials], serial?, model?}: the box menu (the maintenance screen
 * on demand), one per deck, Companion's decks too, the deck's input then the box's only. With serial:
 * on that deck alone (a serial not attached, or of a deck with no screen: RPC_E_UNAVAILABLE). Without: on
 * true it opens on the biggest deck with a screen, on false it closes on every deck. The reply: on, any deck open;
 * open, the serials it is open on; serial and model, the deck just opened. Each deck also closes by its Close key
 * or 60 s without a press on it; the service button closes them all. */
static cJSON *m_deck_maint(const cJSON *p, rpc_err_t *err)
{
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(p, "on"), *sj = cJSON_GetObjectItemCaseSensitive(p, "serial");
    if (!cJSON_IsBool(on)) return rpc_fail(err, RPC_E_INVALID_ARG, "on must be true or false");
    if (sj && !cJSON_IsString(sj)) return rpc_fail(err, RPC_E_INVALID_ARG, "serial must be a string");
    const char *serial_in = sj ? sj->valuestring : NULL;
    const esp_err_t e = surfaces_maint_deck_set(serial_in, cJSON_IsTrue(on));
    if (e == ESP_ERR_NOT_FOUND) return rpc_fail(err, RPC_E_UNAVAILABLE, RPC_MSG_NO_DECK);
    if (e != ESP_OK) return rpc_fail(err, RPC_E_UNAVAILABLE, "%s", esp_err_to_name(e));
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "on", surfaces_maint_on());
    char open[SURF_MAX_DECKS][33];
    const int n = surfaces_maint_list(open, SURF_MAX_DECKS);
    cJSON *a = cJSON_AddArrayToObject(r, "open");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(open[i]));
    if (cJSON_IsTrue(on)) { /* the deck just opened: the one named, else the pick */
        char serial[33], model[33];
        if (serial_in && serial_in[0]) {
            if (!strncmp(serial_in, "streamdeck:", 11)) serial_in += 11; /* a device_id names it too */
            usbdeck_info_t *d = calloc(MAX_DECKS, sizeof *d);
            const int nd = d ? usbdeck_list(d, MAX_DECKS) : 0;
            for (int i = 0; i < nd; i++)
                if (!strcmp(d[i].serial, serial_in)) {
                    cJSON_AddStringToObject(r, "serial", d[i].serial);
                    cJSON_AddStringToObject(r, "model", d[i].model);
                }
            free(d);
        } else if (surfaces_maint_pick(serial, model)) {
            cJSON_AddStringToObject(r, "serial", serial);
            cJSON_AddStringToObject(r, "model", model);
        }
    }
    return r;
}

/* ---- Feedback.* (the speaker and the status light, components/feedback) ---- */

static const char *const FEEDBACK_KEYS[] = {"speakerEnabled", "speakerVolume", "soundReady",    "soundConnected",
                                            "soundProblem",   "soundDeckPlug", "soundKeyClick", "ledEnabled",
                                            "ledFlash",       "ledPin",        "ledBrightness", NULL};
static const char *const FEEDBACK_RO[] = {"ledPins", NULL};

/* Feedback.GetConfig -> {speakerEnabled, speakerVolume, soundReady, soundConnected, soundProblem,
 * soundDeckPlug, soundKeyClick, ledEnabled, ledFlash, ledPin, ledBrightness, ledPins}: ledPins (read-only) are
 * the GPIOs ledPin may name. */
static cJSON *m_feedback_get_config(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    cJSON *r = cJSON_CreateObject();
    webui_cfg_lock();
    cJSON_AddBoolToObject(r, "speakerEnabled", s_cfg->speaker_enabled);
    cJSON_AddNumberToObject(r, "speakerVolume", s_cfg->speaker_volume);
    cJSON_AddBoolToObject(r, "soundReady", s_cfg->sound_ready);
    cJSON_AddBoolToObject(r, "soundConnected", s_cfg->sound_connected);
    cJSON_AddBoolToObject(r, "soundProblem", s_cfg->sound_problem);
    cJSON_AddBoolToObject(r, "soundDeckPlug", s_cfg->sound_deck_plug);
    cJSON_AddBoolToObject(r, "soundKeyClick", s_cfg->sound_key_click);
    cJSON_AddBoolToObject(r, "ledEnabled", s_cfg->led_enabled);
    cJSON_AddBoolToObject(r, "ledFlash", s_cfg->led_flash);
    cJSON_AddNumberToObject(r, "ledPin", s_cfg->led_pin);
    cJSON_AddNumberToObject(r, "ledBrightness", s_cfg->led_brightness);
    webui_cfg_unlock();
    int pins[24];
    cJSON_AddItemToObject(r, "ledPins", cJSON_CreateIntArray(pins, boxcfg_led_pins(pins, 24)));
    return r;
}

/* Feedback.SetConfig {the keys above, dry_run?}: applied at once, no restart (a new ledPin too). */
static cJSON *m_feedback_set_config(const cJSON *p, rpc_err_t *err)
{
    return set_section(p, FEEDBACK_KEYS, FEEDBACK_RO, err);
}

/* Feedback.Test {event, volume?}: plays event's sound, also when it or the speaker is turned off, at
 * volume (0-100, a preview: nothing is saved) or the set volume, and flashes its colour on the light;
 * "identify" is Find my box. -> {event, speaker: "ok" | why it can't}. */
static cJSON *m_feedback_test(const cJSON *p, rpc_err_t *err)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(p, "event"), *v = cJSON_GetObjectItemCaseSensitive(p, "volume");
    fb_event_t ev;
    if (!cJSON_IsString(e) || !fb_event_parse(e->valuestring, &ev))
        return rpc_fail(err, RPC_E_INVALID_ARG,
                        "event must be ready, connected, problem, deckIn, deckOut, keyClick, confirm, locked or "
                        "identify");
    if (v && (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= 100) ||
              v->valuedouble != (double)(int)v->valuedouble))
        return rpc_fail(err, RPC_E_INVALID_ARG, "volume must be a whole number 0-100");
    feedback_test(ev, v ? (int)v->valuedouble : -1);
    const char *why = feedback_speaker_error();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "event", fb_event_name(ev));
    cJSON_AddStringToObject(r, "speaker", why ? why : "ok");
    return r;
}

/* ---- Ota.* / Log.* ---- */

static cJSON *m_ota_status(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    return web_ota_status();
}

/* Ota.Sync {force?} -> {started: true}: copies the running firmware into the other slot now (* slotsync_start). Refused while a sync or an upload runs, while this firmware is pending verify, and
 * over a newer image unless force. Ota.GetStatus.sync shows the progress. */
static cJSON *m_ota_sync(const cJSON *p, rpc_err_t *err)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(p, "force");
    if (f && !cJSON_IsBool(f)) return rpc_fail(err, RPC_E_INVALID_ARG, "force must be true or false");
    const esp_err_t e = slotsync_start(cJSON_IsTrue(f));
    if (e != ESP_OK) return rpc_fail(err, RPC_E_BAD_STATE, "%s", slotsync_err_text(e));
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "started", true);
    return r;
}

/* Ota.BootOther -> {restarting: true, slot}: makes the other slot the boot slot and restarts a second
 * later (slotsync_boot_other). Refused when that slot holds no valid image or another project's,
 * and while a sync or an upload runs or this firmware is pending verify. */
static cJSON *m_ota_boot_other(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    slotsync_status_t ss;
    slotsync_get(&ss);
    const esp_err_t e = slotsync_boot_other();
    if (e != ESP_OK) return rpc_fail(err, RPC_E_BAD_STATE, "%s", slotsync_err_text(e));
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "restarting", true);
    cJSON_AddStringToObject(r, "slot", ss.other);
    return r;
}

/* Log.Get {since?} -> {seq, text}: log text after `since` (0 = the oldest kept), at most 8 KB (the
 * newest); pass seq back next time. Not public: the ring holds the hotspot password. */
static cJSON *m_log_get(const cJSON *p, rpc_err_t *err)
{
    const cJSON *since = cJSON_GetObjectItemCaseSensitive(p, "since");
    if (since && (!cJSON_IsNumber(since) || since->valuedouble < 0 || since->valuedouble > UINT32_MAX ||
                  since->valuedouble != (double)(uint32_t)since->valuedouble))
        return rpc_fail(err, RPC_E_INVALID_ARG, "since must be a whole number 0-4294967295 (the seq from the last call)");
    uint32_t seq = since ? (uint32_t)since->valuedouble : 0;
    char *buf = heap_caps_malloc(LOG_CHUNK + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return rpc_fail(err, RPC_E_UNAVAILABLE, RPC_MSG_NO_MEMORY);
    const size_t n = logbuf_read(buf, LOG_CHUNK, &seq);
    buf[n] = '\0';
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "seq", seq);
    cJSON_AddStringToObject(r, "text", buf);
    free(buf);
    return r;
}

esp_err_t webui_methods_init(box_config_t *cfg, void (*changed)(void), box_surface_mode_t mode)
{
    if (!s_lock_fn || !s_unlock_fn) return ESP_ERR_INVALID_STATE;
    s_cfg = cfg;
    s_mode = mode;
    s_deck_limit = cfg->deck_limit; /* read before the server starts, as main read it for surfaces */
    s_changed = changed;
    rpc_register("Sys.GetStatus", m_sys_status, 0);
    rpc_register("Device.GetInfo", m_device_info, 0);
    rpc_register("Device.Restart", m_restart, 0);
    rpc_register("Device.FactoryReset", m_factory_reset, 0);
    rpc_register("Device.GetConfig", m_device_get_config, 0);
    rpc_register("Device.SetConfig", m_device_set_config, 0);
    rpc_register("Device.ListMethods", m_list_methods, 0);
    rpc_register("Device.Identify", m_identify, 0);
    rpc_register("Companion.GetConfig", m_companion_get_config, 0);
    rpc_register("Companion.SetConfig", m_companion_set_config, 0);
    rpc_register("Companion.GetStatus", m_companion_status, 0);
    rpc_register("Companion.List", m_companion_list, 0);
    rpc_register("Companion.Choose", m_companion_choose, 0);
    rpc_register("Eth.GetConfig", m_eth_get_config, 0);
    rpc_register("Eth.SetConfig", m_eth_set_config, 0);
    rpc_register("Eth.GetStatus", m_eth_status, 0);
    rpc_register("Wifi.GetConfig", m_wifi_get_config, 0);
    rpc_register("Wifi.SetConfig", m_wifi_set_config, 0);
    rpc_register("Wifi.Use", m_wifi_use, 0);
    rpc_register("Wifi.GetStatus", m_wifi_status, 0);
    rpc_register("Wifi.Scan", m_wifi_scan, 0);
    rpc_register("Ap.GetConfig", m_ap_get_config, 0);
    rpc_register("Ap.SetConfig", m_ap_set_config, 0);
    rpc_register("Ap.GetStatus", m_ap_status, 0);
    rpc_register("Deck.List", m_deck_list, 0);
    rpc_register("Deck.ShowMessage", m_deck_show, 0);
    rpc_register("Deck.ClearMessage", m_deck_clear, 0);
    rpc_register("Deck.ShowMaintenance", m_deck_maint, 0);
    rpc_register("Feedback.GetConfig", m_feedback_get_config, 0);
    rpc_register("Feedback.SetConfig", m_feedback_set_config, 0);
    rpc_register("Feedback.Test", m_feedback_test, 0);
    rpc_register("Ota.GetStatus", m_ota_status, 0);
    rpc_register("Ota.Sync", m_ota_sync, 0);
    rpc_register("Ota.BootOther", m_ota_boot_other, 0);
    rpc_register("Log.Get", m_log_get, 0);
    return deckact_start(); /* the decks' maintenance actions */
}
