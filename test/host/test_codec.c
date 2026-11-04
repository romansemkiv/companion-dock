#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "satproto/b64.h"
#include "satproto/codec.h"
#include "unity.h"

static cJSON *corpus;

void setUp(void) {}
void tearDown(void) {}

static cJSON *load_corpus(void)
{
    FILE *f = fopen(CORPUS_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open corpus " CORPUS_PATH);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *txt = malloc((size_t)n + 1);
    TEST_ASSERT_EQUAL(n, (long)fread(txt, 1, (size_t)n, f));
    txt[n] = '\0';
    fclose(f);
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

static cJSON *section(const char *name)
{
    cJSON *s = cJSON_GetObjectItemCaseSensitive(corpus, name);
    TEST_ASSERT_TRUE(cJSON_IsArray(s));
    TEST_ASSERT_TRUE(cJSON_GetArraySize(s) > 0);
    return s;
}

static const char *str_of(const cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void write_args(sat_writer_t *w, const cJSON *args)
{
    for (cJSON *a = args->child; a; a = a->next) {
        if (cJSON_IsBool(a)) sat_w_bool(w, a->string, cJSON_IsTrue(a));
        else if (cJSON_IsNumber(a)) sat_w_int(w, a->string, (long)a->valuedouble);
        else sat_w_str(w, a->string, a->valuestring);
    }
}

static void test_corpus_parse(void)
{
    cJSON *c;
    cJSON_ArrayForEach(c, section("parse"))
    {
        const char *name = str_of(c, "name");
        char *buf = strdup(str_of(c, "line"));
        sat_arg_t args[SAT_MAX_ARGS];
        int n = sat_parse_params(buf, args, SAT_MAX_ARGS);
        cJSON *exp = cJSON_GetObjectItemCaseSensitive(c, "expected");
        TEST_ASSERT_EQUAL_MESSAGE(cJSON_GetArraySize(exp), n, name);
        for (cJSON *e = exp->child; e; e = e->next) {
            int found = 0;
            for (int i = 0; i < n; i++) {
                if (strcmp(args[i].key, e->string) != 0) continue;
                found = 1;
                if (cJSON_IsTrue(e)) TEST_ASSERT_NULL_MESSAGE(args[i].val, name);
                else {
                    TEST_ASSERT_NOT_NULL_MESSAGE(args[i].val, name);
                    TEST_ASSERT_EQUAL_STRING_MESSAGE(e->valuestring, args[i].val, name);
                }
            }
            TEST_ASSERT_EQUAL_MESSAGE(1, found, name);
        }
        free(buf);
    }
}

static void test_corpus_format(void)
{
    cJSON *c;
    cJSON_ArrayForEach(c, section("format"))
    {
        const char *name = str_of(c, "name");
        char out[512];
        sat_writer_t w;
        sat_w_begin(&w, out, sizeof out, str_of(c, "message"), str_of(c, "status"), str_of(c, "deviceId"));
        write_args(&w, cJSON_GetObjectItemCaseSensitive(c, "args"));
        TEST_ASSERT_TRUE_MESSAGE(sat_w_end(&w), name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(str_of(c, "expected"), out, name);
    }
}

static void test_corpus_format_errors(void)
{
    cJSON *c;
    cJSON_ArrayForEach(c, section("formatErrors"))
    {
        const char *name = str_of(c, "name");
        const char *msg = str_of(c, "message");
        char out[512];
        sat_writer_t w;
        sat_w_begin(&w, out, sizeof out, msg ? msg : "X", str_of(c, "status"), str_of(c, "deviceId"));
        write_args(&w, cJSON_GetObjectItemCaseSensitive(c, "args"));
        TEST_ASSERT_FALSE_MESSAGE(sat_w_end(&w), name);
    }
}

static void test_corpus_round_trip(void)
{
    cJSON *s;
    cJSON_ArrayForEach(s, section("roundTrip"))
    {
        char out[256];
        sat_writer_t w;
        sat_w_begin(&w, out, sizeof out, "X", NULL, NULL);
        sat_w_str(&w, "V", s->valuestring);
        TEST_ASSERT_TRUE_MESSAGE(sat_w_end(&w), s->valuestring);
        sat_msg_t m;
        TEST_ASSERT_TRUE(sat_parse_line(out, &m));
        TEST_ASSERT_EQUAL_STRING(s->valuestring, sat_msg_get(&m, "V"));
    }
}

static void test_parse_line_splits_command(void)
{
    char line[] = "KEY-STATE DEVICEID=\"a\" KEY=5 FLAG";
    sat_msg_t m;
    TEST_ASSERT_TRUE(sat_parse_line(line, &m));
    TEST_ASSERT_EQUAL_STRING("KEY-STATE", m.cmd);
    TEST_ASSERT_EQUAL_STRING("5", sat_msg_get(&m, "KEY"));
    TEST_ASSERT_EQUAL_STRING("a", sat_msg_get(&m, "DEVICEID"));
    TEST_ASSERT_EQUAL_STRING("", sat_msg_get(&m, "FLAG"));
    TEST_ASSERT_NULL(sat_msg_get(&m, "NOPE"));
}

static void test_parse_line_edge_cases(void)
{
    sat_msg_t m;
    char empty[] = "";
    TEST_ASSERT_FALSE(sat_parse_line(empty, &m));
    char only[] = "PING";
    TEST_ASSERT_TRUE(sat_parse_line(only, &m));
    TEST_ASSERT_EQUAL_STRING("PING", m.cmd);
    TEST_ASSERT_EQUAL(0, m.argc);
}

static void test_parse_params_caps_at_max(void)
{
    char buf[] = "A=1 B=2 C=3";
    sat_arg_t args[2];
    TEST_ASSERT_EQUAL(2, sat_parse_params(buf, args, 2));
}

static void test_writer_overflow(void)
{
    char out[8];
    sat_writer_t w;
    sat_w_begin(&w, out, sizeof out, "PING", NULL, NULL);
    sat_w_str(&w, "K", "toolongvalue");
    TEST_ASSERT_FALSE(sat_w_end(&w));
    /* exact fit: "PING \n" + NUL = 7 bytes */
    sat_w_begin(&w, out, sizeof out, "PING", NULL, NULL);
    TEST_ASSERT_TRUE(sat_w_end(&w));
    TEST_ASSERT_EQUAL_STRING("PING \n", out);
}

static void test_b64_round_trip(void)
{
    char enc[16];
    TEST_ASSERT_EQUAL(8, sat_b64_encode((const uint8_t *)"hello", 5, enc, sizeof enc));
    TEST_ASSERT_EQUAL_STRING("aGVsbG8=", enc);
    uint8_t dec[16];
    size_t n = 0;
    TEST_ASSERT_TRUE(sat_b64_decode(enc, 8, dec, sizeof dec, &n));
    TEST_ASSERT_EQUAL(5, n);
    TEST_ASSERT_EQUAL_MEMORY("hello", dec, 5);
    TEST_ASSERT_EQUAL(0, sat_b64_encode((const uint8_t *)"hello", 5, enc, 8)); /* no room for NUL */
}

/* KEY-STATE's BITMAP: bare base64 is raw RGB; with BITMAP_FORMAT a data URL, png the one the box decodes */
static void split(const char *v, sat_bitmap_fmt_t want, const char *want_b64)
{
    const char *b = NULL;
    size_t n = 99;
    TEST_ASSERT_EQUAL_MESSAGE(want, sat_bitmap_split(v, strlen(v), &b, &n), v);
    if (want_b64) {
        TEST_ASSERT_NOT_NULL(b);
        TEST_ASSERT_EQUAL_MESSAGE(strlen(want_b64), n, v);
        if (n) TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want_b64, b, n, v);
    }
}

static void test_bitmap_split(void)
{
    split("AAAA", SAT_BITMAP_RGB, "AAAA");
    split("", SAT_BITMAP_RGB, "");
    split("data:image/png;base64,iVBORw0KGgo=", SAT_BITMAP_PNG, "iVBORw0KGgo=");
    split("data:image/png;base64,", SAT_BITMAP_PNG, "");
    split("data:image/webp;base64,UklGRg==", SAT_BITMAP_UNSUPPORTED, NULL);
    split("data:image/png,iVBOR", SAT_BITMAP_UNSUPPORTED, NULL);   /* not base64 */
    split("data:image/pngx;base64,AAAA", SAT_BITMAP_UNSUPPORTED, NULL);
    split("data:", SAT_BITMAP_UNSUPPORTED, NULL);
    split("data:text/plain;base64,AAAA", SAT_BITMAP_UNSUPPORTED, NULL);
    /* the value's own length counts, not a NUL: a prefix cut short is no data URL */
    const char *v = "data:image/png;base64,AAAA";
    const char *b;
    size_t n;
    TEST_ASSERT_EQUAL(SAT_BITMAP_PNG, sat_bitmap_split(v, strlen(v) - 2, &b, &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(SAT_BITMAP_UNSUPPORTED, sat_bitmap_split(v, 10, &b, &n));
    TEST_ASSERT_EQUAL(SAT_BITMAP_RGB, sat_bitmap_split(v, 4, &b, &n)); /* "data" alone: no colon, so base64 */
}

static void test_b64_decode_rejects_garbage(void)
{
    uint8_t dec[16];
    size_t n = 0;
    TEST_ASSERT_FALSE(sat_b64_decode("aGV*bG8=", 8, dec, sizeof dec, &n));
    TEST_ASSERT_FALSE(sat_b64_decode("aGVsbG8=", 8, dec, 2, &n));
    TEST_ASSERT_FALSE(sat_b64_decode("aGVsbG8", 7, dec, sizeof dec, &n));
    TEST_ASSERT_FALSE(sat_b64_decode("aG=sbG8=", 8, dec, sizeof dec, &n));
    TEST_ASSERT_FALSE(sat_b64_decode("aGVs=G8=", 8, dec, sizeof dec, &n));
    TEST_ASSERT_TRUE(sat_b64_decode("", 0, dec, sizeof dec, &n));
    TEST_ASSERT_EQUAL(0, n);
}

/* ---- The old implementations, kept here as the reference the new ones must match ---- */

static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) /* xorshift32: the same cases every run */
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int ref_dec(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool ref_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
    if (n % 4 != 0) return false;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            char c = in[i + k];
            if (c == '=') {
                if (i + 4 != n || k < 2) return false;
                pad++;
                v[k] = 0;
            } else {
                if (pad) return false;
                v[k] = ref_dec(c);
                if (v[k] < 0) return false;
            }
        }
        size_t take = 3 - (size_t)pad;
        if (o + take > cap) return false;
        uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        out[o++] = (uint8_t)(w >> 16);
        if (take > 1) out[o++] = (uint8_t)(w >> 8);
        if (take > 2) out[o++] = (uint8_t)w;
    }
    if (out_len) *out_len = o;
    return true;
}

/* the table decoder accepts and rejects what the old one did, with the same bytes out (and the same partial
 * output on a reject), over 200k cases: random encodings (some broken in one place, some short of room) and
 * random strings of base64 chars, '=' and any byte. */
static void test_b64_decode_matches_old(void)
{
    static const char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char in[64];
    uint8_t a[64], b[64];
    int ok = 0, bad = 0;
    for (int it = 0; it < 200000; it++) {
        size_t n;
        if (it & 1) {
            uint8_t raw[39];
            const size_t rn = rnd() % (sizeof raw + 1);
            for (size_t i = 0; i < rn; i++) raw[i] = (uint8_t)rnd();
            n = sat_b64_encode(raw, rn, in, sizeof in);
            if (n && rnd() % 4 == 0) in[rnd() % n] = (char)rnd();
        } else {
            n = rnd() % 33;
            for (size_t i = 0; i < n; i++) {
                const uint32_t r = rnd() % 16;
                in[i] = r < 12 ? ALPHA[rnd() % 64] : r < 14 ? '=' : (char)rnd();
            }
        }
        const size_t cap = rnd() % 45;
        memset(a, 0xA5, sizeof a);
        memset(b, 0xA5, sizeof b);
        size_t na = 12345, nb = 12345;
        const bool ra = sat_b64_decode(in, n, a, cap, &na), rb = ref_b64_decode(in, n, b, cap, &nb);
        TEST_ASSERT_EQUAL(rb, ra);
        TEST_ASSERT_EQUAL(nb, na);
        TEST_ASSERT_EQUAL_MEMORY(b, a, sizeof a);
        ra ? ok++ : bad++;
    }
    TEST_ASSERT_TRUE(ok > 50000 && bad > 50000); /* both outcomes well covered */
}

static bool ref_is_banned(const char *k)
{
    static const char *const BANNED[] = {
        "__proto__", "constructor", "prototype", "__defineGetter__",
        "__defineSetter__", "__lookupGetter__", "__lookupSetter__",
    };
    for (size_t i = 0; i < sizeof BANNED / sizeof BANNED[0]; i++)
        if (strcmp(k, BANNED[i]) == 0) return true;
    return false;
}

static int ref_parse_params(char *params, sat_arg_t *args, int max)
{
    int argc = 0;
    char *r = params;
    char *w = params;
    bool quoted = false;
    char *frag = params;
    char *eq = NULL;
    for (;;) {
        char c = *r;
        bool end = (c == '\0');
        if (end || (!quoted && c == ' ')) {
            *w = '\0';
            if (w != frag) {
                const char *key = frag;
                const char *val = NULL;
                if (eq) {
                    *eq = '\0';
                    val = eq + 1;
                }
                if (key[0] != '\0' && !ref_is_banned(key) && argc < max) {
                    args[argc].key = key;
                    args[argc].val = val;
                    argc++;
                }
            }
            if (end) break;
            r++;
            frag = w + 1;
            w = frag;
            eq = NULL;
            continue;
        }
        r++;
        if (c == '\\') {
            if (*r == '\0') continue;
            c = *r++;
        } else if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c == '=' && !eq) eq = w;
        *w++ = c;
    }
    return argc;
}

/* the parser (now storing only once something has been compacted) leaves the same buffer and the same args as
 * the old one, over 200k random parameter strings of keys, '=', spaces, quotes, escapes and banned keys. */
static void test_parse_params_matches_old(void)
{
    static const char *const P[] = {"a", "b", "=", "==", " ", "  ", "\"", "\\", "\\\"", "\\ ", "KEY", "x=1", "__proto__",
                                    "constructor", "=v", "\\\\", "BITMAP=QUJD", "TEXT=\"a b\"", "\\="};
    char a[160], b[160];
    sat_arg_t xa[SAT_MAX_ARGS], xb[SAT_MAX_ARGS];
    for (int it = 0; it < 200000; it++) {
        memset(a, 0, sizeof a);
        size_t n = 0;
        const int pieces = (int)(rnd() % 24);
        for (int k = 0; k < pieces; k++) {
            const char *p = P[rnd() % (sizeof P / sizeof P[0])];
            const size_t l = strlen(p);
            if (n + l >= 120) break;
            memcpy(a + n, p, l);
            n += l;
        }
        if (rnd() % 8 == 0 && n < 120) a[n++] = (char)(1 + rnd() % 255); /* any byte but NUL */
        memcpy(b, a, sizeof a);
        const int max = (int)(rnd() % (SAT_MAX_ARGS + 1));
        const int na = sat_parse_params(a, xa, max), nb = ref_parse_params(b, xb, max);
        TEST_ASSERT_EQUAL(nb, na);
        TEST_ASSERT_EQUAL_MEMORY(b, a, sizeof a);
        for (int i = 0; i < na; i++) {
            TEST_ASSERT_EQUAL(xb[i].key - b, xa[i].key - a);
            TEST_ASSERT_EQUAL(xb[i].val == NULL, xa[i].val == NULL);
            if (xa[i].val) TEST_ASSERT_EQUAL(xb[i].val - b, xa[i].val - a);
        }
    }
}

int main(void)
{
    corpus = load_corpus();
    UNITY_BEGIN();
    RUN_TEST(test_corpus_parse);
    RUN_TEST(test_corpus_format);
    RUN_TEST(test_corpus_format_errors);
    RUN_TEST(test_corpus_round_trip);
    RUN_TEST(test_parse_line_splits_command);
    RUN_TEST(test_parse_line_edge_cases);
    RUN_TEST(test_parse_params_caps_at_max);
    RUN_TEST(test_writer_overflow);
    RUN_TEST(test_bitmap_split);
    RUN_TEST(test_b64_round_trip);
    RUN_TEST(test_b64_decode_rejects_garbage);
    RUN_TEST(test_b64_decode_matches_old);
    RUN_TEST(test_parse_params_matches_old);
    int r = UNITY_END();
    cJSON_Delete(corpus);
    return r;
}
