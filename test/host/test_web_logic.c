#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boxconfig/config.h"
#include "cJSON.h"
#include "unity.h"
#include "web_logic.h"

void setUp(void) {}
void tearDown(void) {}

static const wg_hdr_t ABSENT = {0};
static const wg_hdr_t BAD = {.bad = true}; /* present but too long to read (or any read error) */
static wg_hdr_t H(const char *v) { return (wg_hdr_t){.v = v}; }

#define HN "companionbox-8d1c"
#define DEV "companionbox-80f1b2d68d1c"

/* only "not in the request" is absent; a truncated value (or any read failure) is bad */
static void test_header_read_mapping(void)
{
    wg_hdr_t h = wg_hdr(WG_READ_OK, "box.local");
    TEST_ASSERT_EQUAL_STRING("box.local", h.v);
    TEST_ASSERT_FALSE(h.bad);
    h = wg_hdr(WG_READ_ABSENT, NULL);
    TEST_ASSERT_NULL(h.v);
    TEST_ASSERT_FALSE(h.bad);
    h = wg_hdr(WG_READ_FAILED, "partial-value");
    TEST_ASSERT_NULL(h.v);
    TEST_ASSERT_TRUE(h.bad);
    h = wg_hdr(WG_READ_OK, NULL);
    TEST_ASSERT_TRUE(h.bad);
    TEST_ASSERT_FALSE(wg_host_ok(wg_hdr(WG_READ_FAILED, NULL), HN, DEV));
    TEST_ASSERT_TRUE(wg_host_ok(wg_hdr(WG_READ_ABSENT, NULL), HN, DEV));
}

static void test_host_allowed_names(void)
{
    TEST_ASSERT_TRUE(wg_host_ok(H("192.168.8.225"), HN, DEV));
    TEST_ASSERT_TRUE(wg_host_ok(H("192.168.8.225:80"), HN, DEV));
    TEST_ASSERT_TRUE(wg_host_ok(H("[fe80::1]:80"), HN, DEV));
    TEST_ASSERT_TRUE(wg_host_ok(H("CompanionBox-8D1C.local"), HN, DEV));
    TEST_ASSERT_TRUE(wg_host_ok(H("companionbox-8d1c"), HN, DEV));
    TEST_ASSERT_TRUE(wg_host_ok(H("COMPANIONBOX-80F1B2D68D1C:80"), HN, DEV));
    TEST_ASSERT_TRUE(wg_host_ok(ABSENT, HN, DEV)); /* HTTP/1.0 tool */
}

static void test_host_refused(void)
{
    TEST_ASSERT_FALSE(wg_host_ok(H("evil.example"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("companionbox-8d1c.evil.example"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("192.168.8.225.evil.example"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("1.2.3"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("1.2.3.256"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("1..2.3"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H(".local"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H(""), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("[fe80::1"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("[fe80::1]x"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("a b.local"), HN, DEV));
    TEST_ASSERT_FALSE(wg_host_ok(H("evil.example"), "", "")); /* no names to match */
}

/* a Host header too long for the server's buffer must not read as "absent". */
static void test_host_bad_or_long_refused(void)
{
    TEST_ASSERT_FALSE(wg_host_ok(BAD, HN, DEV));
    char longhost[200];
    memset(longhost, 'a', sizeof longhost - 7);
    strcpy(longhost + sizeof longhost - 7, ".local");
    TEST_ASSERT_FALSE(wg_host_ok(H(longhost), HN, DEV)); /* longer than any host name we accept */
}

static void test_cross_site_fetch_metadata(void)
{
    TEST_ASSERT_TRUE(wg_cross_site(H("cross-site"), ABSENT, H("10.0.0.2")));
    TEST_ASSERT_TRUE(wg_cross_site(H("same-site"), ABSENT, H("10.0.0.2")));
    TEST_ASSERT_FALSE(wg_cross_site(H("same-origin"), H("http://evil.example"), H("10.0.0.2")));
    TEST_ASSERT_FALSE(wg_cross_site(H("none"), ABSENT, H("10.0.0.2")));
    TEST_ASSERT_TRUE(wg_cross_site(BAD, ABSENT, H("10.0.0.2")));
}

static void test_cross_site_origin(void)
{
    TEST_ASSERT_FALSE(wg_cross_site(ABSENT, ABSENT, H("10.0.0.2"))); /* curl, Companion */
    TEST_ASSERT_FALSE(wg_cross_site(ABSENT, H("http://10.0.0.2"), H("10.0.0.2")));
    TEST_ASSERT_FALSE(wg_cross_site(ABSENT, H("http://Box.local:80"), H("box.local:80")));
    TEST_ASSERT_TRUE(wg_cross_site(ABSENT, H("http://evil.example"), H("10.0.0.2")));
    TEST_ASSERT_TRUE(wg_cross_site(ABSENT, H("null"), H("10.0.0.2")));
    TEST_ASSERT_TRUE(wg_cross_site(ABSENT, H("http://10.0.0.2"), ABSENT));
    /* an unreadable (too long) Origin or Host is cross-site, never "no Origin" */
    TEST_ASSERT_TRUE(wg_cross_site(ABSENT, BAD, H("10.0.0.2")));
    TEST_ASSERT_TRUE(wg_cross_site(ABSENT, H("http://10.0.0.2"), BAD));
}

static void test_host_only(void)
{
    char h[32];
    TEST_ASSERT_TRUE(web_host_only("Box.Local:8080", h, sizeof h));
    TEST_ASSERT_EQUAL_STRING("box.local", h);
    TEST_ASSERT_TRUE(web_host_only("[::1]:80", h, sizeof h));
    TEST_ASSERT_EQUAL_STRING("[::1]", h);
    TEST_ASSERT_FALSE(web_host_only(":80", h, sizeof h));
    TEST_ASSERT_FALSE(web_host_only("abcdefghijklmnopqrstuvwxyz0123456789", h, sizeof h));
    TEST_ASSERT_FALSE(web_host_only(NULL, h, sizeof h));
}

static void test_hostname(void)
{
    char h[40];
    boxcfg_hostname("CompanionBox-8D1C", h, sizeof h); /* boxconfig's, once web_hostname */
    TEST_ASSERT_EQUAL_STRING("companionbox-8d1c", h);
    boxcfg_hostname("Rack 2 / Left", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("rack-2---left", h);
    boxcfg_hostname("abcdefgh", h, 5);
    TEST_ASSERT_EQUAL_STRING("abcd", h);
}

/* companion-satellite sanitizeHost: new URL(x).hostname, else drop from the first '/' */
static void test_sanitize_host(void)
{
    char h[72];
    web_sanitize_host("  192.168.8.114  ", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("192.168.8.114", h);
    web_sanitize_host("http://192.168.8.114:8000/x?y#z", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("192.168.8.114", h);
    web_sanitize_host("https://user:pw@companion.local/", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("companion.local", h);
    web_sanitize_host("http://[fd00::5]:8000/", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("[fd00::5]", h);
    web_sanitize_host("192.168.8.114/abc", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("192.168.8.114", h);
    web_sanitize_host("   ", h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("", h);
    web_sanitize_host(NULL, h, sizeof h);
    TEST_ASSERT_EQUAL_STRING("", h);
    web_sanitize_host("companion.example.org", h, 10);
    TEST_ASSERT_EQUAL_STRING("companion", h); /* cut to fit */
}

static void test_url_decode(void)
{
    char s[] = "a+b%20c%3D%7b%zz%4";
    web_url_decode(s);
    TEST_ASSERT_EQUAL_STRING("a b c={%zz%4", s);
}

/* ---- Adopting a PIN-locked box ---- */

/* The gate: endpoint x a PIN or password set x companionAdopt x signed in x an Authorization header x
 * Satellite mode x the Host check -> served (all fields, or the adopt subset) or 401. URI tricks (a trailing slash, a
 * different case, percent-encoding, a dot segment, an absolute-form URI) never match an open endpoint: httpd answers
 * them 404 before a handler, and here they fail closed. */
static void test_api_access(void)
{
    static const struct {
        bool post;
        const char *uri;
        bool open; /* a Satellite REST endpoint adoption opens */
    } E[] = {
        {false, "/api/host", true},          {false, "/api/port", true},     {false, "/api/connected", true},
        {false, "/api/config", true},        {false, "/api/status", true},   {false, "/api/surfaces", true},
        {true, "/api/host", true},           {true, "/api/port", true},      {true, "/api/config", true},
        {true, "/api/surfaces/rescan", true}, {false, "/api/config?x=1", true}, /* a query: the same endpoint */
        {true, "/api/config#x", true},       /* a fragment: the same */
        {false, "/api/surfaces/rescan", false}, {true, "/api/status", false}, {true, "/api/connected", false},
        {true, "/api/surfaces", false},      {true, "/rpc", false},          {false, "/rpc/Sys.GetStatus", false},
        {true, "/rpc/Companion.SetConfig", false}, {true, "/ota", false},    {false, "/", false},
        {false, "/api/configx", false},      {false, "/api/confi", false},   {false, "/api", false},
        {false, "/api/", false},             {false, "", false},
        /* URI tricks */
        {true, "/api/config/", false},       {true, "/API/config", false},   {true, "/api/Config", false},
        {true, "/api/%63onfig", false},      {true, "/api/config/..", false}, {true, "/api/config/../config", false},
        {true, "//api/config", false},       {true, "http://h/api/config", false}, {true, "/api/config%3F", false},
        {true, " /api/config", false},       {true, "/api/config ", false},  {true, "/./api/config", false},
    };
    int adopted = 0;
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++)
        for (int f = 0; f < 64; f++) {
            const wg_api_req_t r = {.post = E[i].post, .uri = E[i].uri, .secret_set = f & 1, .authed = f & 2, .credentials = f & 4,
                                    .adopt = f & 8, .satellite = f & 16, .host_ok = f & 32};
            /* signed in implies credentials: an authed request without them can't happen, but stays FULL */
            const wg_api_t want = !r.secret_set || r.authed ? WG_API_FULL
                                  : !r.credentials && r.adopt && r.satellite && r.host_ok && E[i].open ? WG_API_ADOPT
                                                                                                         : WG_API_DENY;
            char what[120];
            snprintf(what, sizeof what, "%s %s secret %d authed %d cred %d adopt %d sat %d host %d", E[i].post ? "POST" : "GET",
                     E[i].uri, r.secret_set, r.authed, r.credentials, r.adopt, r.satellite, r.host_ok);
            TEST_ASSERT_EQUAL_INT_MESSAGE(want, wg_api_access(&r), what);
            adopted += want == WG_API_ADOPT;
        }
    TEST_ASSERT_EQUAL_INT(12, adopted); /* the 12 open forms, each once: PIN set, no credentials, adopt, Satellite, Host ok */
    const wg_api_req_t none = {.post = true, .uri = NULL, .secret_set = true, .adopt = true, .satellite = true, .host_ok = true};
    TEST_ASSERT_EQUAL_INT(WG_API_DENY, wg_api_access(&none));
    const wg_api_req_t open = {.post = true, .uri = NULL};
    TEST_ASSERT_EQUAL_INT(WG_API_FULL, wg_api_access(&open)); /* no PIN: open, as ever */
    TEST_ASSERT_EQUAL_INT(WG_API_DENY, wg_api_access(NULL));
}

/* the Host the adopt subset needs, as the no-PIN path checks it: an IP literal, <name>.local or the box's own
 * name or id; a DNS-rebinding name or an empty Host never. */
static void test_api_host(void)
{
    static const struct {
        const char *host;
        bool ok;
    } H_[] = {
        {"192.168.1.50", true},      {"192.168.1.50:80", true},  {"[fe80::1]", true},         {"[fe80::1]:80", true},
        {"companionbox-8d1c.local", true}, {"anything.local", true}, {HN, true},              {DEV, true},
        {"CompanionBox-8D1C", true}, {"evil.example", false},     {"box.evil.example", false}, {"rebind.attacker.example", false},
        {"", false},                 {"192.168.1.50.evil.example", false}, {"local", false},   {"x.local.evil.example", false},
    };
    for (size_t i = 0; i < sizeof H_ / sizeof H_[0]; i++) {
        const bool ok = wg_host_ok(H(H_[i].host), HN, DEV);
        TEST_ASSERT_EQUAL_MESSAGE(H_[i].ok, ok, H_[i].host);
        const wg_api_req_t r = {.post = true, .uri = "/api/config", .secret_set = true, .adopt = true, .satellite = true, .host_ok = ok};
        TEST_ASSERT_EQUAL_INT_MESSAGE(H_[i].ok ? WG_API_ADOPT : WG_API_DENY, wg_api_access(&r), H_[i].host);
    }
    TEST_ASSERT_FALSE(wg_host_ok(BAD, HN, DEV)); /* a Host too long to read */
}

/* an unauthenticated host: 1-63 printable ASCII characters, no space or control character. */
static void test_adopt_host(void)
{
    TEST_ASSERT_TRUE(wg_adopt_host("10.0.0.5"));
    TEST_ASSERT_TRUE(wg_adopt_host("studio-mac.local"));
    TEST_ASSERT_TRUE(wg_adopt_host("[fe80::1]"));
    TEST_ASSERT_TRUE(wg_adopt_host("012345678901234567890123456789012345678901234567890123456789012")); /* 63 */
    TEST_ASSERT_FALSE(wg_adopt_host("0123456789012345678901234567890123456789012345678901234567890123")); /* 64 */
    TEST_ASSERT_FALSE(wg_adopt_host(""));
    TEST_ASSERT_FALSE(wg_adopt_host(NULL));
    TEST_ASSERT_FALSE(wg_adopt_host("a b"));
    TEST_ASSERT_FALSE(wg_adopt_host("a\nI (1) web: forged"));
    TEST_ASSERT_FALSE(wg_adopt_host("a\x1b[2J"));
    TEST_ASSERT_FALSE(wg_adopt_host("caf\xc3\xa9"));
    TEST_ASSERT_FALSE(wg_adopt_host("a\x7f"));
}

/* the adopt limiter: 3 changes at once, then one more a minute; the same target costs nothing (no write, no
 * toast, no token); refusals are logged at most once a minute. */
static void test_adopt_limiter(void)
{
    wg_bucket_t b = {0};
    int64_t t = 1000000;
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_QUIET, wg_adopt_step(&b, false, t)); /* no change: nothing, and no token spent */
    for (int i = 0; i < 10; i++) TEST_ASSERT_EQUAL_INT(WG_ADOPT_QUIET, wg_adopt_step(&b, false, t));
    for (int i = 0; i < WG_ADOPT_BURST; i++) TEST_ASSERT_EQUAL_INT(WG_ADOPT_APPLY, wg_adopt_step(&b, true, t + i));
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_LIMIT, wg_adopt_step(&b, true, t + 10));
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_QUIET, wg_adopt_step(&b, false, t + 11)); /* the same target stays fine */
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_LIMIT, wg_adopt_step(&b, true, t + WG_ADOPT_REFILL_MS - 1));
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_APPLY, wg_adopt_step(&b, true, t + WG_ADOPT_REFILL_MS)); /* one a minute */
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_LIMIT, wg_adopt_step(&b, true, t + WG_ADOPT_REFILL_MS + 1));
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_APPLY, wg_adopt_step(&b, true, t + 2 * WG_ADOPT_REFILL_MS));
    /* an hour quiet: full again, never more than 3 */
    int n = 0;
    while (wg_adopt_step(&b, true, t + 62 * WG_ADOPT_REFILL_MS) == WG_ADOPT_APPLY) n++;
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_BURST, n);
    /* 50 changes a second for a day: at most 3 + one a minute get through */
    wg_bucket_t c = {0};
    long through = 0;
    for (int64_t ms = 0; ms < 24LL * 3600 * 1000; ms += 20) through += wg_adopt_step(&c, true, ms) == WG_ADOPT_APPLY;
    TEST_ASSERT_TRUE(through <= WG_ADOPT_BURST + 24 * 60);
    TEST_ASSERT_TRUE(through >= 24 * 60);
    /* the clock going back never adds tokens */
    wg_bucket_t d = {0};
    for (int i = 0; i < WG_ADOPT_BURST; i++) wg_adopt_step(&d, true, 5000000);
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_LIMIT, wg_adopt_step(&d, true, 1000));
    TEST_ASSERT_EQUAL_INT(WG_ADOPT_LIMIT, wg_adopt_step(&d, true, 1000 + WG_ADOPT_REFILL_MS - 1));
    /* refusals logged at most once a minute */
    wg_bucket_t e = {0};
    TEST_ASSERT_TRUE(wg_adopt_log_due(&e, 100));
    TEST_ASSERT_FALSE(wg_adopt_log_due(&e, 100 + WG_ADOPT_REFILL_MS - 1));
    TEST_ASSERT_TRUE(wg_adopt_log_due(&e, 100 + WG_ADOPT_REFILL_MS));
}

static uint32_t seq_rand(void)
{
    static uint32_t n;
    return n++;
}

/* body (JSON) as it applies under access, printed; and what boxconfig makes of it on a fresh config: ok, *err. */
static char *applied(const char *body, wg_api_t access, box_config_t *c, bool *ok, char *err, size_t n)
{
    cJSON *j = cJSON_Parse(body);
    TEST_ASSERT_NOT_NULL(j);
    wg_api_body(j, access);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    static const uint8_t mac[6] = {2, 0, 0, 0, 0xA1, 0xB2};
    boxcfg_defaults(c, mac, seq_rand);
    *ok = boxcfg_apply_satellite_config(c, s, err, n);
    return s;
}

/* The field filter: unauthenticated (WG_API_ADOPT), only host and port apply, validated as when signed in; anything
 * else is dropped, never applied and never an error. Signed in or no PIN (WG_API_FULL): the body as it is. */
static void test_api_body(void)
{
    box_config_t c;
    bool ok;
    char err[96], err2[96];
    /* Companion's Discover: {host, port} */
    char *s = applied("{\"host\":\"10.0.0.5\",\"port\":16622}", WG_API_ADOPT, &c, &ok, err, sizeof err);
    TEST_ASSERT_EQUAL_STRING("{\"host\":\"10.0.0.5\",\"port\":16622}", s);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", c.companion_host);
    TEST_ASSERT_EQUAL_UINT16(16622, c.companion_port);
    TEST_ASSERT_FALSE(c.companion_auto); /* the authenticated path's effect: a manual address */
    free(s);
    /* a full ConfigDataUpdate: still adopts, the rest ignored (a bad protocol too) */
    const char *full = "{\"protocol\":\"ws\",\"host\":\"10.0.0.7\",\"port\":\"16623\",\"installationName\":\"Hijacked\","
                       "\"mdnsEnabled\":false,\"httpEnabled\":false,\"httpPort\":8080,\"wsAddress\":\"ws://x\"}";
    s = applied(full, WG_API_ADOPT, &c, &ok, err, sizeof err);
    TEST_ASSERT_EQUAL_STRING("{\"host\":\"10.0.0.7\",\"port\":\"16623\"}", s);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("10.0.0.7", c.companion_host);
    TEST_ASSERT_EQUAL_UINT16(16623, c.companion_port);
    TEST_ASSERT_EQUAL_STRING("CompanionBox-A1B2", c.box_name);
    TEST_ASSERT_TRUE(c.mdns_enabled);
    free(s);
    /* ... signed in, the same body applies whole: here its protocol is refused */
    s = applied(full, WG_API_FULL, &c, &ok, err, sizeof err);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING_LEN("protocol", err, 8);
    free(s);
    s = applied("{\"protocol\":\"tcp\",\"host\":\"10.0.0.7\",\"installationName\":\"Studio\",\"mdnsEnabled\":false}", WG_API_FULL,
                &c, &ok, err, sizeof err);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("Studio", c.box_name);
    TEST_ASSERT_FALSE(c.mdns_enabled);
    free(s);
    /* bad values: refused exactly as when signed in */
    static const char *const BAD[] = {"{\"port\":0}", "{\"port\":65536}", "{\"port\":\"12x\"}", "{\"port\":1.5}",
                                      "{\"port\":true}", "{\"host\":5}",
                                      "{\"host\":\"0123456789012345678901234567890123456789012345678901234567890123\"}",
                                      "{\"host\":\"10.0.0.5\",\"port\":-1,\"installationName\":\"x\"}"};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        box_config_t c2;
        bool ok2;
        char *a = applied(BAD[i], WG_API_ADOPT, &c, &ok, err, sizeof err);
        char *b = applied(BAD[i], WG_API_FULL, &c2, &ok2, err2, sizeof err2);
        TEST_ASSERT_FALSE_MESSAGE(ok, BAD[i]);
        TEST_ASSERT_FALSE_MESSAGE(ok2, BAD[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(err2, err, BAD[i]);
        free(a);
        free(b);
    }
    /* nothing of the target: nothing applies, and that is no error */
    s = applied("{\"installationName\":\"x\",\"mdnsEnabled\":false}", WG_API_ADOPT, &c, &ok, err, sizeof err);
    TEST_ASSERT_EQUAL_STRING("{}", s);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(c.mdns_enabled);
    free(s);
    /* adopt off (WG_API_DENY: the request gets its 401 and no body is read), or no access at all: untouched here */
    s = applied(full, WG_API_DENY, &c, &ok, err, sizeof err);
    TEST_ASSERT_NOT_NULL(strstr(s, "installationName"));
    free(s);
    wg_api_body(NULL, WG_API_ADOPT); /* nothing to do */
}

/* adversarial bodies under adopt: the box's other settings (the mode, the deck limit, the PIN,
 * Wi-Fi, companionAdopt itself), case variants, nested objects and a thousand keys are all dropped; duplicate hosts
 * keep the first, as the handler and boxconfig read it; an object or array for host or port is refused as when signed
 * in; a body that isn't an object is left alone (the handler refuses it first). */
static void test_api_body_adversarial(void)
{
    box_config_t c, d;
    bool ok;
    char err[96];
    const char *evil = "{\"surfaceMode\":\"satellite\",\"mode\":\"dock\",\"deckLimit\":4,\"adminPass\":\"\",\"pin\":\"1234\","
                       "\"wifiSsid\":\"Evil\",\"wifiPass\":\"12345678\",\"companionAdopt\":false,\"companionAuto\":true,"
                       "\"Host\":\"6.6.6.6\",\"PORT\":1,\"x\":{\"host\":\"7.7.7.7\"},\"host\":\"10.0.0.9\",\"port\":16700,"
                       "\"apEnabled\":false,\"ethDhcp\":false,\"ethIp\":\"1.2.3.4\"}";
    char *s = applied(evil, WG_API_ADOPT, &c, &ok, err, sizeof err);
    TEST_ASSERT_EQUAL_STRING("{\"host\":\"10.0.0.9\",\"port\":16700}", s);
    TEST_ASSERT_TRUE(ok);
    free(s);
    /* nothing but the target moved: the config is the defaults with the address set by hand */
    static const uint8_t mac[6] = {2, 0, 0, 0, 0xA1, 0xB2};
    boxcfg_defaults(&d, mac, seq_rand);
    strcpy(d.companion_host, "10.0.0.9");
    d.companion_port = 16700;
    d.companion_auto = false;
    strcpy(d.ap_pass, c.ap_pass); /* the random hotspot password of each fresh config */
    char *jc = boxcfg_to_json(&c, true), *jd = boxcfg_to_json(&d, true);
    TEST_ASSERT_EQUAL_STRING(jd, jc);
    free(jc);
    free(jd);
    /* duplicate keys: both kept, the first one applies */
    s = applied("{\"host\":\"10.0.0.1\",\"host\":\"10.0.0.2\",\"port\":1000,\"port\":2000}", WG_API_ADOPT, &c, &ok, err, sizeof err);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", c.companion_host);
    TEST_ASSERT_EQUAL_UINT16(1000, c.companion_port);
    free(s);
    /* an object or array for host or port: refused, as when signed in */
    static const char *const NESTED[] = {"{\"host\":{\"a\":1}}", "{\"host\":[\"10.0.0.1\"]}", "{\"port\":{\"p\":1}}",
                                         "{\"port\":[16622]}", "{\"port\":null}", "{\"port\":99999}"};
    for (size_t i = 0; i < sizeof NESTED / sizeof NESTED[0]; i++) {
        s = applied(NESTED[i], WG_API_ADOPT, &c, &ok, err, sizeof err);
        TEST_ASSERT_FALSE_MESSAGE(ok, NESTED[i]);
        free(s);
    }
    /* a thousand keys: only the target left */
    cJSON *big = cJSON_CreateObject();
    for (int i = 0; i < 1000; i++) {
        char k[16];
        snprintf(k, sizeof k, "k%d", i);
        cJSON_AddNumberToObject(big, k, i);
    }
    cJSON_AddStringToObject(big, "host", "10.0.0.3");
    wg_api_body(big, WG_API_ADOPT);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(big));
    cJSON_Delete(big);
    /* not an object: left as it is */
    cJSON *arr = cJSON_Parse("[{\"host\":\"x\"},1]");
    wg_api_body(arr, WG_API_ADOPT);
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(arr));
    cJSON_Delete(arr);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_read_mapping);
    RUN_TEST(test_host_allowed_names);
    RUN_TEST(test_host_refused);
    RUN_TEST(test_host_bad_or_long_refused);
    RUN_TEST(test_cross_site_fetch_metadata);
    RUN_TEST(test_cross_site_origin);
    RUN_TEST(test_host_only);
    RUN_TEST(test_hostname);
    RUN_TEST(test_sanitize_host);
    RUN_TEST(test_url_decode);
    RUN_TEST(test_api_access);
    RUN_TEST(test_api_host);
    RUN_TEST(test_adopt_host);
    RUN_TEST(test_adopt_limiter);
    RUN_TEST(test_api_body);
    RUN_TEST(test_api_body_adversarial);
    return UNITY_END();
}
