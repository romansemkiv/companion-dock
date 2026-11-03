#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "rpc/rpc.h"
#include "unity.h"

static bool s_auth_on;
static int s_calls;
static rpc_current_t s_seen;
static char s_seen_token[80], s_seen_src[40]; /* copies: the pointers live only during the call */
static int s_locks, s_unlocks;

static bool auth_enabled(void) { return s_auth_on; }
static bool token_ok(const char *t) { return t && strcmp(t, "good") == 0; }
static void lock(void) { s_locks++; }
static void unlock(void) { s_unlocks++; }

static cJSON *m_echo(const cJSON *p, rpc_err_t *err)
{
    (void)err;
    s_calls++;
    s_seen = *rpc_current();
    snprintf(s_seen_token, sizeof s_seen_token, "%s", s_seen.who.token ? s_seen.who.token : "");
    snprintf(s_seen_src, sizeof s_seen_src, "%s", s_seen.who.src ? s_seen.who.src : "");
    cJSON *r = cJSON_CreateObject();
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "v");
    cJSON_AddNumberToObject(r, "v", cJSON_IsNumber(v) ? v->valuedouble : -1);
    return r;
}

static cJSON *m_fail(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    return rpc_fail(err, RPC_E_NOT_FOUND, "no deck %d", 7);
}

static cJSON *m_null(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    return NULL;
}

static cJSON *m_public(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    return cJSON_CreateString("hello");
}

void setUp(void)
{
    rpc_reset();
    rpc_set_device_id("companionbox-test");
    rpc_set_auth(auth_enabled, token_ok);
    rpc_set_lock(lock, unlock);
    rpc_register("Test.Echo", m_echo, 0);
    rpc_register("Test.Fail", m_fail, 0);
    rpc_register("Test.Null", m_null, 0);
    rpc_register("Auth.GetStatus", m_public, RPC_F_PUBLIC);
    s_auth_on = false;
    s_calls = 0;
    s_locks = s_unlocks = 0;
    memset(&s_seen, 0, sizeof s_seen);
    s_seen_token[0] = s_seen_src[0] = 0;
}

void tearDown(void) {}

/* Parses a response frame; fails the test when it is not an object. */
static cJSON *parse(char *out)
{
    TEST_ASSERT_NOT_NULL(out);
    cJSON *r = cJSON_Parse(out);
    free(out);
    TEST_ASSERT_TRUE(cJSON_IsObject(r));
    return r;
}

static int err_code(const cJSON *r)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(r, "error");
    TEST_ASSERT_TRUE(cJSON_IsObject(e));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(r, "result"));
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(e, "code");
    TEST_ASSERT_TRUE(cJSON_IsNumber(c));
    TEST_ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "message")));
    return c->valueint;
}

static void test_parse_error(void)
{
    cJSON *r = parse(rpc_handle_frame("{\"id\":1,\"method\":", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_PARSE, err_code(r));
    TEST_ASSERT_EQUAL_STRING("companionbox-test", cJSON_GetObjectItem(r, "src")->valuestring);
    cJSON_Delete(r);

    r = parse(rpc_handle_frame("[1,2]", NULL, NULL)); /* valid JSON but not a frame */
    TEST_ASSERT_EQUAL_INT(RPC_E_PARSE, err_code(r));
    cJSON_Delete(r);

    r = parse(rpc_handle_frame(NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_PARSE, err_code(r));
    cJSON_Delete(r);
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}

static void test_deep_nesting_refused_before_parsing(void)
{
    char deep[4 * RPC_JSON_DEPTH + 80];
    int n = sprintf(deep, "{\"id\":1,\"method\":\"Test.Echo\",\"params\":{\"v\":");
    for (int i = 0; i < RPC_JSON_DEPTH; i++) deep[n++] = '[';
    for (int i = 0; i < RPC_JSON_DEPTH; i++) deep[n++] = ']';
    strcpy(deep + n, "}}");
    cJSON *r = parse(rpc_handle_frame(deep, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_PARSE, err_code(r));
    cJSON_Delete(r);
    TEST_ASSERT_EQUAL_INT(0, s_calls);

    /* brackets inside strings (escaped quotes too) do not count */
    char str[3 * RPC_JSON_DEPTH + 80];
    n = sprintf(str, "{\"id\":1,\"method\":\"Test.Echo\",\"params\":{\"s\":\"\\\"");
    for (int i = 0; i < 2 * RPC_JSON_DEPTH; i++) str[n++] = '[';
    strcpy(str + n, "\"}}");
    r = parse(rpc_handle_frame(str, NULL, NULL));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "result"));
    cJSON_Delete(r);

    TEST_ASSERT_NULL(rpc_json_parse(NULL));
    cJSON *ok = rpc_json_parse("{\"a\":[[1]]}");
    TEST_ASSERT_TRUE(cJSON_IsObject(ok));
    cJSON_Delete(ok);
}

/* a REST query value (GET /rpc/<Method>?k=v) is JSON only when the whole value is JSON, else the string itself:
 * cJSON alone takes a prefix ("10.0.0.5" was 10, "trueNorth" true). */
static void test_query_value_whole_json_only(void)
{
    static const char *const STR[] = {"10.0.0.5", "192.168.1.10", "trueNorth", "null pointer", "5x", "f", "-", "[1", ""};
    for (size_t i = 0; i < sizeof STR / sizeof *STR; i++) {
        cJSON *v = rpc_query_value(STR[i]);
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(v), STR[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(STR[i], v->valuestring, STR[i]);
        cJSON_Delete(v);
    }
    cJSON *v = rpc_query_value("16622");
    TEST_ASSERT_TRUE(cJSON_IsNumber(v) && v->valuedouble == 16622);
    cJSON_Delete(v);
    v = rpc_query_value("true");
    TEST_ASSERT_TRUE(cJSON_IsTrue(v));
    cJSON_Delete(v);
    v = rpc_query_value("\"x\"");
    TEST_ASSERT_TRUE(cJSON_IsString(v));
    TEST_ASSERT_EQUAL_STRING("x", v->valuestring);
    cJSON_Delete(v);
    v = rpc_query_value("{\"a\":[1]} "); /* trailing white space is fine */
    TEST_ASSERT_TRUE(cJSON_IsObject(v));
    cJSON_Delete(v);
    v = rpc_query_value("null");
    TEST_ASSERT_TRUE(cJSON_IsNull(v));
    cJSON_Delete(v);
}

static void test_unknown_method(void)
{
    cJSON *r = parse(rpc_handle_frame("{\"id\":5,\"method\":\"Nope.Nothing\"}", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_NO_METHOD, err_code(r));
    TEST_ASSERT_EQUAL_INT(5, cJSON_GetObjectItem(r, "id")->valueint);
    cJSON_Delete(r);
}

static void test_missing_method_and_bad_params(void)
{
    cJSON *r = parse(rpc_handle_frame("{\"id\":1}", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_INVALID_ARG, err_code(r));
    cJSON_Delete(r);
    r = parse(rpc_handle_frame("{\"id\":1,\"method\":\"Test.Echo\",\"params\":[1]}", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_INVALID_ARG, err_code(r));
    cJSON_Delete(r);
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}

static void test_result_framed(void)
{
    cJSON *r = parse(rpc_handle_frame(
        "{\"id\":\"abc\",\"src\":\"webui\",\"method\":\"Test.Echo\",\"params\":{\"v\":42}}", NULL, "10.0.0.9"));
    TEST_ASSERT_EQUAL_STRING("abc", cJSON_GetObjectItem(r, "id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("companionbox-test", cJSON_GetObjectItem(r, "src")->valuestring);
    TEST_ASSERT_EQUAL_STRING("webui", cJSON_GetObjectItem(r, "dst")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "error"));
    const cJSON *res = cJSON_GetObjectItem(r, "result");
    TEST_ASSERT_TRUE(cJSON_IsObject(res));
    TEST_ASSERT_EQUAL_INT(42, cJSON_GetObjectItem(res, "v")->valueint);
    cJSON_Delete(r);
    TEST_ASSERT_EQUAL_INT(1, s_calls);
    TEST_ASSERT_EQUAL_STRING("10.0.0.9", s_seen_src);
    TEST_ASSERT_TRUE(s_seen.authed);  /* no password: open */
    TEST_ASSERT_NULL(rpc_current());  /* only inside a call */
    TEST_ASSERT_EQUAL_INT(1, s_locks);
    TEST_ASSERT_EQUAL_INT(1, s_unlocks);
}

static void test_numeric_id_no_src_no_params(void)
{
    cJSON *r = parse(rpc_handle_frame("{\"id\":7,\"method\":\"test.echo\"}", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(7, cJSON_GetObjectItem(r, "id")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "dst")); /* the request had no src */
    TEST_ASSERT_EQUAL_INT(-1, cJSON_GetObjectItem(cJSON_GetObjectItem(r, "result"), "v")->valueint);
    cJSON_Delete(r);
}

static void test_notification_gets_no_reply(void)
{
    TEST_ASSERT_NULL(rpc_handle_frame("{\"method\":\"Test.Echo\"}", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, s_calls); /* but it ran */
}

static void test_handler_error_and_null_result(void)
{
    cJSON *r = parse(rpc_handle_frame("{\"id\":1,\"method\":\"Test.Fail\"}", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_NOT_FOUND, err_code(r));
    TEST_ASSERT_EQUAL_STRING("no deck 7", cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "message")->valuestring);
    cJSON_Delete(r);

    r = parse(rpc_handle_frame("{\"id\":2,\"method\":\"Test.Null\"}", NULL, NULL));
    const cJSON *res = cJSON_GetObjectItem(r, "result");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_TRUE(cJSON_IsNull(res));
    cJSON_Delete(r);
}

static void test_auth_enabled_requires_token(void)
{
    s_auth_on = true;
    cJSON *r = parse(rpc_handle_frame("{\"id\":1,\"method\":\"Test.Echo\"}", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_UNAUTHORIZED, err_code(r));
    cJSON_Delete(r);
    r = parse(rpc_handle_frame("{\"id\":1,\"method\":\"Test.Echo\",\"token\":\"bad\"}", "worse", NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_UNAUTHORIZED, err_code(r));
    cJSON_Delete(r);
    /* an unknown method is not revealed to a caller without a token */
    r = parse(rpc_handle_frame("{\"id\":1,\"method\":\"Nope.Nothing\"}", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(RPC_E_UNAUTHORIZED, err_code(r));
    cJSON_Delete(r);
    TEST_ASSERT_EQUAL_INT(0, s_calls);
    TEST_ASSERT_EQUAL_INT(0, s_locks);
}

static void test_auth_enabled_token_in_frame_or_transport(void)
{
    s_auth_on = true;
    cJSON *r = parse(rpc_handle_frame("{\"id\":1,\"method\":\"Test.Echo\",\"token\":\"good\"}", NULL, NULL));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "result"));
    cJSON_Delete(r);
    TEST_ASSERT_TRUE(s_seen.authed);
    TEST_ASSERT_EQUAL_STRING("good", s_seen_token);

    r = parse(rpc_handle_frame("{\"id\":2,\"method\":\"Test.Echo\"}", "good", NULL));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "result"));
    cJSON_Delete(r);
    TEST_ASSERT_EQUAL_INT(2, s_calls);
}

/* a transport that already signed the caller in (a WebSocket) vouches for it without a token */
static void test_frame_as_trusted_caller(void)
{
    s_auth_on = true;
    const rpc_caller_t ws = {.src = "10.0.0.7", .trusted = true};
    cJSON *r = parse(rpc_handle_frame_as("{\"id\":1,\"method\":\"Test.Echo\"}", &ws));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "result"));
    cJSON_Delete(r);
    TEST_ASSERT_TRUE(s_seen.authed);
    TEST_ASSERT_EQUAL_STRING("10.0.0.7", s_seen_src);
    const rpc_caller_t anon = {.src = "10.0.0.7"};
    r = parse(rpc_handle_frame_as("{\"id\":2,\"method\":\"Test.Echo\"}", &anon));
    TEST_ASSERT_EQUAL_INT(RPC_E_UNAUTHORIZED, err_code(r));
    cJSON_Delete(r);
    r = parse(rpc_handle_frame_as("{\"id\":3,\"method\":\"Auth.GetStatus\"}", NULL));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "result"));
    cJSON_Delete(r);
}

/* the session a transport signed the caller in with reaches the handler (Auth.Logout ends it) */
static const uint8_t *s_seen_session;
static cJSON *m_session(const cJSON *p, rpc_err_t *err)
{
    (void)p;
    (void)err;
    s_seen_session = rpc_current()->who.session;
    s_seen_token[0] = 0;
    if (rpc_current()->who.token) snprintf(s_seen_token, sizeof s_seen_token, "%s", rpc_current()->who.token);
    return NULL;
}

static void test_transport_session_reaches_handler(void)
{
    s_auth_on = true;
    rpc_register("Auth.Logout", m_session, 0);
    static const uint8_t hash[32] = {1, 2, 3};
    const rpc_caller_t ws = {.src = "10.0.0.7", .trusted = true, .session = hash};
    cJSON *r = parse(rpc_handle_frame_as("{\"id\":1,\"method\":\"Auth.Logout\"}", &ws));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(r, "result")));
    cJSON_Delete(r);
    TEST_ASSERT_EQUAL_PTR(hash, s_seen_session);
    TEST_ASSERT_EQUAL_STRING("", s_seen_token); /* no token text: the handler goes by the hash */
    const rpc_caller_t http = {.token = "good", .src = "10.0.0.8"};
    r = parse(rpc_handle_frame_as("{\"id\":2,\"method\":\"Auth.Logout\"}", &http));
    cJSON_Delete(r);
    TEST_ASSERT_NULL(s_seen_session);
    TEST_ASSERT_EQUAL_STRING("good", s_seen_token);
}

static void test_public_method_without_token(void)
{
    s_auth_on = true;
    cJSON *r = parse(rpc_handle_frame("{\"id\":1,\"method\":\"Auth.GetStatus\"}", NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetObjectItem(r, "result")->valuestring);
    cJSON_Delete(r);
}

static void test_rpc_call_trusted_and_untrusted(void)
{
    s_auth_on = true;
    rpc_err_t err = {0};
    const rpc_caller_t anon = {.src = "10.0.0.2"};
    TEST_ASSERT_NULL(rpc_call("Test.Echo", NULL, &anon, &err));
    TEST_ASSERT_EQUAL_INT(RPC_E_UNAUTHORIZED, err.code);

    const rpc_caller_t self = {.trusted = true};
    memset(&err, 0, sizeof err);
    cJSON *res = rpc_call("Test.Echo", NULL, &self, &err);
    TEST_ASSERT_TRUE(cJSON_IsObject(res));
    TEST_ASSERT_EQUAL_INT(0, err.code);
    TEST_ASSERT_EQUAL_STRING("local", s_seen_src);
    cJSON_Delete(res);

    memset(&err, 0, sizeof err);
    TEST_ASSERT_NULL(rpc_call("Nope.Nothing", NULL, &self, &err));
    TEST_ASSERT_EQUAL_INT(RPC_E_NO_METHOD, err.code);
}

static void test_list_and_flags(void)
{
    TEST_ASSERT_TRUE(rpc_method_exists("TEST.ECHO"));
    TEST_ASSERT_FALSE(rpc_method_exists("Test"));
    TEST_ASSERT_TRUE(rpc_method_is_public("Auth.GetStatus"));
    TEST_ASSERT_FALSE(rpc_method_is_public("Test.Echo"));
    cJSON *a = rpc_list_methods();
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(a));
    TEST_ASSERT_EQUAL_STRING("Test.Echo", cJSON_GetArrayItem(a, 0)->valuestring);
    cJSON_Delete(a);
}

static int s_overflows;
static void overflow(const char *name)
{
    (void)name;
    s_overflows++;
}

static void test_register_same_name_replaces(void)
{
    rpc_register("test.echo", m_public, RPC_F_PUBLIC);
    cJSON *a = rpc_list_methods();
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(a));
    cJSON_Delete(a);
    TEST_ASSERT_TRUE(rpc_method_is_public("Test.Echo"));
    cJSON *r = parse(rpc_handle_frame("{\"id\":1,\"method\":\"Test.Echo\"}", NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetObjectItem(r, "result")->valuestring);
    cJSON_Delete(r);
}

static char s_names[RPC_MAX_METHODS + 3][12];

static void test_table_full_drops(void)
{
    rpc_set_overflow_hook(overflow);
    for (int i = 0; i < RPC_MAX_METHODS + 3; i++) {
        snprintf(s_names[i], sizeof s_names[i], "Fill.F%d", i);
        rpc_register(s_names[i], m_null, 0);
    }
    TEST_ASSERT_EQUAL_INT(1, s_overflows);
    cJSON *a = rpc_list_methods();
    TEST_ASSERT_EQUAL_INT(RPC_MAX_METHODS, cJSON_GetArraySize(a));
    cJSON_Delete(a);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_error);
    RUN_TEST(test_deep_nesting_refused_before_parsing);
    RUN_TEST(test_query_value_whole_json_only);
    RUN_TEST(test_unknown_method);
    RUN_TEST(test_missing_method_and_bad_params);
    RUN_TEST(test_result_framed);
    RUN_TEST(test_numeric_id_no_src_no_params);
    RUN_TEST(test_notification_gets_no_reply);
    RUN_TEST(test_handler_error_and_null_result);
    RUN_TEST(test_auth_enabled_requires_token);
    RUN_TEST(test_auth_enabled_token_in_frame_or_transport);
    RUN_TEST(test_frame_as_trusted_caller);
    RUN_TEST(test_transport_session_reaches_handler);
    RUN_TEST(test_public_method_without_token);
    RUN_TEST(test_rpc_call_trusted_and_untrusted);
    RUN_TEST(test_list_and_flags);
    RUN_TEST(test_register_same_name_replaces);
    RUN_TEST(test_table_full_drops);
    return UNITY_END();
}
