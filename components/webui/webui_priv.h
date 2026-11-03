/* Shared between the webui files (not part of the component's interface). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boxconfig/config.h"
#include "cJSON.h"
#include "esp_err.h"

#define WEBUI_AP_IP "192.168.4.1"

/* ---- methods.c: the config, status and the RPC methods other than Auth.* ---- */

/* The box config and the lock given to webui_set_cfg_lock. Hold the lock only to copy, change and
 * save: never across network I/O, Wi-Fi calls or anything that waits on the supervisor. */
void webui_cfg_lock(void);
void webui_cfg_unlock(void);
box_config_t *webui_cfg(void);
/* Registers the methods; cfg / changed / mode as given to webui_start. ESP_ERR_INVALID_STATE when
 * webui_set_cfg_lock was not called. */
esp_err_t webui_methods_init(box_config_t *cfg, void (*changed)(void), box_surface_mode_t mode);
/* Applies a companion-satellite /api/config body (boxcfg_apply_satellite_config) to the config:
 * lock, validate a copy, save, unlock, then signal the change. false with err = the offending key
 * ("host", "port", ...) when it is refused, "save" when saving failed, or "limit". has_host / has_port: the
 * body has them. adopted_by: the client's address when it came without the PIN (companionAdopt), else NULL; then
 * a request that keeps the Companion target writes nothing, a change spends one of the adopt limiter's tokens
 * (none left: "limit", nothing applied) and is logged ("adopted by <ip>: Companion <host>:<port>") and toasted on the
 * decks ("Adopted"). */
bool webui_apply_satellite(const char *json, bool has_host, bool has_port, const char *adopted_by, char *err, size_t n);
/* This run's surface mode (as main gave it to webui_start). */
box_surface_mode_t webui_run_mode(void);
/* Restarts in about a second (after the reply went out); with factory_reset the settings are erased
 * first, holding the config lock until the restart so no save lands after the erase. why: the
 * restart breadcrumb's reason (BC_WHY_*, breadcrumb.h; a string that outlives the call).
 * ESP_ERR_INVALID_STATE: a restart is already on its way; ESP_ERR_NO_MEM: no task for it. */
esp_err_t webui_restart_later(bool factory_reset, const char *why);
/* The running image is on its first boot after an update, not confirmed yet (reads otadata from
 * flash: not from a task with a PSRAM stack). */
bool webui_pending_verify(void);
/* The Companion connection, for Sys.GetStatus, Companion.GetStatus and /api/status. */
cJSON *webui_companion_status(void);
/* Attached decks as an array of objects (Deck.List, /api/surfaces). */
cJSON *webui_decks(void);

/* ---- deckact.c ---- */

/* The deck actions' worker (webui_deck_action). */
esp_err_t deckact_start(void);
/* Gives surfaces what the decks' screens show of the settings (surfaces_set_settings): after webui_start,
 * every saved settings change and every settings password change. Takes the config lock: never call it
 * with it held. */
void webui_deck_push_settings(void);

/* ---- auth.c ---- */

/* Loads the stored password verifier (NVS "boxcfg"/"pwv"); a plain admin password left in the
 * config by an older setup is turned into one and removed. Registers Auth.* and Device.SetAuth. */
void auth_init(void);
bool auth_enabled(void);
/* the settings secret is a PIN (false: none, or a text password of before). */
bool auth_pin_set(void);
/* a PIN entered on a deck, checked as the client "deck" with the lockout. PBKDF2: call it on
 * a worker, never on the surfaces or USB tasks. *wait_s: LOCKED's wait (also after the wrong entry that
 * locked it). The caller wipes pin. Never logged. */
typedef enum { AUTH_PIN_OK, AUTH_PIN_WRONG, AUTH_PIN_LOCKED, AUTH_PIN_NONE } auth_pin_result_t;
auth_pin_result_t auth_check_pin(const char *pin, int *wait_s);
/* Seconds the deck must still wait before a PIN try (0: it may try). Short lock, no hashing. */
int auth_deck_wait_s(void);
bool auth_token_ok(const char *token);
/* A live session's token -> its SHA-256 (for holders that keep no token text); false otherwise. */
bool auth_token_digest(const char *token, uint8_t out[32]);
/* The session with this SHA-256 is still live. */
bool auth_digest_ok(const uint8_t digest[32]);

/* ---- logbuf.c ---- */

/* Copies log text newer than *seq (a running byte count), at most max bytes (the newest ones if
 * more), and moves *seq on. Returns the byte count. */
size_t logbuf_read(char *out, size_t max, uint32_t *seq);

/* ---- notify.c ---- */

/* Pushes NotifyStatus to the WebSocket clients whenever Sys.GetStatus changes (checked every
 * second while anyone listens). */
esp_err_t notify_start(void);
/* A client connected or signed in: the next check pushes even an unchanged status. */
void notify_kick(void);
/* Check for a status to push now, not at the next second (a settings save: rev.cfg). Any task. */
void notify_soon(void);
/* A settings save went through (a *.SetConfig, the settings PIN): Sys.GetStatus.rev.cfg counts it and the open pages
 * read their settings again. Any task. */
void webui_settings_saved(void);

/* ---- web.c ---- */

esp_err_t web_start(void);
/* Runs fn(arg) on the HTTP server's task. */
esp_err_t web_queue_work(void (*fn)(void *), void *arg);
/* A WebSocket client can receive pushes: any client while no password is set, else a signed-in one. */
bool web_ws_listening(void);
/* Sends text to every WebSocket client allowed to see pushes. HTTP server task only. */
void web_ws_broadcast_now(const char *text);
/* Ota.GetStatus. */
cJSON *web_ota_status(void);
