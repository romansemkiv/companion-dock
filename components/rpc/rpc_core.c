/* The pure part of the dispatcher (rpc.h): method table, access rule, frames and errors.
 * No transports here, no perf counters, no per-method stats. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpc/rpc.h"

typedef struct {
    const char *name;
    rpc_fn_t fn;
    uint32_t flags;
} method_t;

static method_t s_methods[RPC_MAX_METHODS];
static int s_n_methods;
static char s_device_id[48] = "companionbox";
static bool (*s_auth_enabled)(void);
static bool (*s_token_ok)(const char *);
static void (*s_lock)(void);
static void (*s_unlock)(void);
static void (*s_overflow)(const char *);
static bool s_overflowed;
static const rpc_current_t *s_cur; /* set only behind the lock, during a call */

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb || !ca) return ca - cb;
    }
}

static method_t *find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_n_methods; i++)
        if (!name_cmp(s_methods[i].name, name)) return &s_methods[i];
    return NULL;
}

void rpc_register(const char *name, rpc_fn_t fn, uint32_t flags)
{
    if (!name || !fn) return;
    method_t *m = find(name);
    if (m) { /* the same name again: the newer handler wins */
        *m = (method_t){.name = name, .fn = fn, .flags = flags};
        return;
    }
    if (s_n_methods >= RPC_MAX_METHODS) {
        if (!s_overflowed && s_overflow) s_overflow(name);
        s_overflowed = true;
        return;
    }
    s_methods[s_n_methods++] = (method_t){.name = name, .fn = fn, .flags = flags};
}

cJSON *rpc_fail(rpc_err_t *err, int code, const char *fmt, ...)
{
    if (!err) return NULL;
    err->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
    return NULL;
}

/* Brackets outside strings never nest deeper than max. */
static bool depth_ok(const char *s, int max)
{
    int depth = 0;
    bool in_str = false;
    for (; *s; s++) {
        if (in_str) {
            if (*s == '\\' && s[1]) s++;
            else if (*s == '"') in_str = false;
        } else if (*s == '"') {
            in_str = true;
        } else if (*s == '{' || *s == '[') {
            if (++depth > max) return false;
        } else if (*s == '}' || *s == ']') {
            depth--;
        }
    }
    return true;
}

cJSON *rpc_json_parse(const char *json)
{
    return json && depth_ok(json, RPC_JSON_DEPTH) ? cJSON_Parse(json) : NULL;
}

cJSON *rpc_query_value(const char *val)
{
    if (!val) val = "";
    cJSON *v = NULL;
    if (*val && strchr("{[\"-0123456789tfn", *val) && depth_ok(val, RPC_JSON_DEPTH))
        v = cJSON_ParseWithOpts(val, NULL, 1); /* the whole value, or nothing */
    return v ? v : cJSON_CreateString(val);
}

bool rpc_method_exists(const char *name) { return find(name) != NULL; }

bool rpc_method_is_public(const char *name)
{
    const method_t *m = find(name);
    return m && (m->flags & RPC_F_PUBLIC);
}

cJSON *rpc_list_methods(void)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < s_n_methods; i++) cJSON_AddItemToArray(a, cJSON_CreateString(s_methods[i].name));
    return a;
}

const rpc_current_t *rpc_current(void) { return s_cur; }

void rpc_set_device_id(const char *id)
{
    snprintf(s_device_id, sizeof s_device_id, "%s", id ? id : "");
}

const char *rpc_device_id(void) { return s_device_id; }

void rpc_set_auth(bool (*enabled)(void), bool (*token_ok)(const char *token))
{
    s_auth_enabled = enabled;
    s_token_ok = token_ok;
}

void rpc_set_lock(void (*lock)(void), void (*unlock)(void))
{
    s_lock = lock;
    s_unlock = unlock;
}

void rpc_set_overflow_hook(void (*hook)(const char *name)) { s_overflow = hook; }

void rpc_reset(void)
{
    memset(s_methods, 0, sizeof s_methods);
    s_n_methods = 0;
    s_overflowed = false;
    s_auth_enabled = NULL;
    s_token_ok = NULL;
    s_lock = s_unlock = NULL;
    s_overflow = NULL;
    s_cur = NULL;
    rpc_set_device_id("companionbox");
}

/* Runs a known method behind the lock. params NULL = {}. */
static cJSON *invoke(method_t *m, const cJSON *params, const rpc_current_t *cur, rpc_err_t *err)
{
    cJSON *empty = NULL;
    if (!params) params = empty = cJSON_CreateObject();
    if (s_lock) s_lock();
    err->code = 0;
    err->msg[0] = '\0';
    s_cur = cur;
    cJSON *r = m->fn(params, err);
    s_cur = NULL;
    if (s_unlock) s_unlock();
    cJSON_Delete(empty);
    if (r && err->code) { /* a handler that failed and still returned something */
        cJSON_Delete(r);
        r = NULL;
    }
    if (!r && !err->code) r = cJSON_CreateNull();
    return r;
}

/* The access rule and the call; token2 is a second credential (the frame's "token"). Protected
 * methods are open while no password is set; otherwise they need a valid token or a trusted caller. */
static cJSON *call(const char *method, const cJSON *params, const rpc_caller_t *who, const char *token2,
                   rpc_err_t *err)
{
    rpc_current_t cur = {.who = *who};
    if (!cur.who.src) cur.who.src = "local";
    const bool ok1 = who->token && s_token_ok && s_token_ok(who->token);
    const bool ok2 = !ok1 && token2 && s_token_ok && s_token_ok(token2);
    /* the handler sees the valid one (Auth.Logout ends it), else whichever was given */
    cur.who.token = ok1 ? who->token : ok2 ? token2 : who->token ? who->token : token2;
    cur.authed = who->trusted || !s_auth_enabled || !s_auth_enabled() || ok1 || ok2;
    method_t *m = find(method);
    if (!cur.authed && !(m && (m->flags & RPC_F_PUBLIC))) return rpc_fail(err, RPC_E_UNAUTHORIZED, "Unauthorized");
    if (!m) return rpc_fail(err, RPC_E_NO_METHOD, "No handler for %s", method);
    return invoke(m, params, &cur, err);
}

cJSON *rpc_call(const char *method, const cJSON *params, const rpc_caller_t *who, rpc_err_t *err)
{
    static const rpc_caller_t nobody = {0};
    if (!err) return NULL;
    err->code = 0;
    err->msg[0] = '\0';
    if (!method) return rpc_fail(err, RPC_E_INVALID_ARG, "method is required");
    if (params && !cJSON_IsObject(params)) return rpc_fail(err, RPC_E_INVALID_ARG, "params must be an object");
    return call(method, params, who ? who : &nobody, NULL, err);
}

char *rpc_handle_frame(const char *json, const char *token, const char *src)
{
    const rpc_caller_t who = {.token = token, .src = src};
    return rpc_handle_frame_as(json, &who);
}

char *rpc_handle_frame_as(const char *json, const rpc_caller_t *who_in)
{
    static const rpc_caller_t nobody = {0};
    const rpc_caller_t *who = who_in ? who_in : &nobody;
    cJSON *req = rpc_json_parse(json); /* untrusted and not yet authenticated */
    rpc_err_t err = {0};
    cJSON *result = NULL;
    const cJSON *id = NULL, *dst = NULL;

    if (!cJSON_IsObject(req)) {
        rpc_fail(&err, RPC_E_PARSE, "Invalid JSON");
    } else {
        id = cJSON_GetObjectItemCaseSensitive(req, "id");
        dst = cJSON_GetObjectItemCaseSensitive(req, "src");
        const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON *ftok = cJSON_GetObjectItemCaseSensitive(req, "token");
        if (!cJSON_IsString(method) || !method->valuestring[0]) {
            rpc_fail(&err, RPC_E_INVALID_ARG, "method is required");
        } else if (params && !cJSON_IsObject(params) && !cJSON_IsNull(params)) {
            rpc_fail(&err, RPC_E_INVALID_ARG, "params must be an object");
        } else {
            result = call(method->valuestring, cJSON_IsObject(params) ? params : NULL, who,
                          cJSON_IsString(ftok) ? ftok->valuestring : NULL, &err);
        }
    }

    char *out = NULL;
    if (!req || !cJSON_IsObject(req) || id) {
        cJSON *resp = cJSON_CreateObject();
        if (resp) {
            if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, false));
            cJSON_AddStringToObject(resp, "src", s_device_id);
            if (cJSON_IsString(dst) && dst->valuestring[0]) cJSON_AddStringToObject(resp, "dst", dst->valuestring);
            if (result) {
                cJSON_AddItemToObject(resp, "result", result);
                result = NULL;
            } else {
                cJSON *e = cJSON_AddObjectToObject(resp, "error");
                cJSON_AddNumberToObject(e, "code", err.code);
                cJSON_AddStringToObject(e, "message", err.msg);
            }
            out = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
        }
    }
    cJSON_Delete(result);
    cJSON_Delete(req);
    return out;
}
