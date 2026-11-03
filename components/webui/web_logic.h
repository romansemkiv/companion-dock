/* The web server's pure rules (host-tested): the browser guards (Host check against DNS rebinding,
 * cross-site detection), host-name cleaning for the Satellite REST API, URL decoding and the box's
 * host name. web.c reads the headers and passes them in. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A request header as read: v NULL = absent; bad = present but not readable (longer than the
 * buffer, or any other read error). A bad header never counts as absent: the guards refuse it. */
typedef struct {
    const char *v;
    bool bad;
} wg_hdr_t;

/* How reading a header went; wg_hdr turns it into the guards' view. Only WG_READ_ABSENT (the
 * header is not in the request) counts as absent: a value longer than the buffer
 * (ESP_ERR_HTTPD_RESULT_TRUNC) or any other failure is WG_READ_FAILED, a bad header. */
typedef enum { WG_READ_OK, WG_READ_ABSENT, WG_READ_FAILED } wg_read_t;
wg_hdr_t wg_hdr(wg_read_t r, const char *buf);

/* While no password is set: the Host header must be an IPv4 address, an IPv6 literal, a *.local
 * name, the box's host name or its device id (any case, with or without a port). An absent Host
 * (HTTP/1.0 tools) passes; a bad or malformed one does not. */
bool wg_host_ok(wg_hdr_t host, const char *box_hostname, const char *device_id);

/* A request a page on another site made the browser send. With Sec-Fetch-Site the browser says
 * so (cross-site / same-site); without it a foreign or opaque ("null") Origin is cross-site. A bad
 * Sec-Fetch-Site, Origin or Host counts as cross-site. */
bool wg_cross_site(wg_hdr_t sec_fetch_site, wg_hdr_t origin, wg_hdr_t host);

/* "host[:port]" -> lowercase host ("[v6]" keeps its brackets); false when malformed or too long. */
bool web_host_only(const char *hp, char *out, size_t n);


/* Like companion-satellite's sanitizeHost: trimmed; a URL gives its host name (no user, port or
 * path); otherwise anything from the first '/' on is dropped. */
void web_sanitize_host(const char *in, char *out, size_t n);

/* %XX and '+' decoding in place. */
void web_url_decode(char *s);

/* how a Satellite REST request is served. WG_API_FULL: as any API request, all fields (no settings PIN or
 * password set, or the request is signed in); WG_API_ADOPT: a PIN or password is set and the request carries no
 * credentials at all, companionAdopt is on, this run is in Satellite mode, its Host passes wg_host_ok (no DNS
 * rebinding), and it is one of the Satellite REST endpoints (GET /api/host, /api/port, /api/connected, /api/config,
 * /api/status, /api/surfaces; POST /api/host, /api/port, /api/config, /api/surfaces/rescan): served, a POST applying
 * only the Companion target (wg_api_body); WG_API_DENY: 401. */
typedef enum { WG_API_DENY, WG_API_FULL, WG_API_ADOPT } wg_api_t;
typedef struct {
    bool post;        /* a POST, else a GET */
    const char *uri;  /* the request line's URI as received: its path, a query or fragment after it ignored. httpd routes
                       * on the parsed path, so an absolute-form URI ("http://h/api/config") can reach a handler: it
                       * matches nothing here, so it fails closed (DENY) */
    bool secret_set;  /* a settings PIN or password is set */
    bool authed;      /* it carries a valid token */
    bool credentials; /* it carries an Authorization header, valid or not (an invalid one never gets ADOPT) */
    bool adopt;       /* companionAdopt */
    bool satellite;   /* this run is in Satellite mode (the only one Companion's Discover sees) */
    bool host_ok;     /* its Host passes wg_host_ok */
} wg_api_req_t;
wg_api_t wg_api_access(const wg_api_req_t *r);

/* an unauthenticated adopt's host, as the box would use it (sanitized): 1 to 63 printable ASCII characters,
 * no space or control character (it reaches the log and the decks). */
bool wg_adopt_host(const char *h);

/* the unauthenticated adopt writes' budget, a bucket of WG_ADOPT_BURST changes refilled one every
 * WG_ADOPT_REFILL_MS (zeroed: full). */
#define WG_ADOPT_BURST 3
#define WG_ADOPT_REFILL_MS 60000
typedef struct {
    bool started, warned;
    int tokens;
    int64_t at_ms;     /* the refill clock */
    int64_t warned_ms; /* the last refusal logged */
} wg_bucket_t;
/* What an unauthenticated adopt that passed validation does. moved: it changes the Companion target (host or port).
 * WG_ADOPT_QUIET: it doesn't (nothing written or toasted, no token); WG_ADOPT_APPLY: a token spent, applied;
 * WG_ADOPT_LIMIT: none left (429, nothing applied or toasted). */
typedef enum { WG_ADOPT_QUIET, WG_ADOPT_APPLY, WG_ADOPT_LIMIT } wg_adopt_t;
wg_adopt_t wg_adopt_step(wg_bucket_t *b, bool moved, int64_t now_ms);
/* A refused adopt is logged now: true at most once per WG_ADOPT_REFILL_MS. */
bool wg_adopt_log_due(wg_bucket_t *b, int64_t now_ms);

/* the fields of a Satellite REST body (an object) that apply under access: every one (WG_API_FULL), or only the
 * Companion target, "host" and "port" (WG_API_ADOPT: any other field is taken out of body, so it is ignored, never
 * applied and never an error; host and port are then validated as with WG_API_FULL). */
struct cJSON;
void wg_api_body(struct cJSON *body, wg_api_t access);

#ifdef __cplusplus
}
#endif
