/* JSON-RPC dispatcher. One method table that every transport
 * feeds: HTTP POST /rpc (frames), GET|POST /rpc/<Method> (REST), the WebSocket /rpc, and in-process
 * callers (the OTA self-check).
 *
 * Frames:
 *   request  {"id":1,"src":"client","method":"Sys.GetStatus","params":{...},"token":"..."}
 *   response {"id":1,"src":"<device id>","dst":"client","result":{...}}
 *            {"id":1,"src":"<device id>","dst":"client","error":{"code":-103,"message":"..."}}
 * "token" (a session token from Auth.Login) is optional: the transport may pass one too (HTTP
 * "Authorization: Bearer"). While no password is set every method is open; with a password only
 * RPC_F_PUBLIC methods answer without a valid token (401 otherwise).
 *
 * rpc_core.c is pure (frames, method table, errors; host-tested); rpc.c is the ESP glue (the
 * dispatcher lock and the device id). Handlers run one at a time behind the lock. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "rpc/refusals.h" /* RPC_E_INVALID_ARG, _BAD_STATE, _NOT_FOUND, _UNAVAILABLE, _UNAUTHORIZED: one place */

#ifdef __cplusplus
extern "C" {
#endif

#define RPC_E_NO_METHOD (-32601)
#define RPC_E_PARSE (-32700)

#define RPC_MAX_METHODS 48

typedef struct {
    int code;      /* 0 = no error */
    char msg[128];
} rpc_err_t;

/* A handler: returns the result (the dispatcher owns it), or NULL. NULL with err->code set is an
 * error; NULL without is a null result. params is always an object. */
typedef cJSON *(*rpc_fn_t)(const cJSON *params, rpc_err_t *err);

#define RPC_F_PUBLIC 0x01u /* callable without a token while a password is set */

/* Who is calling. */
typedef struct {
    const char *token; /* session token (Bearer header or "token" in the frame); NULL = none */
    const char *src;   /* the caller's address (client IP) for lockouts and logs; NULL = "local" */
    bool trusted;      /* vouched for by the process or the transport (the OTA self-check, a
                        * WebSocket signed in earlier): no token needed */
    const uint8_t *session; /* the SHA-256 of the session the transport signed the caller in with
                             * (32 bytes; a WebSocket keeps no token text), NULL none: Auth.Logout
                             * ends it when the call names no token */
} rpc_caller_t;

/* Adds a method. The name must stay valid for the program's life (a literal). Names match without
 * regard to case. A full table drops the method (logged once by rpc.c). */
void rpc_register(const char *name, rpc_fn_t fn, uint32_t flags);

/* Handles one request frame. token: the transport's credential (NULL none); a "token" string in the
 * frame counts as well. src: the caller's address (see rpc_caller_t). Returns the malloc'd response
 * frame (free with free()), or NULL for a request without "id" (a notification). */
char *rpc_handle_frame(const char *json, const char *token, const char *src);
/* The same with the whole caller (who->token as the transport's credential). */
char *rpc_handle_frame_as(const char *json, const rpc_caller_t *who);

/* Calls one method with params (NULL = {}), with the same access rule as frames. Returns the result
 * (caller deletes), or NULL with err set (a null result comes back as a cJSON null). */
cJSON *rpc_call(const char *method, const cJSON *params, const rpc_caller_t *who, rpc_err_t *err);

/* Inside a handler: the caller of the running call (NULL outside a call). authed says whether it
 * may call protected methods (no password set, a valid token, or trusted). */
typedef struct {
    rpc_caller_t who;
    bool authed;
} rpc_current_t;
const rpc_current_t *rpc_current(void);

/* Sets err and returns NULL, for `return rpc_fail(err, RPC_E_INVALID_ARG, "...")`. */
cJSON *rpc_fail(rpc_err_t *err, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* cJSON_Parse for untrusted text: NULL when it is not JSON or nests deeper than RPC_JSON_DEPTH
 * (cJSON's parser recurses per level; a deeply nested body would overflow the caller's stack before
 * anyone is authenticated). Free with cJSON_Delete. */
#define RPC_JSON_DEPTH 16
cJSON *rpc_json_parse(const char *json);
/* A REST query value (GET /rpc/<Method>?k=v): JSON when the whole value is JSON (trailing white space allowed;
 * cJSON_Parse alone takes a prefix: "10.0.0.5" would be 10), else the value as a string. NULL only without memory. */
cJSON *rpc_query_value(const char *val);

bool rpc_method_exists(const char *name);
bool rpc_method_is_public(const char *name);
/* ["Auth.GetStatus", ...] in registration order. */
cJSON *rpc_list_methods(void);

/* ---- set-up (rpc.c sets the lock and device id; webui sets the auth hooks) ---- */

/* "src" of every response frame. Copied. */
void rpc_set_device_id(const char *id);
const char *rpc_device_id(void);
/* enabled: a password is set. token_ok: a valid session token. NULL hooks = no password. */
void rpc_set_auth(bool (*enabled)(void), bool (*token_ok)(const char *token));
/* Serialises the handlers. NULL = no lock (single-threaded tests). */
void rpc_set_lock(void (*lock)(void), void (*unlock)(void));
/* Called once when the method table overflows (rpc.c logs it). */
void rpc_set_overflow_hook(void (*hook)(const char *name));
/* Empties the method table and drops the hooks (tests). */
void rpc_reset(void);

#ifdef ESP_PLATFORM
#include "esp_err.h"
/* Creates the dispatcher lock and sets the device id ("companionbox-<Ethernet MAC>"). Call once,
 * before any method runs; idempotent. */
esp_err_t rpc_init(void);
#endif

#ifdef __cplusplus
}
#endif
