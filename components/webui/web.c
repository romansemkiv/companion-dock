/* The HTTP server on port 80 (routes in webui.h).
 *
 * Browser protections (scripts, curl and Companion send none of the headers involved):
 *   - cross-site requests (Sec-Fetch-Site cross-site / same-site, or a foreign Origin) are refused
 *     unless they carry a Bearer token, which a page from elsewhere cannot add: no CSRF, no
 *     cross-site WebSocket hijacking. No CORS replies.
 *   - while no password is set, the Host header must be an IP address, a *.local name or the box's
 *     own host name: a DNS-rebinding page cannot reach the open API.
 *   - while no password is set, protected methods over GET /rpc/<Method> need an Authorization
 *     header (an <img> on another page in a browser that sends neither Sec-Fetch-Site nor Origin);
 *     POST works without.
 *   - the page is sent with a CSP, frame-ancestors 'none' and nosniff.
 * Authentication (auth.c): "Authorization: Bearer <session token>" on HTTP; on the WebSocket a
 * frame carrying a valid "token" signs the socket in, and only signed-in sockets get pushes while a
 * password is set.
 *
 * Everything here runs on the server's one task (handlers, queued work, the close callback), so the
 * WebSocket table and the OTA state need no lock; web_ws_listening is a plain counter. */
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "breadcrumb/breadcrumb.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "netmgr/netmgr.h"
#include "nvs.h"
#include "rpc/rpc.h"
#include "satclient/satclient.h"
#include "slotsync/slotsync.h"
#include "web_logic.h"
#include "webui/webui.h"
#include "ws_table.h"
#include "webui_priv.h"

static const char *TAG = "web";

static httpd_handle_t s_srv;

/* The page, gzip-compressed at build time (CMakeLists.txt). */
extern const char index_gz_start[] asm("_binary_index_html_gz_start");
extern const char index_gz_end[] asm("_binary_index_html_gz_end");
static char s_etag[24]; /* "<app ELF SHA-256 prefix>": new with every build */

#define MAX_WS WST_MAX
#define WS_SEND_TIMEOUT_S 2 /* a push to one client may hold up the server task this long */
#define WS_FRAME_MAX 8192
#define BODY_MAX 16384
#define RECV_TIMEOUT_S 5     /* one receive may wait this long (IDF's default) */
#define BODY_DEADLINE_US (5 * 1000000LL) /* a whole request body (<= 16 KB) must arrive within this */
#define OTA_STALL_TIMEOUTS 3 /* OTA: consecutive receive timeouts (5 s each) before giving up */
#define OTA_MIN_BYTES_PER_S 8192 /* OTA: overall deadline 30 s + size at this rate (slow-trickle guard) */
#define TOKEN_CAP 72
#define PEER_CAP 48
#define OTA_CHUNK 8192
#define MAX_SURFACES SURF_MAX_DECKS /* webui_decks lists at most this many */

/* One inline script with inline handlers ('unsafe-inline'); it may talk only to this box:
 * connect-src names its own WebSocket. */
#define CSP_HEAD                                                                                        \
    "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "        \
    "img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'; object-src 'none'; " \
    "connect-src 'self'"

static wst_t s_ws; /* the WebSocket clients (ws_table.c): server task only, counters read by notify */

/* ---------- request helpers ---------- */

static void ip4_str(uint32_t a, char *out, size_t n)
{
    const uint8_t *b = (const uint8_t *)&a; /* network order */
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* The client's address ("192.168.1.20"), for the sign-in lockout and logs. */
static void peer_addr(httpd_req_t *req, char *out, size_t n)
{
    struct sockaddr_in6 a;
    socklen_t l = sizeof a;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &l) != 0) {
        snprintf(out, n, "?");
        return;
    }
    if (a.sin6_family == AF_INET6) {
        const uint32_t *w = a.sin6_addr.un.u32_addr;
        if (!w[0] && !w[1] && w[2] == htonl(0xffff)) ip4_str(w[3], out, n); /* IPv4-mapped */
        else inet6_ntoa_r(a.sin6_addr, out, n);
    } else {
        ip4_str(((struct sockaddr_in *)&a)->sin_addr.s_addr, out, n);
    }
}

/* The socket's local IPv4 address (network order), the box's address the request came to; 0 if
 * unknown. */
static uint32_t local_ip4(httpd_req_t *req)
{
    struct sockaddr_in6 a;
    socklen_t l = sizeof a;
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &l) != 0) return 0;
    return a.sin6_family == AF_INET6 ? a.sin6_addr.un.u32_addr[3] /* IPv4-mapped */
                                     : ((struct sockaddr_in *)&a)->sin_addr.s_addr;
}

/* The request came in on the hotspot: the socket's local address is the AP interface's own (not
 * merely 192.168.4.x, which a LAN may use too). */
static bool on_ap(httpd_req_t *req)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (!ap || esp_netif_get_ip_info(ap, &info) != ESP_OK || !info.ip.addr) return false;
    return local_ip4(req) == info.ip.addr;
}

/* A header as the guards need it: absent only when it is not there at all. Any other failure,
 * a value longer than the buffer included, is "bad" and the guards refuse it. */
static wg_hdr_t hdr_get(httpd_req_t *req, const char *name, char *buf, size_t n)
{
    const esp_err_t e = httpd_req_get_hdr_value_str(req, name, buf, n);
    return wg_hdr(e == ESP_OK ? WG_READ_OK : e == ESP_ERR_NOT_FOUND ? WG_READ_ABSENT : WG_READ_FAILED, buf);
}

/* "Authorization: Bearer <token>" -> out; false without one. */
static bool bearer(httpd_req_t *req, char *out, size_t n)
{
    char hdr[TOKEN_CAP + 16];
    out[0] = '\0';
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) != ESP_OK) return false;
    if (strncasecmp(hdr, "Bearer ", 7)) return false;
    const char *t = hdr + 7;
    while (*t == ' ') t++;
    const size_t len = strlen(t);
    if (len == 0 || len >= n) return false; /* longer than the buffer: never a token */
    memcpy(out, t, len + 1);
    return true;
}

static bool has_auth_header(httpd_req_t *req) { return httpd_req_get_hdr_value_len(req, "Authorization") > 0; }

/* May call protected endpoints: no password set, or a valid Bearer token. */
static bool req_authed(httpd_req_t *req)
{
    if (!auth_enabled()) return true;
    char tok[TOKEN_CAP];
    return bearer(req, tok, sizeof tok) && auth_token_ok(tok);
}

/* Only names a DNS-rebinding page cannot use (web_logic.c): the check while no password is set, and for the
 * Satellite REST requests served without the PIN. */
static bool host_name_ok(httpd_req_t *req)
{
    char hp[96], name[sizeof ((box_config_t *)0)->box_name], bh[40];
    const wg_hdr_t host = hdr_get(req, "Host", hp, sizeof hp);
    webui_cfg_lock();
    strlcpy(name, webui_cfg()->box_name, sizeof name);
    webui_cfg_unlock();
    boxcfg_hostname(name, bh, sizeof bh);
    return wg_host_ok(host, bh, rpc_device_id());
}

static bool host_ok(httpd_req_t *req) { return auth_enabled() || host_name_ok(req); }

/* A request a page on another site made the browser send (web_logic.c). */
static bool cross_site(httpd_req_t *req)
{
    char sfs[24], origin[128], host[96];
    return wg_cross_site(hdr_get(req, "Sec-Fetch-Site", sfs, sizeof sfs), hdr_get(req, "Origin", origin, sizeof origin),
                         hdr_get(req, "Host", host, sizeof host));
}

static esp_err_t send_body(httpd_req_t *req, const char *status, const char *type, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    return send_body(req, status, "application/json", body);
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    return send_body(req, status, "text/plain; charset=utf-8", body);
}

/* o printed, deleted, and sent as JSON with status (fallback when it couldn't be printed). */
static esp_err_t send_cjson(httpd_req_t *req, const char *status, cJSON *o, const char *fallback)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    const esp_err_t r = send_json(req, status, s ? s : fallback);
    cJSON_free(s);
    return r;
}

/* {"code", "message"} with an HTTP status. */
static esp_err_t send_error(httpd_req_t *req, const char *status, int code, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", msg);
    return send_cjson(req, status, o, "{}");
}

static esp_err_t deny(httpd_req_t *req)
{
    return send_error(req, "401 Unauthorized", RPC_E_UNAUTHORIZED, "Unauthorized");
}

/* The browser protections for every API request; false (reply sent) when refused. */
static bool guard(httpd_req_t *req)
{
    if (!host_ok(req)) {
        send_error(req, "403 Forbidden", 403, "unknown host name: use the box's IP address or <name>.local");
        return false;
    }
    char tok[TOKEN_CAP];
    if (cross_site(req) && !bearer(req, tok, sizeof tok)) {
        send_error(req, "403 Forbidden", 403, "cross-site request refused");
        return false;
    }
    return true;
}

/* a Satellite REST request: guard, then how it is served (wg_api_access: signed in or no PIN, everything; else,
 * with companionAdopt on, the adopt subset). WG_API_DENY: refused, the reply sent. */
static wg_api_t guard_api(httpd_req_t *req)
{
    if (!guard(req)) return WG_API_DENY;
    webui_cfg_lock();
    const bool adopt = webui_cfg()->companion_adopt;
    webui_cfg_unlock();
    const bool secret = auth_enabled(), authed = req_authed(req);
    const wg_api_req_t r = {.post = req->method == HTTP_POST,
                            .uri = req->uri,
                            .secret_set = secret,
                            .authed = authed,
                            .credentials = has_auth_header(req),
                            .adopt = adopt,
                            .satellite = webui_run_mode() == BOX_SURFACE_SATELLITE,
                            .host_ok = secret && !authed && adopt && host_name_ok(req)};
    const wg_api_t a = wg_api_access(&r);
    if (a == WG_API_DENY) deny(req);
    return a;
}

/* guard, then the password (Bearer) when one is set; false (reply sent) when refused. */
static bool guard_authed(httpd_req_t *req)
{
    if (!guard(req)) return false;
    if (req_authed(req)) return true;
    deny(req);
    return false;
}

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* The whole body (at most max bytes), NUL-terminated, or NULL. */
static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len > max) return NULL;
    char *b = psram_alloc(req->content_len + 1);
    if (!b) return NULL;
    size_t got = 0;
    /* one deadline for the whole body: a client trickling a byte now and then must not hold the
     * server's only task */
    const int64_t deadline = esp_timer_get_time() + BODY_DEADLINE_US;
    while (got < req->content_len) {
        /* checked before each receive: a body the last chunk completes is kept */
        const int r = esp_timer_get_time() > deadline ? -1 : httpd_req_recv(req, b + got, req->content_len - got);
        if (r <= 0) { /* too slow, a timeout (5 s) or a close */
            free(b);
            return NULL;
        }
        got += (size_t)r;
    }
    b[got] = '\0';
    return b;
}

/* ?k=v&... -> params: a value that parses as JSON is taken as such, else as a string. */
static cJSON *query_to_params(httpd_req_t *req)
{
    cJSON *p = cJSON_CreateObject();
    const size_t ql = httpd_req_get_url_query_len(req);
    if (!ql || !p) return p;
    char *q = psram_alloc(ql + 1);
    if (!q) return p;
    if (httpd_req_get_url_query_str(req, q, ql + 1) != ESP_OK) q[0] = '\0';
    char *save = NULL;
    for (char *kv = strtok_r(q, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(kv, '=');
        char *val = eq ? eq + 1 : (char *)"";
        if (eq) *eq = '\0';
        web_url_decode(kv);
        web_url_decode(val);
        cJSON *v = rpc_query_value(val); /* JSON only when the whole value is */
        cJSON_DeleteItemFromObjectCaseSensitive(p, kv);
        cJSON_AddItemToObject(p, kv, v);
    }
    free(q);
    return p;
}

/* ---------- the page ---------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache"); /* revalidate: a reload is a 304 */
    httpd_resp_set_hdr(req, "ETag", s_etag);
    /* this host's WebSocket only (some browsers do not count ws: as 'self'); the Host header is
     * checked character by character, else 'self' alone */
    static char csp[sizeof CSP_HEAD + 80]; /* httpd keeps the pointer until the reply is sent */
    char host[64];
    bool ok = httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) == ESP_OK && host[0];
    for (const char *c = host; ok && *c; c++) ok = isalnum((unsigned char)*c) || strchr(".-:[]", *c);
    if (ok) snprintf(csp, sizeof csp, CSP_HEAD " ws://%s", host);
    else strlcpy(csp, CSP_HEAD, sizeof csp);
    httpd_resp_set_hdr(req, "Content-Security-Policy", csp);
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    char inm[64];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof inm) == ESP_OK && strstr(inm, s_etag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, index_gz_start, index_gz_end - index_gz_start);
}

/* ---------- JSON-RPC over HTTP ---------- */

/* POST /rpc: a frame. The Bearer header counts, and so does "token" in the frame (401 in the reply). */
static esp_err_t h_rpc_frame(httpd_req_t *req)
{
    if (!guard(req)) return ESP_OK;
    char *b = read_body(req, BODY_MAX);
    if (!b) return send_error(req, "400 Bad Request", RPC_E_PARSE, "bad body");
    char tok[TOKEN_CAP], peer[PEER_CAP];
    bearer(req, tok, sizeof tok);
    peer_addr(req, peer, sizeof peer);
    char *out = rpc_handle_frame(b, tok[0] ? tok : NULL, peer);
    free(b);
    const esp_err_t r = send_json(req, "200 OK", out ? out : "{}");
    free(out);
    return r;
}

/* GET|POST /rpc/<Method>: the result, or {"code","message"} with 400/401/404/503. */
static esp_err_t h_rpc_method(httpd_req_t *req)
{
    if (!guard(req)) return ESP_OK;
    const char *m = req->uri + 5; /* after "/rpc/" */
    char method[48];
    const size_t n = strcspn(m, "?");
    if (n == 0 || n >= sizeof method) return send_error(req, "404 Not Found", RPC_E_NO_METHOD, "no method");
    memcpy(method, m, n);
    method[n] = '\0';
    const bool pub = rpc_method_is_public(method);
    if (!pub && !req_authed(req)) return deny(req);
    if (req->method == HTTP_GET && !auth_enabled() && !pub && rpc_method_exists(method) && !has_auth_header(req))
        return send_error(req, "405 Method Not Allowed", 405, "use POST while the box has no password");

    cJSON *params;
    if (req->method == HTTP_POST && req->content_len) {
        char *b = read_body(req, BODY_MAX);
        params = b ? rpc_json_parse(b) : NULL;
        free(b);
        if (!cJSON_IsObject(params)) {
            cJSON_Delete(params);
            return send_error(req, "400 Bad Request", RPC_E_PARSE, "body must be a JSON object");
        }
    } else {
        params = query_to_params(req);
    }
    char tok[TOKEN_CAP], peer[PEER_CAP];
    bearer(req, tok, sizeof tok);
    peer_addr(req, peer, sizeof peer);
    const rpc_caller_t who = {.token = tok[0] ? tok : NULL, .src = peer};
    rpc_err_t err = {0};
    cJSON *res = rpc_call(method, params, &who, &err);
    cJSON_Delete(params);
    const char *status = "200 OK";
    if (!res) {
        status = err.code == RPC_E_NO_METHOD     ? "404 Not Found"
                 : err.code == RPC_E_UNAUTHORIZED ? "401 Unauthorized"
                 : err.code == RPC_E_UNAVAILABLE  ? "503 Service Unavailable"
                                                  : "400 Bad Request";
        return send_error(req, status, err.code, err.msg);
    }
    return send_cjson(req, status, res, "null");
}

/* ---------- WebSocket /rpc ---------- */

/* Someone can receive pushes: any socket while the API is open, else a signed-in one (sockets
 * that never sign in do not make the notifier build the status every second). */
bool web_ws_listening(void) { return wst_listening(&s_ws, auth_enabled()); }

/* Signs the socket in with a live session's token (only its hash is kept); true when new. */
static bool ws_sign_in(wst_slot_t *w, const char *token)
{
    uint8_t h[32];
    return w && token && auth_token_digest(token, h) && wst_sign_in(&s_ws, w, h);
}

/* The socket's session is still live; one that ended (logout, expiry, password change) drops the
 * socket's sign-in, so it stops getting pushes and counting as a listener. */
static bool ws_authed(wst_slot_t *w) { return wst_authed(&s_ws, w, auth_digest_ok); }

/* A frame's "token", when it is valid, signs the socket in (for its later frames and pushes). */
static void ws_take_token(wst_slot_t *w, const char *frame)
{
    cJSON *j = rpc_json_parse(frame);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "token");
    if (cJSON_IsString(t) && ws_sign_in(w, t->valuestring)) notify_kick(); /* newly signed in: push to it */
    cJSON_Delete(j);
}

/* A socket that cannot take a push is closed; on_close then forgets it, and the page reconnects. The
 * close is queued as server work. If the queue is full, the socket is shut down so the server sees
 * it close by itself; if the server no longer knows the socket, it is gone already. */
static void ws_close(wst_slot_t *w)
{
    if (w->closing) return;
    w->closing = true;
    const esp_err_t e = httpd_sess_trigger_close(s_srv, w->fd);
    if (e == ESP_ERR_NOT_FOUND) wst_remove(&s_ws, w->fd);
    else if (e != ESP_OK) shutdown(w->fd, SHUT_RDWR);
}

/* Before the 101 reply (IDF calls the URI handler only for frames, never for the handshake). The
 * upgrade needs no credentials (browsers cannot add headers to it), but it must come from this box's
 * own page or a non-browser client; ESP_FAIL closes the socket. */
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (!host_ok(req) || cross_site(req)) {
        ESP_LOGW(TAG, "WebSocket from another site refused");
        return ESP_FAIL;
    }
    const int fd = httpd_req_to_sockfd(req);
    wst_slot_t *w = wst_add(&s_ws, fd);
    if (!w) {
        ESP_LOGW(TAG, "WebSocket refused: %d clients already", MAX_WS);
        return ESP_FAIL;
    }
    /* pushes run on the one server task: a client that stops reading holds it up this long */
    const struct timeval tv = {.tv_sec = WS_SEND_TIMEOUT_S};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char tok[TOKEN_CAP];
    if (bearer(req, tok, sizeof tok)) ws_sign_in(w, tok);
    notify_kick();
    return ESP_OK;
}

/* A frame on a WebSocket that passed ws_pre_handshake. */
static esp_err_t h_ws(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t f = {0};
    esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
    if (e != ESP_OK) return e;
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        wst_remove(&s_ws, fd);
        return ESP_OK;
    }
    if (f.len == 0) return ESP_OK;
    /* the payload is still in the socket: returning without reading it would make httpd parse it as
     * the next frame's header, so anything else closes the socket */
    if (f.type != HTTPD_WS_TYPE_TEXT || f.len > WS_FRAME_MAX) {
        wst_remove(&s_ws, fd);
        return ESP_FAIL;
    }
    uint8_t *buf = psram_alloc(f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = buf;
    e = httpd_ws_recv_frame(req, &f, f.len);
    if (e == ESP_OK) {
        buf[f.len] = '\0';
        wst_slot_t *w = wst_find(&s_ws, fd);
        if (w) ws_take_token(w, (const char *)buf);
        char peer[PEER_CAP];
        peer_addr(req, peer, sizeof peer);
        /* a signed-in socket is vouched for by the transport, and its session goes with the call
         * (Auth.Logout without a token ends it); "token" in the frame counts too */
        const bool authed = ws_authed(w);
        uint8_t session[32];
        if (authed) memcpy(session, w->hash, sizeof session);
        const rpc_caller_t who = {.src = peer, .trusted = authed, .session = authed ? session : NULL};
        char *out = rpc_handle_frame_as((const char *)buf, &who);
        if (authed) ws_authed(w); /* a logout in this call drops the socket's sign-in now */
        if (out) {
            httpd_ws_frame_t r = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)out, .len = strlen(out), .final = true};
            e = httpd_ws_send_frame(req, &r);
            free(out);
        }
    }
    free(buf);
    return e;
}

void web_ws_broadcast_now(const char *text)
{
    if (!s_srv || !text) return;
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text), .final = true};
    const bool open = !auth_enabled();
    for (int i = 0; i < MAX_WS; i++) {
        wst_slot_t *w = &s_ws.s[i];
        if (!w->fd || w->closing) continue;
        if (!open && !ws_authed(w)) continue; /* not signed in, signed out or expired */
        if (httpd_ws_get_fd_info(s_srv, w->fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
            httpd_ws_send_frame_async(s_srv, w->fd, &f) != ESP_OK)
            ws_close(w); /* maybe half a frame went out: the session cannot go on */
    }
}

esp_err_t web_queue_work(void (*fn)(void *), void *arg)
{
    return s_srv ? httpd_queue_work(s_srv, fn, arg) : ESP_ERR_INVALID_STATE;
}

/* ---------- POST /ota ---------- */

typedef enum { OTA_IDLE, OTA_WRITING, OTA_DONE, OTA_ERROR } ota_state_t;

static struct {
    ota_state_t state;
    size_t done, total;
    char msg[112];
    char version[33]; /* of the image being written / written */
} s_ota;
static atomic_bool s_ota_busy; /* writing or done (restarting): webui_ota_busy, read by the supervisor */
static bool s_ota_claimed;     /* this upload holds the flash against a recovery-slot sync (slotsync) */

bool webui_ota_busy(void) { return atomic_load(&s_ota_busy); }

#define APP_DESC_OFF (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HDR_LEN (APP_DESC_OFF + sizeof(esp_app_desc_t))

cJSON *web_ota_status(void)
{
    static const char *const names[] = {"idle", "writing", "done", "error"};
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const bool pending = webui_pending_verify();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "state", names[s_ota.state]);
    cJSON_AddStringToObject(r, "message", s_ota.msg);
    cJSON_AddNumberToObject(r, "done", (double)s_ota.done);
    cJSON_AddNumberToObject(r, "total", (double)s_ota.total);
    cJSON_AddStringToObject(r, "new_version", s_ota.version);
    cJSON_AddStringToObject(r, "running", run ? run->label : "");
    cJSON_AddStringToObject(r, "version", esp_app_get_description()->version);
    cJSON_AddBoolToObject(r, "pending_verify", pending);
    cJSON_AddStringToObject(r, "next", next ? next->label : "");
    cJSON_AddNumberToObject(r, "slot_size", next ? (double)next->size : 0);
    /* the recovery slot: {state, pct, other: {label, project, version, date, time, valid, foreign}};
     * valid: a valid image of this project, which Ota.BootOther may take; foreign: another project's
     * image or one without an app descriptor, shown as "invalid" */
    slotsync_status_t ss;
    slotsync_get(&ss);
    cJSON *sync = cJSON_AddObjectToObject(r, "sync"), *oth = cJSON_CreateObject();
    cJSON_AddStringToObject(sync, "state", slotsync_state_name(ss.state));
    cJSON_AddNumberToObject(sync, "pct", ss.pct);
    cJSON_AddStringToObject(oth, "label", ss.other);
    cJSON_AddStringToObject(oth, "project", ss.other_project);
    cJSON_AddStringToObject(oth, "version", ss.other_version);
    cJSON_AddStringToObject(oth, "date", ss.other_date);
    cJSON_AddStringToObject(oth, "time", ss.other_time);
    cJSON_AddBoolToObject(oth, "valid", ss.other_valid);
    cJSON_AddBoolToObject(oth, "foreign", ss.other_foreign);
    cJSON_AddItemToObject(sync, "other", oth);
    return r;
}

static esp_err_t ota_refuse(httpd_req_t *req, const char *status, int code, const char *why)
{
    s_ota.state = OTA_ERROR;
    atomic_store(&s_ota_busy, false);
    if (s_ota_claimed) { /* a held sync goes on; the other slot is checked again first */
        s_ota_claimed = false;
        slotsync_flash_release();
    }
    strlcpy(s_ota.msg, why, sizeof s_ota.msg);
    ESP_LOGE(TAG, "firmware upload refused: %s", why);
    return send_error(req, status, code, why);
}

/* The image's app descriptor: NULL when it belongs here, else why not. */
static const char *check_image(const uint8_t *hdr)
{
    static char why[112];
    esp_app_desc_t d;
    memcpy(&d, hdr + APP_DESC_OFF, sizeof d);
    if (d.magic_word != ESP_APP_DESC_MAGIC_WORD) return "not an ESP-IDF application image";
    const esp_app_desc_t *run = esp_app_get_description();
    if (strncmp(d.project_name, run->project_name, sizeof d.project_name)) {
        snprintf(why, sizeof why, "this image is for another project (%.32s, not %.32s)", d.project_name,
                 run->project_name);
        return why;
    }
    strlcpy(s_ota.version, d.version, sizeof s_ota.version);
    return NULL;
}

/* ---- the upload's interface, for the new image's self-check ----
 * NVS rather than RTC memory: another image's RTC variables may sit elsewhere, and NVS also survives
 * a power cut between the upload and the first start. */
#define OTA_NS "ota"
#define OTA_KEY_VIA "via"

static void ota_via_record(netmgr_if_t via)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(OTA_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = via != NETMGR_IF_NONE ? nvs_set_u8(h, OTA_KEY_VIA, (uint8_t)via) : nvs_erase_key(h, OTA_KEY_VIA);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "upload interface not recorded (%s): the self-check asks for any network", esp_err_to_name(e));
}

netmgr_if_t webui_ota_via_take(void)
{
    nvs_handle_t h;
    uint8_t v = NETMGR_IF_NONE;
    if (nvs_open(OTA_NS, NVS_READWRITE, &h) != ESP_OK) return NETMGR_IF_NONE;
    if (nvs_get_u8(h, OTA_KEY_VIA, &v) == ESP_OK) {
        nvs_erase_key(h, OTA_KEY_VIA);
        nvs_commit(h);
    } else {
        v = NETMGR_IF_NONE;
    }
    nvs_close(h);
    return v == NETMGR_IF_ETH || v == NETMGR_IF_STA || v == NETMGR_IF_AP ? (netmgr_if_t)v : NETMGR_IF_NONE;
}

static const char *iface_name(netmgr_if_t i)
{
    return i == NETMGR_IF_ETH ? "eth" : i == NETMGR_IF_STA ? "sta" : i == NETMGR_IF_AP ? "ap" : "unknown";
}

/* POST /ota, body = the raw .bin: written to the other slot, checked, made the boot slot; then the
 * reply and a restart a second later (the new image confirms itself with the supervisor's check,
 * which also wants the interface the upload came in on back: recorded here). A recovery-slot sync
 * writes the same slot: the upload claims the flash first (409 while a sync runs) and keeps it until
 * the restart, or gives it back when it fails (ota_refuse). */
static esp_err_t h_ota(httpd_req_t *req)
{
    if (!guard_authed(req)) return ESP_OK;
    if (s_ota.state == OTA_WRITING || s_ota.state == OTA_DONE)
        return send_error(req, "409 Conflict", RPC_E_BAD_STATE, "an update is already in progress or done");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ota_refuse(req, "500 Internal Server Error", RPC_E_UNAVAILABLE, "no OTA slot to write");
    if (req->content_len < HDR_LEN) return ota_refuse(req, "400 Bad Request", RPC_E_INVALID_ARG, "image is too short");
    if (req->content_len > part->size)
        return ota_refuse(req, "400 Bad Request", RPC_E_INVALID_ARG, "image is larger than the OTA slot");
    const esp_err_t ce = slotsync_flash_claim();
    if (ce != ESP_OK) {
        ESP_LOGW(TAG, "firmware upload refused: %s", slotsync_err_text(ce));
        return send_error(req, "409 Conflict", RPC_E_BAD_STATE, slotsync_err_text(ce));
    }
    s_ota_claimed = true;
    uint8_t *buf = psram_alloc(OTA_CHUNK);
    if (!buf) return ota_refuse(req, "500 Internal Server Error", RPC_E_UNAVAILABLE, "no memory");

    memset(&s_ota, 0, sizeof s_ota);
    s_ota.state = OTA_WRITING;
    atomic_store(&s_ota_busy, true);
    s_ota.total = req->content_len;
    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        free(buf);
        return ota_refuse(req, "409 Conflict", RPC_E_BAD_STATE,
                          "the running firmware is still checking itself (about 30 s after start): try again shortly");
    }
    if (e != ESP_OK) {
        free(buf);
        return ota_refuse(req, "500 Internal Server Error", RPC_E_UNAVAILABLE, esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "firmware upload: %u bytes to %s", (unsigned)req->content_len, part->label);
    static uint8_t hdr[HDR_LEN]; /* nothing reaches the slot before the descriptor is checked */
    size_t hdr_n = 0, left = req->content_len;
    int timeouts = 0;
    const char *why = NULL;
    /* a stall of 15 s, or a rate too slow to finish by the deadline, ends the upload: the server
     * has one task, and a trickling client must not hold it */
    const int64_t deadline =
        esp_timer_get_time() + (30 + (int64_t)req->content_len / OTA_MIN_BYTES_PER_S) * 1000000LL;
    while (left > 0 && !why) {
        if (esp_timer_get_time() > deadline) {
            why = "upload too slow";
            break;
        }
        const int r = httpd_req_recv(req, (char *)buf, left < OTA_CHUNK ? left : OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < OTA_STALL_TIMEOUTS) continue;
        if (r <= 0) {
            why = "upload interrupted";
            break;
        }
        timeouts = 0;
        left -= (size_t)r;
        const uint8_t *p = buf;
        size_t n = (size_t)r;
        if (hdr_n < HDR_LEN) {
            const size_t k = n < HDR_LEN - hdr_n ? n : HDR_LEN - hdr_n;
            memcpy(hdr + hdr_n, p, k);
            hdr_n += k;
            p += k;
            n -= k;
            if (hdr_n < HDR_LEN) continue;
            why = check_image(hdr);
            if (why) break;
            e = esp_ota_write(h, hdr, HDR_LEN);
            if (e != ESP_OK) {
                why = e == ESP_ERR_OTA_VALIDATE_FAILED ? "not a firmware image for this chip" : esp_err_to_name(e);
                break;
            }
        }
        if (n && (e = esp_ota_write(h, p, n)) != ESP_OK) {
            why = esp_err_to_name(e);
            break;
        }
        s_ota.done = req->content_len - left;
    }
    free(buf);
    if (why) {
        esp_ota_abort(h);
        return ota_refuse(req, "400 Bad Request", RPC_E_INVALID_ARG, why);
    }
    e = esp_ota_end(h); /* verifies the image */
    if (e != ESP_OK)
        return ota_refuse(req, "400 Bad Request", RPC_E_INVALID_ARG,
                          e == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed verification" : esp_err_to_name(e));
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) return ota_refuse(req, "500 Internal Server Error", RPC_E_UNAVAILABLE, esp_err_to_name(e));
    s_ota.state = OTA_DONE;
    strlcpy(s_ota.msg, "rebooting into the new firmware", sizeof s_ota.msg);
    const netmgr_if_t via = netmgr_iface_of_addr(local_ip4(req));
    ota_via_record(via);
    ESP_LOGW(TAG, "firmware %s written to %s (came in on %s): restarting", s_ota.version, part->label, iface_name(via));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "message", s_ota.msg);
    cJSON_AddStringToObject(o, "version", s_ota.version);
    const esp_err_t r = send_cjson(req, "200 OK", o, "{\"ok\":true}");
    const esp_err_t re = webui_restart_later(false, BC_WHY_OTA);
    if (re == ESP_ERR_NO_MEM) { /* the reply is out; restart from here rather than not at all */
        ESP_LOGW(TAG, "no memory for the restart task: restarting from the server task");
        vTaskDelay(pdMS_TO_TICKS(1000));
        breadcrumb_restart(BC_WHY_OTA);
    } else if (re != ESP_OK) {
        ESP_LOGW(TAG, "a restart was already on its way");
    }
    return r;
}

/* ---------- Companion Satellite REST (companion-satellite satellite/src/rest.ts) ---------- */

static bool content_is_json(httpd_req_t *req)
{
    char ct[64];
    return httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) == ESP_OK &&
           !strncasecmp(ct, "application/json", 16);
}

/* The current /api/config object (ConfigData). */
static char *satellite_config(void)
{
    webui_cfg_lock();
    char *s = boxcfg_satellite_config_json(webui_cfg());
    webui_cfg_unlock();
    return s;
}

static esp_err_t send_satellite_config(httpd_req_t *req)
{
    char *s = satellite_config();
    const esp_err_t r = send_json(req, "200 OK", s ? s : "{}");
    free(s);
    return r;
}

/* The client's address when access is the adopt (no PIN), for webui_apply_satellite; else NULL. */
static const char *adopter(httpd_req_t *req, wg_api_t access, char *buf, size_t n)
{
    if (access != WG_API_ADOPT) return NULL;
    peer_addr(req, buf, n);
    return buf;
}

/* Applies {"<key>": value} (host or port: the Companion target, under adopt too); "OK", or 400 with "Invalid <what>"
 * (the reference's replies). */
static esp_err_t satellite_set(httpd_req_t *req, cJSON *update, const char *what, wg_api_t access)
{
    wg_api_body(update, access);
    const bool has_host = cJSON_GetObjectItemCaseSensitive(update, "host") != NULL;
    const bool has_port = cJSON_GetObjectItemCaseSensitive(update, "port") != NULL;
    char *json = cJSON_PrintUnformatted(update);
    cJSON_Delete(update);
    char err[32] = "", by[48];
    const bool ok = json && webui_apply_satellite(json, has_host, has_port, adopter(req, access, by, sizeof by), err, sizeof err);
    cJSON_free(json);
    if (ok) return send_text(req, "200 OK", "OK");
    if (!strcmp(err, "save")) return send_text(req, "500 Internal Server Error", "Saving failed");
    if (!strcmp(err, "limit")) return send_text(req, "429 Too Many Requests", "Too many changes");
    char msg[48];
    snprintf(msg, sizeof msg, "Invalid %s", what);
    return send_text(req, "400 Bad Request", msg);
}

/* The body's "<key>" (JSON body) or the whole body (text): a malloc'd string, or NULL. */
static char *body_field(httpd_req_t *req, const char *key)
{
    char *b = read_body(req, 1024);
    if (!b || !content_is_json(req)) return b;
    cJSON *j = rpc_json_parse(b);
    free(b);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    char *out = NULL;
    if (cJSON_IsString(v)) out = strdup(v->valuestring);
    else if (cJSON_IsNumber(v)) out = cJSON_PrintUnformatted(v);
    cJSON_Delete(j);
    return out;
}

/* the GETs hold nothing secret (the Companion target, whether it is connected and its version, ConfigData: the
 * box name and mDNS setting, which mDNS announces anyway, and the decks' serials and models, which Companion is given
 * anyway): with companionAdopt they answer without the PIN, every field included. */
static esp_err_t h_api_get_host(httpd_req_t *req)
{
    if (guard_api(req) == WG_API_DENY) return ESP_OK;
    char host[sizeof ((box_config_t *)0)->companion_host];
    webui_cfg_lock();
    strlcpy(host, webui_cfg()->companion_host, sizeof host);
    webui_cfg_unlock();
    return send_text(req, "200 OK", host);
}

static esp_err_t h_api_get_port(httpd_req_t *req)
{
    if (guard_api(req) == WG_API_DENY) return ESP_OK;
    webui_cfg_lock();
    const unsigned port = webui_cfg()->companion_port;
    webui_cfg_unlock();
    char s[8];
    snprintf(s, sizeof s, "%u", port);
    return send_json(req, "200 OK", s);
}

static esp_err_t h_api_get_connected(httpd_req_t *req)
{
    if (guard_api(req) == WG_API_DENY) return ESP_OK;
    return send_json(req, "200 OK", satclient_state() == SATC_CONNECTED ? "true" : "false");
}

static esp_err_t h_api_get_config(httpd_req_t *req)
{
    if (guard_api(req) == WG_API_DENY) return ESP_OK;
    return send_satellite_config(req);
}

/* StatusResponse: {connected, companionVersion, companionApiVersion, companionUnsupportedApi} */
static esp_err_t h_api_get_status(httpd_req_t *req)
{
    if (guard_api(req) == WG_API_DENY) return ESP_OK;
    cJSON *c = webui_companion_status();
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(c, "state"));
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(c, "version"));
    const char *api = cJSON_GetStringValue(cJSON_GetObjectItem(c, "api_version"));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "connected", st && !strcmp(st, "connected"));
    if (ver && ver[0]) cJSON_AddStringToObject(o, "companionVersion", ver);
    else cJSON_AddNullToObject(o, "companionVersion");
    if (api && api[0]) cJSON_AddStringToObject(o, "companionApiVersion", api);
    else cJSON_AddNullToObject(o, "companionApiVersion");
    cJSON_AddBoolToObject(o, "companionUnsupportedApi", st && !strcmp(st, "too_old"));
    cJSON_Delete(c);
    return send_cjson(req, "200 OK", o, "{}");
}

static int surface_cmp(const void *a, const void *b)
{
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    const char *sx = cJSON_GetStringValue(cJSON_GetObjectItem(x, "surfaceId")); /* NULL when memory ran out */
    const char *sy = cJSON_GetStringValue(cJSON_GetObjectItem(y, "surfaceId"));
    return strcmp(sx ? sx : "", sy ? sy : "");
}

/* SurfaceInfo[]: {surfaceId, productName, pluginId, pluginName}, sorted by surfaceId */
static esp_err_t h_api_get_surfaces(httpd_req_t *req)
{
    if (guard_api(req) == WG_API_DENY) return ESP_OK;
    cJSON *decks = webui_decks();
    cJSON *items[MAX_SURFACES];
    int k = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, decks) {
        if (k >= (int)(sizeof items / sizeof *items)) break;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "surfaceId", cJSON_GetStringValue(cJSON_GetObjectItem(d, "device_id")));
        cJSON_AddStringToObject(o, "productName", cJSON_GetStringValue(cJSON_GetObjectItem(d, "model")));
        cJSON_AddStringToObject(o, "pluginId", "elgato-stream-deck");
        cJSON_AddStringToObject(o, "pluginName", "Elgato Stream Deck");
        items[k++] = o;
    }
    cJSON_Delete(decks);
    qsort(items, (size_t)k, sizeof *items, surface_cmp);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < k; i++) cJSON_AddItemToArray(a, items[i]);
    return send_cjson(req, "200 OK", a, "[]");
}

static esp_err_t h_api_post_host(httpd_req_t *req)
{
    const wg_api_t a = guard_api(req);
    if (a == WG_API_DENY) return ESP_OK;
    char *v = body_field(req, "host");
    char host[72] = "";
    if (v) web_sanitize_host(v, host, sizeof host);
    free(v);
    if (!host[0] || (a == WG_API_ADOPT && !wg_adopt_host(host))) return send_text(req, "400 Bad Request", "Invalid host");
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "host", host);
    return satellite_set(req, u, "host", a);
}

static esp_err_t h_api_post_port(httpd_req_t *req)
{
    const wg_api_t a = guard_api(req);
    if (a == WG_API_DENY) return ESP_OK;
    char *v = body_field(req, "port");
    char *end = NULL;
    const double port = v && v[0] ? strtod(v, &end) : -1;
    while (end && isspace((unsigned char)*end)) end++; /* like Number(): surrounding blanks are fine */
    const bool ok = v && end && end != v && !*end;
    free(v);
    if (!ok) return send_text(req, "400 Bad Request", "Invalid port");
    cJSON *u = cJSON_CreateObject();
    cJSON_AddNumberToObject(u, "port", port);
    return satellite_set(req, u, "port", a);
}

/* POST /api/config (ConfigDataUpdate) -> ConfigData. httpEnabled / httpPort / wsAddress are ignored;
 * protocol must be "tcp" (the box has no WebSocket client). without the PIN (companionAdopt), only host and
 * port apply (wg_api_body: any other field ignored, never an error), validated and applied as when signed in. */
static esp_err_t h_api_post_config(httpd_req_t *req)
{
    const wg_api_t a = guard_api(req);
    if (a == WG_API_DENY) return ESP_OK;
    char *b = read_body(req, 2048);
    cJSON *j = b ? rpc_json_parse(b) : NULL;
    free(b);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return send_text(req, "400 Bad Request", "Invalid request");
    }
    wg_api_body(j, a);
    cJSON *host = cJSON_GetObjectItemCaseSensitive(j, "host");
    if (cJSON_IsString(host)) {
        char h[72];
        web_sanitize_host(host->valuestring, h, sizeof h);
        cJSON_SetValuestring(host, h);
    }
    if (a == WG_API_ADOPT && host && !(cJSON_IsString(host) && wg_adopt_host(host->valuestring))) {
        cJSON_Delete(j);
        return send_text(req, "400 Bad Request", "Invalid host");
    }
    const bool has_host = host != NULL, has_port = cJSON_GetObjectItemCaseSensitive(j, "port") != NULL;
    char *json = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    char err[32] = "", by[48];
    const bool ok = json && webui_apply_satellite(json, has_host, has_port, adopter(req, a, by, sizeof by), err, sizeof err);
    cJSON_free(json);
    if (!ok) {
        if (!strcmp(err, "save")) return send_text(req, "500 Internal Server Error", "Saving failed");
        if (!strcmp(err, "limit")) return send_text(req, "429 Too Many Requests", "Too many changes");
        char msg[48];
        snprintf(msg, sizeof msg, "Invalid %s", err[0] ? err : "request");
        return send_text(req, "400 Bad Request", msg);
    }
    return send_satellite_config(req);
}

/* Decks attach by themselves (USB host events): nothing to scan. */
static esp_err_t h_api_post_rescan(httpd_req_t *req)
{
    if (guard_api(req) == WG_API_DENY) return ESP_OK;
    return send_text(req, "200 OK", "OK");
}

/* ---------- captive portal ---------- */

static esp_err_t redirect_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WEBUI_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_captive(httpd_req_t *req) { return redirect_root(req); }

/* Any other GET on the hotspot goes to the page (phones' captive-portal checks). */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    if (req->method == HTTP_GET && on_ap(req)) return redirect_root(req);
    return send_text(req, "404 Not Found", "Not found");
}

static void on_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    wst_remove(&s_ws, fd);
    close(fd);
}

/* ---------- start ---------- */

esp_err_t web_start(void)
{
    if (s_srv) return ESP_OK;
    char sha[17];
    esp_app_get_elf_sha256(sha, sizeof sha);
    snprintf(s_etag, sizeof s_etag, "\"%s\"", sha);

    static const httpd_uri_t uris[] = {
        {.uri = "/rpc", .method = HTTP_GET, .handler = h_ws, .is_websocket = true, .ws_pre_handshake_cb = ws_pre_handshake},
        {.uri = "/rpc", .method = HTTP_POST, .handler = h_rpc_frame},
        {.uri = "/rpc/*", .method = HTTP_GET, .handler = h_rpc_method},
        {.uri = "/rpc/*", .method = HTTP_POST, .handler = h_rpc_method},
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = h_index},
        {.uri = "/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/api/host", .method = HTTP_GET, .handler = h_api_get_host},
        {.uri = "/api/host", .method = HTTP_POST, .handler = h_api_post_host},
        {.uri = "/api/port", .method = HTTP_GET, .handler = h_api_get_port},
        {.uri = "/api/port", .method = HTTP_POST, .handler = h_api_post_port},
        {.uri = "/api/connected", .method = HTTP_GET, .handler = h_api_get_connected},
        {.uri = "/api/config", .method = HTTP_GET, .handler = h_api_get_config},
        {.uri = "/api/config", .method = HTTP_POST, .handler = h_api_post_config},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_api_get_status},
        {.uri = "/api/surfaces", .method = HTTP_GET, .handler = h_api_get_surfaces},
        {.uri = "/api/surfaces/rescan", .method = HTTP_POST, .handler = h_api_post_rescan},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = h_captive},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = h_captive},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = h_captive},
        {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = h_captive},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = h_captive},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = h_captive},
        {.uri = "/canonical.html", .method = HTTP_GET, .handler = h_captive},
        {.uri = "/success.txt", .method = HTTP_GET, .handler = h_captive},
    };
    const size_t nuris = sizeof uris / sizeof *uris;

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = nuris + 4;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.stack_size = 8192;
    /* Core 1: the USB host interrupt and client task live on core 0 (usb_host_install runs from
     * app_main), and the 270 ms PBKDF2 of a settings-password check runs on this task (sign-in
     * round trips varied 270-320 ms with the core it landed on). */
    c.core_id = 1;
    c.max_open_sockets = 8;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = RECV_TIMEOUT_S;
    c.send_wait_timeout = 10;
    c.close_fn = on_close;
    esp_err_t e = httpd_start(&s_srv, &c);
    if (e != ESP_OK) {
        s_srv = NULL;
        return e;
    }
    /* every route matters (the page, the API, /ota, the Satellite REST): a server missing one is
     * not "running", so the OTA self-check refuses such an image */
    for (size_t i = 0; i < nuris && e == ESP_OK; i++) {
        e = httpd_register_uri_handler(s_srv, &uris[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "%s: %s", uris[i].uri, esp_err_to_name(e));
    }
    if (e == ESP_OK) e = httpd_register_err_handler(s_srv, HTTPD_404_NOT_FOUND, h_404);
    if (e != ESP_OK) {
        httpd_stop(s_srv);
        s_srv = NULL;
        return e;
    }
    ESP_LOGI(TAG, "http server on :80");
    return ESP_OK;
}

bool webui_running(void) { return s_srv != NULL; }

esp_err_t webui_start(box_config_t *cfg, void (*on_config_changed)(void), box_surface_mode_t mode)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_srv) return ESP_ERR_INVALID_STATE;
    webui_log_capture_start();
    rpc_init();
    esp_err_t e = webui_methods_init(cfg, on_config_changed, mode);
    if (e != ESP_OK) return e;
    auth_init();
    rpc_set_auth(auth_enabled, auth_token_ok);
    webui_deck_push_settings(); /* the decks' screens: the settings and whether a password is set */
    e = web_start();
    if (e != ESP_OK) return e;
    e = notify_start();
    if (e != ESP_OK) ESP_LOGW(TAG, "status pushes off: %s (pages poll instead)", esp_err_to_name(e));
    return ESP_OK;
}
