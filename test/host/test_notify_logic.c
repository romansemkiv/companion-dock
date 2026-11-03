#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "notify_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t rng = 0x85EBCA6Bu;
static uint32_t rnd(void) /* xorshift32: the same cases every run */
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* notify.c's earlier key, kept as the reference: a copy of the status with the fields deleted, printed. */
static char *old_key(const cJSON *st)
{
    cJSON *cmp = cJSON_Duplicate(st, true);
    cJSON_DeleteItemFromObjectCaseSensitive(cmp, "uptime_s");
    cJSON_DeleteItemFromObjectCaseSensitive(cmp, "heap");
    cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(cmp, "message"), "remaining_s");
    char *key = cJSON_PrintUnformatted(cmp);
    cJSON_Delete(cmp);
    return key;
}

/* The frame notify.c pushes for st (params.sys: st, by reference, so st stays the caller's). */
static char *frame_of(cJSON *st)
{
    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "src", "companionbox-80f1b2d68d1c");
    cJSON_AddStringToObject(frame, "dst", "user");
    cJSON_AddStringToObject(frame, "method", "NotifyStatus");
    cJSON *params = cJSON_AddObjectToObject(frame, "params");
    cJSON_AddItemReferenceToObject(params, "sys", st);
    char *text = cJSON_PrintUnformatted(frame);
    cJSON_Delete(frame);
    return text;
}

static const char *const NAMES[] = {"name", "uptime_s", "mode", "net", "heap", "message", "remaining_s",
                                    "decks", "active", "text", "Heap", "UPTIME_S", "internal_free", "lastReset"};
#define NNAMES (int)(sizeof NAMES / sizeof NAMES[0])

static cJSON *rand_value(int depth);

/* An object of random fields from NAMES (a name may repeat, the fields to leave out anywhere, at any depth). */
static cJSON *rand_object(int depth)
{
    cJSON *o = cJSON_CreateObject();
    const int n = (int)(rnd() % 7);
    for (int i = 0; i < n; i++) cJSON_AddItemToObject(o, NAMES[rnd() % NNAMES], rand_value(depth + 1));
    return o;
}

static cJSON *rand_value(int depth)
{
    static const char *const S[] = {"", "Stream Deck XL", "a \"quoted\" \\ text\n", "\"uptime_s\":0", "caf\xc3\xa9"};
    switch (rnd() % (depth < 3 ? 7 : 4)) {
    case 0: return cJSON_CreateNumber((double)(int32_t)rnd() / (rnd() % 2 ? 1 : 1000));
    case 1: return cJSON_CreateString(S[rnd() % 5]);
    case 2: return cJSON_CreateBool(rnd() % 2);
    case 3: return cJSON_CreateNull();
    case 4:
    case 5: return rand_object(depth);
    default: {
        cJSON *a = cJSON_CreateArray();
        const int n = (int)(rnd() % 4);
        for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, rand_value(depth + 1));
        return a;
    }
    }
}

/* A status as Sys.GetStatus makes it (the fields in place), or with them missing, moved, repeated or of
 * another type, among random fields. */
static cJSON *rand_status(void)
{
    if (rnd() % 4 == 0) return rand_object(0);
    cJSON *st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "name", "CompanionBox-8D1C");
    if (rnd() % 8) cJSON_AddNumberToObject(st, "uptime_s", rnd() % 100000);
    for (int i = (int)(rnd() % 4); i > 0; i--) cJSON_AddItemToObject(st, NAMES[rnd() % NNAMES], rand_value(1));
    if (rnd() % 8) {
        cJSON *m = cJSON_AddObjectToObject(st, "message");
        cJSON_AddBoolToObject(m, "active", rnd() % 2);
        cJSON_AddStringToObject(m, "text", rnd() % 2 ? "Back at 3" : "");
        if (rnd() % 8) cJSON_AddNumberToObject(m, "remaining_s", rnd() % 600);
        if (rnd() % 4 == 0) cJSON_AddItemToObject(m, NAMES[rnd() % NNAMES], rand_value(2));
    }
    for (int i = (int)(rnd() % 3); i > 0; i--) cJSON_AddItemToObject(st, NAMES[rnd() % NNAMES], rand_value(1));
    if (rnd() % 8) {
        cJSON *h = cJSON_AddObjectToObject(st, "heap");
        cJSON_AddNumberToObject(h, "internal_free", rnd() % 300000);
        cJSON_AddNumberToObject(h, "psram_free", rnd() % 30000000);
    }
    if (rnd() % 2) cJSON_AddStringToObject(st, "lastReset", "power-on");
    return st;
}

/* the key is the old one byte for byte (so every push decision is the old one), and the status, and so the
 * frame pushed, is as it was, over 20k random statuses. */
static void test_key_and_frame_match_old(void)
{
    for (int it = 0; it < 20000; it++) {
        cJSON *st = rand_status();
        char *before = cJSON_PrintUnformatted(st), *frame_before = frame_of(st), *want = old_key(st);
        TEST_ASSERT_NOT_NULL(before);
        TEST_ASSERT_NOT_NULL(want);
        char *key = notify_status_key(st);
        TEST_ASSERT_NOT_NULL(key);
        TEST_ASSERT_EQUAL_STRING(want, key);
        char *after = cJSON_PrintUnformatted(st), *frame_after = frame_of(st);
        TEST_ASSERT_EQUAL_STRING(before, after);
        TEST_ASSERT_EQUAL_STRING(frame_before, frame_after);
        cJSON_free(before);
        cJSON_free(frame_before);
        cJSON_free(want);
        cJSON_free(key);
        cJSON_free(after);
        cJSON_free(frame_after);
        cJSON_Delete(st);
    }
}

/* What the key leaves out and what it keeps (the push decision: a new key, a push). */
static void test_key_ignores_self_changing_fields_only(void)
{
    const char *a = "{\"name\":\"B\",\"uptime_s\":5,\"message\":{\"active\":true,\"text\":\"Hi\",\"remaining_s\":30},"
                    "\"heap\":{\"internal_free\":1000,\"psram_free\":2000},\"lastReset\":\"power-on\"}";
    const char *b = "{\"name\":\"B\",\"uptime_s\":6,\"message\":{\"active\":true,\"text\":\"Hi\",\"remaining_s\":29},"
                    "\"heap\":{\"internal_free\":900,\"psram_free\":2100},\"lastReset\":\"power-on\"}";
    const char *c = "{\"name\":\"B\",\"uptime_s\":6,\"message\":{\"active\":true,\"text\":\"Ho\",\"remaining_s\":29},"
                    "\"heap\":{\"internal_free\":900,\"psram_free\":2100},\"lastReset\":\"power-on\"}";
    cJSON *ja = cJSON_Parse(a), *jb = cJSON_Parse(b), *jc = cJSON_Parse(c);
    char *ka = notify_status_key(ja), *kb = notify_status_key(jb), *kc = notify_status_key(jc);
    TEST_ASSERT_EQUAL_STRING("{\"name\":\"B\",\"message\":{\"active\":true,\"text\":\"Hi\"},\"lastReset\":\"power-on\"}", ka);
    TEST_ASSERT_EQUAL_STRING(ka, kb);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(ka, kc));
    char *pa = cJSON_PrintUnformatted(ja);
    TEST_ASSERT_EQUAL_STRING(a, pa); /* put back as it was */
    TEST_ASSERT_NULL(notify_status_key(NULL));
    cJSON_free(ka);
    cJSON_free(kb);
    cJSON_free(kc);
    cJSON_free(pa);
    cJSON_Delete(ja);
    cJSON_Delete(jb);
    cJSON_Delete(jc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_and_frame_match_old);
    RUN_TEST(test_key_ignores_self_changing_fields_only);
    return UNITY_END();
}
