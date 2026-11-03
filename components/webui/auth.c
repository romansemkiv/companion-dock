/* Settings password and sign-in sessions (password hashing, token sessions and the per-client
 * lockout; no two-factor, API keys or setup window). The rules are
 * in auth_logic.c.
 *
 * No secret set (the default): every method is open. Device.SetAuth sets one, a PIN of 4-8 digits;
 * then only public methods answer without a token. Auth.Login {password} -> {token, expires_in}: a
 * random 32-byte token, valid 12 h; only its SHA-256 is kept, in RAM (a restart signs everyone out). Five
 * wrong entries in a row from one client address lock that address for 60 s, each further lock twice as
 * long up to 1 h. A text password set by an older firmware keeps working for the web sign-in (pin false);
 * the decks unlock with a PIN only (auth_check_pin, the client "deck").
 *
 * Storage: the PBKDF2 verifier is the blob "pwv" in boxconfig's NVS namespace "boxcfg", with "pwpin"
 * (u8 1) beside it when the secret is a PIN, so a factory reset removes them with the rest of the
 * settings. They are written under the config lock, like the config itself (a factory reset holds that
 * lock until the restart). A PIN is never logged or returned. */
#include <stdio.h>
#include <string.h>

#include "auth_logic.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "rpc/rpc.h"
#include "webui_priv.h"

static const char *TAG = "auth";

#define NS "boxcfg"
#define KEY_PWV "pwv"
#define KEY_PIN "pwpin"
#define DECK_CLIENT AUTH_DECK_CLIENT /* the decks are one client for the lockout */

static SemaphoreHandle_t s_mtx; /* guards everything below (never held across PBKDF2) */
static StaticSemaphore_t s_mtx_buf;
static bool s_en;
static bool s_pin; /* the secret is a PIN */
static auth_pwv_t s_pw;
static auth_sessions_t s_sess;
static auth_lim_t s_lim;

static void lock(void) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

bool auth_enabled(void) { return s_en; }

bool auth_pin_set(void) { return auth_secret_kind(s_en, s_pin) == AUTH_SECRET_PIN; }

bool auth_token_ok(const char *token)
{
    if (!s_mtx || !token || !token[0]) return false;
    lock();
    const bool ok = s_en && auth_sess_check(&s_sess, token, now_ms());
    unlock();
    return ok;
}

bool auth_token_digest(const char *token, uint8_t out[32])
{
    return auth_token_ok(token) && auth_token_hash(token, out);
}

bool auth_digest_ok(const uint8_t digest[32])
{
    if (!s_mtx || !digest) return false;
    lock();
    const bool ok = s_en && auth_sess_check_hash(&s_sess, digest, now_ms());
    unlock();
    return ok;
}

/* pw NULL removes the verifier (and the PIN flag); pin: the secret is a PIN. Under the config lock: see
 * the file comment. */
static esp_err_t store_pwv(const auth_pwv_t *pw, bool pin)
{
    webui_cfg_lock();
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = pw ? nvs_set_blob(h, KEY_PWV, pw, sizeof *pw) : nvs_erase_key(h, KEY_PWV);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = pw && pin ? nvs_set_u8(h, KEY_PIN, 1) : nvs_erase_key(h, KEY_PIN);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    webui_cfg_unlock();
    if (e != ESP_OK) ESP_LOGE(TAG, "storing the password failed: %s", esp_err_to_name(e));
    return e;
}

static void make_pwv(auth_pwv_t *pw, const char *pass)
{
    uint8_t salt[AUTH_SALT_LEN];
    esp_fill_random(salt, sizeof salt);
    auth_pwv_make(pw, pass, salt, AUTH_PBKDF2_ITERS);
}

/* A new session for the caller (under lock). */
static void new_session_locked(char out[AUTH_TOKEN_HEX + 1])
{
    uint8_t raw[AUTH_TOKEN_BYTES];
    esp_fill_random(raw, sizeof raw);
    auth_token_hex(raw, out);
    auth_wipe(raw, sizeof raw);
    auth_sess_add(&s_sess, out, now_ms());
}

static const char *caller_src(void)
{
    const rpc_current_t *c = rpc_current();
    return c && c->who.src ? c->who.src : "local";
}

typedef enum { CHK_OK, CHK_WRONG, CHK_WAIT } chk_t;

/* Checks the stored secret for client src, with the lockout (this client's, and the box-wide budget once
 * wrong entries pile up: a refused check is answered without hashing, so a flood cannot keep the box busy
 * with PBKDF2). CHK_WAIT: refused, *wait (s); CHK_WRONG: *wait is the lock it just started (0: none). */
static chk_t check_secret(const char *src, const char *pass, const char *what, int *wait)
{
    *wait = 0;
    lock();
    const bool go = auth_lim_begin(&s_lim, src, now_ms(), wait);
    const auth_pwv_t pw = s_pw;
    unlock();
    if (!go) return CHK_WAIT;
    /* PBKDF2, without auth's lock */
    const bool ok = auth_pwv_check(&pw, pass);
    lock();
    /* the secret changed while we checked: treat as wrong */
    const bool same = s_en && !memcmp(s_pw.salt, pw.salt, sizeof pw.salt);
    if (ok && same) {
        auth_lim_ok(&s_lim, src);
        unlock();
        return CHK_OK;
    }
    const int64_t now = now_ms();
    const bool locked = auth_lim_fail(&s_lim, src, now);
    if (locked) *wait = auth_lim_wait_s(&s_lim, src, now);
    unlock();
    if (locked) ESP_LOGW(TAG, "%s: wrong entry from %s, locked for %d s", what, src, *wait);
    else ESP_LOGW(TAG, "%s: wrong entry from %s", what, src);
    return CHK_WRONG;
}

/* Checks the current secret for the caller: true when it is right, else false with err set. */
static bool verify_current(const char *pass, const char *what, rpc_err_t *err)
{
    int wait = 0;
    switch (check_secret(caller_src(), pass, what, &wait)) {
    case CHK_OK: return true;
    case CHK_WAIT: rpc_fail(err, RPC_E_BAD_STATE, "too many attempts: try again in %d s", wait); return false;
    default: rpc_fail(err, RPC_E_UNAUTHORIZED, s_pin ? "wrong PIN" : "wrong password"); return false;
    }
}

auth_pin_result_t auth_check_pin(const char *pin, int *wait_s)
{
    int wait = 0;
    auth_pin_result_t r = AUTH_PIN_NONE;
    if (s_mtx && auth_pin_set()) {
        switch (check_secret(DECK_CLIENT, pin, "deck unlock", &wait)) {
        case CHK_OK: r = AUTH_PIN_OK; break;
        case CHK_WAIT: r = AUTH_PIN_LOCKED; break;
        default: r = wait ? AUTH_PIN_LOCKED : AUTH_PIN_WRONG; break;
        }
    }
    if (wait_s) *wait_s = wait;
    return r;
}

int auth_deck_wait_s(void)
{
    if (!s_mtx) return 0;
    lock();
    const int w = auth_lim_wait_s(&s_lim, DECK_CLIENT, now_ms());
    unlock();
    return w;
}

/* Auth.GetStatus -> {auth_en, pin, authed, locked_s, sessions?} (public). pin: the secret is a PIN;
 * false with auth_en: a text password of before (the page asks for a password, and to set a PIN). */
static cJSON *m_status(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    const rpc_current_t *c = rpc_current();
    const bool authed = c && c->authed;
    cJSON *r = cJSON_CreateObject();
    lock();
    const int64_t now = now_ms();
    cJSON_AddBoolToObject(r, "auth_en", s_en);
    cJSON_AddBoolToObject(r, "pin", auth_secret_kind(s_en, s_pin) == AUTH_SECRET_PIN);
    cJSON_AddBoolToObject(r, "authed", authed);
    cJSON_AddNumberToObject(r, "locked_s", auth_lim_wait_s(&s_lim, caller_src(), now));
    if (authed && s_en) cJSON_AddNumberToObject(r, "sessions", auth_sess_count(&s_sess, now));
    unlock();
    return r;
}

/* Auth.Login {password} -> {token, expires_in} (public) */
static cJSON *m_login(const cJSON *p, rpc_err_t *err)
{
    if (!s_en) return rpc_fail(err, RPC_E_BAD_STATE, "no PIN is set");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(p, "password");
    if (!cJSON_IsString(pass)) return rpc_fail(err, RPC_E_INVALID_ARG, "password is required");
    if (!verify_current(pass->valuestring, "sign-in", err)) return NULL;
    char tok[AUTH_TOKEN_HEX + 1];
    lock();
    new_session_locked(tok);
    const int n = auth_sess_count(&s_sess, now_ms());
    unlock();
    ESP_LOGI(TAG, "signed in from %s (%d session%s)", caller_src(), n, n == 1 ? "" : "s");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "token", tok);
    cJSON_AddNumberToObject(r, "expires_in", (double)(AUTH_SESS_MS / 1000));
    auth_wipe(tok, sizeof tok);
    return r;
}

/* Auth.Logout {token?} - ends that session; by default the one the call came with: its token, or
 * the session a WebSocket signed in with (it keeps only the hash; web.c then drops the socket's
 * sign-in as well) */
static cJSON *m_logout(const cJSON *p, rpc_err_t *err)
{
    (void)err;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(p, "token");
    const rpc_current_t *c = rpc_current();
    const char *tok = cJSON_IsString(t) ? t->valuestring : c ? c->who.token : NULL;
    const uint8_t *session = !tok && c ? c->who.session : NULL;
    lock();
    const bool ended = tok ? auth_sess_remove(&s_sess, tok) : session ? auth_sess_remove_hash(&s_sess, session) : false;
    unlock();
    if (ended) ESP_LOGI(TAG, "signed out from %s", caller_src());
    return cJSON_CreateNull();
}

/* Device.SetAuth {pass: "<new PIN>" | "" | null, old_pass} -> {auth_en, pin, token?, expires_in?}
 * Sets, changes or removes the settings PIN (4-8 digits; a text password of before can only be
 * replaced by a PIN or removed). With one set, the current one is required (a stolen session must not be
 * able to lock the user out). Every session ends; setting one returns a new session for the caller, so
 * the page stays signed in. */
static cJSON *m_set_auth(const cJSON *p, rpc_err_t *err)
{
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(p, "pass");
    if (!cJSON_IsNull(pass) && !cJSON_IsString(pass))
        return rpc_fail(err, RPC_E_INVALID_ARG, "pass must be a string, or null to remove the PIN");
    const char *pw = cJSON_IsString(pass) && pass->valuestring[0] ? pass->valuestring : NULL;
    if (pw && !auth_pin_ok(pw)) return rpc_fail(err, RPC_E_INVALID_ARG, "The PIN must be 4 to 8 digits");
    const bool had = s_en;
    if (had) {
        const cJSON *old = cJSON_GetObjectItemCaseSensitive(p, "old_pass");
        if (!cJSON_IsString(old)) return rpc_fail(err, RPC_E_INVALID_ARG, "old_pass (the current PIN or password) is required");
        if (!verify_current(old->valuestring, "PIN change", err)) return NULL;
    }
    auth_pwv_t nw;
    if (pw) make_pwv(&nw, pw); /* PBKDF2, no lock held */
    if (store_pwv(pw ? &nw : NULL, pw != NULL) != ESP_OK) return rpc_fail(err, RPC_E_UNAVAILABLE, "could not store the PIN");
    char tok[AUTH_TOKEN_HEX + 1] = "";
    lock();
    if (pw) s_pw = nw;
    else memset(&s_pw, 0, sizeof s_pw);
    s_en = pw != NULL;
    s_pin = pw != NULL;
    auth_sess_clear(&s_sess);
    auth_lim_clear(&s_lim);
    if (pw) new_session_locked(tok);
    unlock();
    ESP_LOGW(TAG, "%s from %s", pw ? (had ? "settings PIN changed" : "settings PIN set") : "settings PIN removed", caller_src());
    webui_settings_saved(); /* the open pages show it */
    webui_deck_push_settings(); /* the decks lock their five actions with it, and unlock with it */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "auth_en", pw != NULL);
    cJSON_AddBoolToObject(r, "pin", pw != NULL);
    if (pw) {
        cJSON_AddStringToObject(r, "token", tok);
        cJSON_AddNumberToObject(r, "expires_in", (double)(AUTH_SESS_MS / 1000));
    }
    auth_wipe(tok, sizeof tok);
    return r;
}

/* An admin password stored as text by an older setup (boxcfg "adminPass"): hash it, drop the text. */
static void migrate_plain(void)
{
    char plain[sizeof ((box_config_t *)0)->admin_pass];
    webui_cfg_lock();
    strlcpy(plain, webui_cfg()->admin_pass, sizeof plain);
    webui_cfg_unlock();
    if (!plain[0]) return;
    if (!s_en) {
        auth_pwv_t pw;
        make_pwv(&pw, plain);
        if (store_pwv(&pw, false) != ESP_OK) { /* a text password: no PIN flag */
            auth_wipe(plain, sizeof plain);
            return; /* keep the text until the verifier is stored */
        }
        s_pw = pw;
        s_en = true;
        ESP_LOGW(TAG, "the settings password stored as text is now stored hashed");
    }
    auth_wipe(plain, sizeof plain);
    webui_cfg_lock();
    /* not over a stored config boxconfig keeps: then the text goes from RAM only */
    const auth_migrate_t plan = auth_migrate_plan(true, s_en, boxcfg_stored_kept());
    box_config_t *c = webui_cfg();
    esp_err_t e = ESP_OK;
    if (plan != AUTH_MIGRATE_NONE) memset(c->admin_pass, 0, sizeof c->admin_pass);
    if (plan == AUTH_MIGRATE_SAVE) e = boxcfg_save(c);
    webui_cfg_unlock();
    if (e != ESP_OK) ESP_LOGE(TAG, "removing the text password from the settings failed: %s", esp_err_to_name(e));
    if (plan == AUTH_MIGRATE_RAM_ONLY)
        ESP_LOGW(TAG, "the text password stays in the kept stored settings until settings are saved on the page");
}

void auth_init(void)
{
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof s_pw;
        if (nvs_get_blob(h, KEY_PWV, &s_pw, &len) == ESP_OK && len == sizeof s_pw && auth_pwv_valid(&s_pw)) {
            s_en = true;
            uint8_t pin = 0;
            s_pin = nvs_get_u8(h, KEY_PIN, &pin) == ESP_OK && pin == 1;
        } else {
            memset(&s_pw, 0, sizeof s_pw);
        }
        nvs_close(h);
    }
    migrate_plain();
    ESP_LOGI(TAG, "settings %s", !s_en ? "PIN not set (the API is open)" : s_pin ? "PIN set" : "password set (from before PINs: set a PIN to unlock the decks)");
    rpc_register("Auth.GetStatus", m_status, RPC_F_PUBLIC);
    rpc_register("Auth.Login", m_login, RPC_F_PUBLIC);
    rpc_register("Auth.Logout", m_logout, 0);
    rpc_register("Device.SetAuth", m_set_auth, 0);
}
