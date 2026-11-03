/* The web server's pure rules (web_logic.h). */
#include "web_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        const int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
    }
}

bool web_host_only(const char *hp, char *out, size_t n)
{
    if (!hp || !n) return false;
    size_t len;
    if (hp[0] == '[') {
        const char *e = strchr(hp, ']');
        if (!e) return false;
        len = (size_t)(e - hp) + 1;
        if (e[1] && e[1] != ':') return false;
    } else {
        len = strcspn(hp, ":");
    }
    if (!len || len >= n) return false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)hp[i];
        if (c <= ' ' || c >= 0x7F) return false;
        out[i] = (char)tolower(c);
    }
    out[len] = '\0';
    return true;
}

static bool is_ipv4(const char *h)
{
    int parts = 0;
    while (*h) {
        int v = 0, digits = 0;
        while (*h >= '0' && *h <= '9') {
            v = v * 10 + (*h++ - '0');
            if (++digits > 3 || v > 255) return false;
        }
        if (!digits) return false;
        parts++;
        if (*h == '.') {
            h++;
            if (!*h) return false;
        } else if (*h) {
            return false;
        }
    }
    return parts == 4;
}

static bool ends_with(const char *s, const char *suffix)
{
    const size_t a = strlen(s), b = strlen(suffix);
    return a > b && !strcmp(s + a - b, suffix);
}

wg_hdr_t wg_hdr(wg_read_t r, const char *buf)
{
    if (r == WG_READ_OK && buf) return (wg_hdr_t){.v = buf};
    if (r == WG_READ_ABSENT) return (wg_hdr_t){0};
    return (wg_hdr_t){.bad = true};
}

bool wg_host_ok(wg_hdr_t host, const char *box_hostname, const char *device_id)
{
    if (host.bad) return false;
    if (!host.v) return true; /* HTTP/1.0 tool */
    char h[96];
    if (!web_host_only(host.v, h, sizeof h)) return false;
    if (h[0] == '[' || is_ipv4(h) || ends_with(h, ".local")) return true;
    return (box_hostname && box_hostname[0] && !ci_cmp(h, box_hostname)) ||
           (device_id && device_id[0] && !ci_cmp(h, device_id));
}

bool wg_cross_site(wg_hdr_t sec_fetch_site, wg_hdr_t origin, wg_hdr_t host)
{
    if (sec_fetch_site.bad) return true;
    if (sec_fetch_site.v) return !strcmp(sec_fetch_site.v, "cross-site") || !strcmp(sec_fetch_site.v, "same-site");
    if (origin.bad) return true;
    if (!origin.v || !origin.v[0]) return false; /* not a browser, or a same-origin GET */
    if (!strcmp(origin.v, "null")) return true;
    if (host.bad || !host.v) return true;
    const char *p = strstr(origin.v, "://");
    return ci_cmp(p ? p + 3 : origin.v, host.v) != 0; /* Origin's host[:port] against Host */
}

void web_sanitize_host(const char *in, char *out, size_t n)
{
    if (!n) return;
    if (!in) in = "";
    while (isspace((unsigned char)*in)) in++;
    size_t len = strlen(in);
    while (len && isspace((unsigned char)in[len - 1])) len--;
    const char *s = in, *end = in + len;
    const char *scheme = strstr(in, "://");
    if (scheme && scheme < end) {
        s = scheme + 3;
        const char *stop = s;
        while (stop < end && !strchr("/?#", *stop)) stop++;
        end = stop;
        const char *at = memchr(s, '@', (size_t)(end - s));
        if (at) s = at + 1;
        if (*s == '[') {
            const char *rb = memchr(s, ']', (size_t)(end - s));
            if (rb) end = rb + 1;
        } else {
            const char *colon = memchr(s, ':', (size_t)(end - s));
            if (colon) end = colon;
        }
    } else {
        const char *slash = memchr(s, '/', len);
        if (slash) end = slash;
    }
    snprintf(out, n, "%.*s", (int)(end - s), s);
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
}

void web_url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            *o++ = (char)(hexval((unsigned char)s[1]) << 4 | hexval((unsigned char)s[2]));
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

wg_api_t wg_api_access(const wg_api_req_t *r)
{
    if (!r) return WG_API_DENY;
    if (!r->secret_set || r->authed) return WG_API_FULL;
    if (r->credentials || !r->adopt || !r->satellite || !r->host_ok || !r->uri) return WG_API_DENY;
    const bool post = r->post;
    const char *uri = r->uri;
    static const struct {
        bool post;
        const char *path;
    } OPEN[] = {
        {false, "/api/host"},   {false, "/api/port"},     {false, "/api/connected"}, {false, "/api/config"},
        {false, "/api/status"}, {false, "/api/surfaces"}, {true, "/api/host"},       {true, "/api/port"},
        {true, "/api/config"},  {true, "/api/surfaces/rescan"},
    };
    const size_t len = strcspn(uri, "?#");
    for (size_t i = 0; i < sizeof OPEN / sizeof OPEN[0]; i++)
        if (OPEN[i].post == post && strlen(OPEN[i].path) == len && !strncmp(OPEN[i].path, uri, len))
            return WG_API_ADOPT;
    return WG_API_DENY;
}

void wg_api_body(cJSON *body, wg_api_t access)
{
    if (access != WG_API_ADOPT || !cJSON_IsObject(body)) return;
    cJSON *it = body->child;
    while (it) {
        cJSON *next = it->next;
        if (!it->string || (strcmp(it->string, "host") && strcmp(it->string, "port")))
            cJSON_Delete(cJSON_DetachItemViaPointer(body, it));
        it = next;
    }
}

bool wg_adopt_host(const char *h)
{
    if (!h || !h[0]) return false;
    size_t n = 0;
    for (; h[n]; n++)
        if ((unsigned char)h[n] <= 0x20 || (unsigned char)h[n] >= 0x7F) return false;
    return n <= 63;
}

wg_adopt_t wg_adopt_step(wg_bucket_t *b, bool moved, int64_t now_ms)
{
    if (!moved) return WG_ADOPT_QUIET;
    if (!b->started) {
        b->started = true;
        b->tokens = WG_ADOPT_BURST;
        b->at_ms = now_ms;
    }
    if (now_ms < b->at_ms) b->at_ms = now_ms; /* the clock went back: the period starts again */
    const int64_t k = (now_ms - b->at_ms) / WG_ADOPT_REFILL_MS;
    if (k > 0) {
        b->tokens = b->tokens + k >= WG_ADOPT_BURST ? WG_ADOPT_BURST : b->tokens + (int)k;
        b->at_ms += k * WG_ADOPT_REFILL_MS;
    }
    if (b->tokens <= 0) return WG_ADOPT_LIMIT;
    if (b->tokens == WG_ADOPT_BURST) b->at_ms = now_ms; /* full: the refill clock starts with this change */
    b->tokens--;
    return WG_ADOPT_APPLY;
}

bool wg_adopt_log_due(wg_bucket_t *b, int64_t now_ms)
{
    if (b->warned && now_ms - b->warned_ms < WG_ADOPT_REFILL_MS && now_ms >= b->warned_ms) return false;
    b->warned = true;
    b->warned_ms = now_ms;
    return true;
}
