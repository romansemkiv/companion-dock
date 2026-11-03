#include <string.h>

#include "unity.h"
#include "ws_table.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t s_live[32]; /* the one live session in these tests */
static bool s_any_live;
static bool live(const uint8_t h[32]) { return s_any_live && !memcmp(h, s_live, 32); }

static void hash_of(uint8_t h[32], uint8_t v) { memset(h, v, 32); }

static void test_add_find_remove_full(void)
{
    wst_t t = {0};
    TEST_ASSERT_NULL(wst_add(&t, 0));
    wst_slot_t *a = wst_add(&t, 54);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_PTR(a, wst_add(&t, 54)); /* same socket, same slot */
    for (int fd = 55; fd < 54 + WST_MAX; fd++) TEST_ASSERT_NOT_NULL(wst_add(&t, fd));
    TEST_ASSERT_NULL(wst_add(&t, 99)); /* full */
    TEST_ASSERT_EQUAL_INT(WST_MAX, atomic_load(&t.n));
    wst_remove(&t, 54);
    wst_remove(&t, 54); /* twice: no-op */
    TEST_ASSERT_EQUAL_INT(WST_MAX - 1, atomic_load(&t.n));
    TEST_ASSERT_NULL(wst_find(&t, 54));
    TEST_ASSERT_NOT_NULL(wst_add(&t, 99));
}

static void test_sign_in_counts_and_listening(void)
{
    wst_t t = {0};
    wst_slot_t *a = wst_add(&t, 60), *b = wst_add(&t, 61);
    TEST_ASSERT_TRUE(wst_listening(&t, false)); /* open API: any socket listens */
    TEST_ASSERT_FALSE(wst_listening(&t, true)); /* with a password: only signed-in ones */
    uint8_t h1[32], h2[32];
    hash_of(h1, 1);
    hash_of(h2, 2);
    TEST_ASSERT_TRUE(wst_sign_in(&t, a, h1));
    TEST_ASSERT_FALSE(wst_sign_in(&t, a, h1)); /* the same session again: nothing new */
    TEST_ASSERT_TRUE(wst_sign_in(&t, a, h2));  /* another session replaces it, still one count */
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&t.n_signed));
    TEST_ASSERT_TRUE(wst_listening(&t, true));
    TEST_ASSERT_TRUE(wst_sign_in(&t, b, h1));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&t.n_signed));
    wst_remove(&t, 60); /* removing a signed-in socket takes both counts down */
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&t.n_signed));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&t.n));
    TEST_ASSERT_FALSE(wst_sign_in(&t, NULL, h1));
}

/* a logout (or expiry) that ends the session also ends the socket's sign-in */
static void test_session_end_drops_sign_in(void)
{
    wst_t t = {0};
    wst_slot_t *a = wst_add(&t, 70);
    hash_of(s_live, 7);
    s_any_live = true;
    TEST_ASSERT_TRUE(wst_sign_in(&t, a, s_live));
    TEST_ASSERT_TRUE(wst_authed(&t, a, live));
    TEST_ASSERT_TRUE(wst_listening(&t, true));
    s_any_live = false; /* Auth.Logout removed the session */
    TEST_ASSERT_FALSE(wst_authed(&t, a, live));
    TEST_ASSERT_FALSE(a->signed_in);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&t.n_signed));
    TEST_ASSERT_FALSE(wst_listening(&t, true));
    s_any_live = true; /* even if the session came back, the socket must sign in again */
    TEST_ASSERT_FALSE(wst_authed(&t, a, live));
    TEST_ASSERT_FALSE(wst_authed(&t, NULL, live));
    /* an explicit sign-out */
    TEST_ASSERT_TRUE(wst_sign_in(&t, a, s_live));
    wst_sign_out(&t, a);
    wst_sign_out(&t, a);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&t.n_signed));
    TEST_ASSERT_EACH_EQUAL_UINT8(0, a->hash, 32);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_add_find_remove_full);
    RUN_TEST(test_sign_in_counts_and_listening);
    RUN_TEST(test_session_end_drops_sign_in);
    return UNITY_END();
}
