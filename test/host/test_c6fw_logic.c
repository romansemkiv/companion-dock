/* c6fw_logic: when the ESP32-C6 gets the ESP-Hosted image this P4 build carries. */
#include "c6fw_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_needs_update_same_version(void)
{
    c6ver_t a = {2, 3, 1};
    TEST_ASSERT_FALSE(c6fw_needs_update(a, true, a));
}

/* Older or newer: a rolled-back P4 puts its own C6 firmware back. */
static void test_needs_update_any_other_version(void)
{
    c6ver_t a = {2, 3, 1}, b = {2, 4, 0};
    TEST_ASSERT_TRUE(c6fw_needs_update(a, true, b));
    TEST_ASSERT_TRUE(c6fw_needs_update(b, true, a));
    c6ver_t major = {3, 0, 9}, other = {2, 9, 9};
    TEST_ASSERT_TRUE(c6fw_needs_update(other, true, major));
    TEST_ASSERT_FALSE(c6fw_needs_update(major, true, major));
}

/* No answer about its version: update. */
static void test_needs_update_unknown_version(void)
{
    c6ver_t a = {2, 3, 1};
    TEST_ASSERT_TRUE(c6fw_needs_update(a, false, a));
}

static const c6ver_t carried = {3, 1, 0}, minor = {3, 0, 9}, other = {2, 9, 9};

static void test_decide_ok_when_carried(void)
{
    TEST_ASSERT_EQUAL_INT(C6FW_OK, c6fw_decide(carried, true, carried, false, 0, false));
}

static void test_decide_update_when_confirmed(void)
{
    TEST_ASSERT_EQUAL_INT(C6FW_UPDATE, c6fw_decide(minor, true, carried, false, 0, false));
    TEST_ASSERT_EQUAL_INT(C6FW_UPDATE, c6fw_decide(minor, false, carried, false, 0, false));
}

/* The image not confirmed yet (first start after an OTA) never restarts for the
 * C6: the bootloader would take it for a failed start and roll back. */
static void test_decide_waits_for_confirmation(void)
{
    TEST_ASSERT_EQUAL_INT(C6FW_WAIT_CONFIRM, c6fw_decide(minor, true, carried, true, 0, false));
    TEST_ASSERT_EQUAL_INT(C6FW_WAIT_CONFIRM, c6fw_decide(other, true, carried, true, 0, false));
}

/* Two updates since the power-on that did not take: none more until the next. */
static void test_decide_gives_up_after_max_tries(void)
{
    TEST_ASSERT_EQUAL_INT(C6FW_UPDATE, c6fw_decide(minor, true, carried, false, 1, false));
    TEST_ASSERT_EQUAL_INT(C6FW_GIVE_UP, c6fw_decide(minor, true, carried, false, C6FW_MAX_TRIES, false));
    TEST_ASSERT_EQUAL_INT(C6FW_GIVE_UP, c6fw_decide(minor, true, carried, false, C6FW_MAX_TRIES + 1, false));
    TEST_ASSERT_EQUAL_INT(C6FW_OK, c6fw_decide(carried, true, carried, false, C6FW_MAX_TRIES, false));
}

/* A build without build-c6/ carries no image: it never updates, only reports. */
static void test_decide_no_image_never_updates(void)
{
    TEST_ASSERT_EQUAL_INT(C6FW_MISMATCH, c6fw_decide(minor, true, carried, false, 0, true));
    TEST_ASSERT_EQUAL_INT(C6FW_MISMATCH, c6fw_decide(minor, true, carried, true, 0, true));
    TEST_ASSERT_EQUAL_INT(C6FW_MISMATCH, c6fw_decide(minor, false, carried, false, 0, true));
    TEST_ASSERT_EQUAL_INT(C6FW_OK, c6fw_decide(carried, true, carried, true, 0, true));
}

/* Wi-Fi meanwhile: with a C6 of the same major version (the same protocol). */
static void test_usable_same_major_only(void)
{
    TEST_ASSERT_TRUE(c6fw_usable(minor, true, carried));
    TEST_ASSERT_TRUE(c6fw_usable(carried, true, carried));
    TEST_ASSERT_FALSE(c6fw_usable(other, true, carried));
    TEST_ASSERT_FALSE(c6fw_usable(carried, false, carried));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_needs_update_same_version);
    RUN_TEST(test_needs_update_any_other_version);
    RUN_TEST(test_needs_update_unknown_version);
    RUN_TEST(test_decide_ok_when_carried);
    RUN_TEST(test_decide_update_when_confirmed);
    RUN_TEST(test_decide_waits_for_confirmation);
    RUN_TEST(test_decide_gives_up_after_max_tries);
    RUN_TEST(test_decide_no_image_never_updates);
    RUN_TEST(test_usable_same_major_only);
    return UNITY_END();
}
