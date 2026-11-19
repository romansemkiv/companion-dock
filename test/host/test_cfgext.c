#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cfgext_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char *const KEYS[] = {"companionAuto", "companionHost", "companionPort", NULL};
static const char *const RO[] = {"wifiHasPass", NULL};

static bool in_list(const char *const *list, const char *key)
{
    for (; list && *list; list++)
        if (!strcmp(*list, key)) return true;
    return false;
}

/* 0.2.8's set_section filter, kept as the reference: dry_run and the ro keys skipped, a key not in keys refused. NULL
 * when refused (err set). */
static cJSON *old_filter(const cJSON *p, const char *const *keys, const char *const *ro, char *err, size_t n)
{
    cJSON *f = cJSON_CreateObject();
    const cJSON *it;
    cJSON_ArrayForEach(it, p) {
        if (!strcmp(it->string, "dry_run") || in_list(ro, it->string)) continue;
        if (!in_list(keys, it->string)) {
            cJSON_Delete(f);
            snprintf(err, n, "unknown key %s", it->string);
            return NULL;
        }
        cJSON_AddItemToObject(f, it->string, cJSON_Duplicate(it, true));
    }
    return f;
}

static void expect_json(const char *want, const cJSON *got)
{
    TEST_ASSERT_NOT_NULL(got);
    char *s = cJSON_PrintUnformatted(got);
    TEST_ASSERT_EQUAL_STRING(want, s);
    cJSON_free(s);
}

/* With no extension the built-in part is exactly 0.2.8's filtered params, and a refusal is its refusal. */
static void test_split_builtin_only(void)
{
    static const char *const cases[] = {
        "{}",
        "{\"dry_run\":true}",
        "{\"companionPort\":1}",
        "{\"companionPort\":1,\"wifiHasPass\":true,\"dry_run\":false,\"companionHost\":\"h\"}",
        "{\"companionAuto\":true,\"companionAuto\":false}",
        "{\"companionPort\":1,\"zz\":2}",
        "{\"dry_run\":1,\"companionHost\":{\"a\":[1,2]}}",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cJSON *p = cJSON_Parse(cases[i]);
        TEST_ASSERT_NOT_NULL(p);
        char e_old[128] = "", e_new[128] = "";
        cJSON *want = old_filter(p, KEYS, RO, e_old, sizeof e_old);
        cJSON *b = (cJSON *)1;
        const bool ok = cfgext_split(p, KEYS, RO, NULL, 0, &b, NULL, e_new, sizeof e_new);
        TEST_ASSERT_EQUAL_MESSAGE(want != NULL, ok, cases[i]);
        if (want) {
            char *w = cJSON_PrintUnformatted(want);
            expect_json(w, b);
            cJSON_free(w);
        } else {
            TEST_ASSERT_NULL(b);
            TEST_ASSERT_EQUAL_STRING(e_old, e_new);
        }
        cJSON_Delete(want);
        cJSON_Delete(b);
        cJSON_Delete(p);
    }
}

static void test_split_ext_keys(void)
{
    static const char *const x1[] = {"x1", NULL};
    const char *const *ext[] = {x1};
    cJSON *p = cJSON_Parse("{\"companionPort\":1,\"x1\":true}");
    cJSON *b = NULL, *x[1] = {NULL};
    char e[128] = "";
    TEST_ASSERT_TRUE(cfgext_split(p, KEYS, RO, ext, 1, &b, x, e, sizeof e));
    expect_json("{\"companionPort\":1}", b);
    expect_json("{\"x1\":true}", x[0]);
    cJSON_Delete(b);
    cJSON_Delete(x[0]);
    cJSON_Delete(p);

    /* extension keys only: the built-in part is empty; dry_run and the ro keys go to neither */
    p = cJSON_Parse("{\"dry_run\":true,\"x1\":{\"a\":1},\"wifiHasPass\":false}");
    TEST_ASSERT_TRUE(cfgext_split(p, KEYS, RO, ext, 1, &b, x, e, sizeof e));
    expect_json("{}", b);
    expect_json("{\"x1\":{\"a\":1}}", x[0]);
    cJSON_Delete(b);
    cJSON_Delete(x[0]);
    cJSON_Delete(p);

    /* built-in keys only: the extension gets nothing (NULL) */
    p = cJSON_Parse("{\"companionHost\":\"h\"}");
    x[0] = (cJSON *)1;
    TEST_ASSERT_TRUE(cfgext_split(p, KEYS, RO, ext, 1, &b, x, e, sizeof e));
    expect_json("{\"companionHost\":\"h\"}", b);
    TEST_ASSERT_NULL(x[0]);
    cJSON_Delete(b);
    cJSON_Delete(p);
}

static void test_split_unknown(void)
{
    static const char *const x1[] = {"x1", NULL};
    const char *const *ext[] = {x1};
    cJSON *p = cJSON_Parse("{\"x1\":true,\"companionPort\":1,\"zz\":0}");
    cJSON *b = (cJSON *)1, *x[1] = {(cJSON *)1};
    char e[128] = "";
    TEST_ASSERT_FALSE(cfgext_split(p, KEYS, RO, ext, 1, &b, x, e, sizeof e));
    TEST_ASSERT_EQUAL_STRING("unknown key zz", e);
    TEST_ASSERT_NULL(b); /* nothing left to free */
    TEST_ASSERT_NULL(x[0]);
    cJSON_Delete(p);
}

static void test_split_two_exts(void)
{
    static const char *const x1[] = {"x1", "x2", NULL};
    static const char *const y1[] = {"y1", NULL};
    static const char *const z1[] = {"z1", NULL};
    const char *const *ext[] = {x1, y1, z1};
    cJSON *p = cJSON_Parse("{\"y1\":\"a\",\"x2\":2,\"companionAuto\":false,\"x1\":1}");
    cJSON *b = NULL, *x[3] = {NULL, NULL, (cJSON *)1};
    char e[128] = "";
    TEST_ASSERT_TRUE(cfgext_split(p, KEYS, RO, ext, 3, &b, x, e, sizeof e));
    expect_json("{\"companionAuto\":false}", b);
    expect_json("{\"x2\":2,\"x1\":1}", x[0]);
    expect_json("{\"y1\":\"a\"}", x[1]);
    TEST_ASSERT_NULL(x[2]); /* none of its keys */
    cJSON_Delete(b);
    cJSON_Delete(x[0]);
    cJSON_Delete(x[1]);
    cJSON_Delete(p);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_split_builtin_only);
    RUN_TEST(test_split_ext_keys);
    RUN_TEST(test_split_unknown);
    RUN_TEST(test_split_two_exts);
    return UNITY_END();
}
