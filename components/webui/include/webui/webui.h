/* The box's web server and JSON-RPC API.
 * HTTP on port 80:
 *   GET  /                      the setup page (gzip, ETag, CSP)
 *   POST /rpc                   a JSON-RPC frame (rpc/rpc.h)
 *   GET|POST /rpc/<Method>      REST: query values or the JSON body are the params
 *   GET  /rpc (WebSocket)       frames both ways, plus NotifyStatus pushes when the status changes
 *   POST /ota                   a raw firmware image (.bin), then a restart into it
 *   /api/...                    the Companion Satellite REST API (companion-satellite rest.ts)
 * plus captive-portal redirects to http://192.168.4.1/ on the hotspot.
 * With a settings password (Device.SetAuth) everything but the page itself, Auth.GetStatus and
 * Auth.Login needs a session token ("Authorization: Bearer <token>" or "token" in a frame). */
#pragma once
#include <stdbool.h>

#include "boxconfig/config.h"
#include "esp_err.h"
#include "netmgr/netmgr.h"
#include "surfaces/surfaces.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The lock that guards *cfg (supervisor_cfg_lock / supervisor_cfg_unlock). Call before webui_start. */
void webui_set_cfg_lock(void (*lock)(void), void (*unlock)(void));

/* Starts capturing log output for Log.Get (a 16 KB ring). Call early in the boot so the ring holds
 * the start-up lines; webui_start calls it too. Idempotent. */
void webui_log_capture_start(void);

/* Registers the RPC methods and starts the HTTP server. cfg is the box's one config (guarded by the
 * lock above); after a *.SetConfig has saved it, on_config_changed signals whoever applies it (the
 * supervisor: handlers never apply network settings themselves). mode: the surface mode this run
 * uses (read once at boot), which the status reports; Companion.SetConfig restarts the box when the
 * saved mode differs from it. ESP_ERR_INVALID_STATE without the lock. Call once, after netmgr_start. */
esp_err_t webui_start(box_config_t *cfg, void (*on_config_changed)(void), box_surface_mode_t mode);

/* The HTTP server is running (webui_start got it up). The OTA self-check requires it: an image
 * without a web page and POST /ota could only be replaced over USB. */
bool webui_running(void);

/* The interface the last firmware upload came in on (POST /ota records it in NVS, namespace "ota",
 * right before the restart into the new image), then forgets it: NETMGR_IF_NONE when there was no
 * record. The supervisor takes it once at start for the OTA self-check. */
netmgr_if_t webui_ota_via_take(void);

/* A firmware upload (POST /ota) is being written, or is written and the box restarts into it. For
 * the status light (the supervisor reports it to components/feedback). Any task. */
bool webui_ota_busy(void);

/* The deck maintenance screens' action hook (surfaces_set_hooks): queues a DUI_A_* action for
 * webui's own worker (internal stack), which runs it through the code path of its RPC
 * (Companion.SetConfig / .Choose, Ap.SetConfig, Device.SetConfig / .Restart, Feedback.SetConfig,
 * Ota.Sync / .BootOther; netmgr_ap_force, satclient_retry) and refuses the locked ones while a settings
 * PIN is set, unless the deck was unlocked with it; a notice on the decks says how it went. Needs
 * webui_start first (before: a no-op). webui also gives surfaces the settings the screens show
 * (surfaces_set_settings) whenever they change. */
void webui_deck_action(int action, const char *arg, bool unlocked);
/* The decks' PIN pad (surfaces hooks pin_check / pin_wait_s): the PIN is checked on the same worker
 * (PBKDF2 never on the surfaces or USB tasks), as the client "deck" with the lockout; the answer
 * goes back by surfaces_pin_result. One check at a time: another meanwhile, or the worker's queue full,
 * returns false at once and nothing comes back (never posts from the caller's task). */
bool webui_deck_pin_check(int slot, uint32_t ticket, const char *pin);
int webui_deck_pin_wait_s(void);

#ifdef __cplusplus
}
#endif
