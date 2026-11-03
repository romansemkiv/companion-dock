#include <stdint.h>
#include "unity.h"
#include "quickreset_logic.h"

void setUp(void) {}
void tearDown(void) {}

/* Starts 1 and 2 within the window do nothing; the third opens the hotspot. */
static void test_quickreset_third_start_triggers(void)
{
    uint8_t n = 0;
    TEST_ASSERT_EQUAL_INT(QR_NONE, quickreset_on_start(&n));
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_INT(QR_NONE, quickreset_on_start(&n));
    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_INT(QR_SETUP, quickreset_on_start(&n));
    TEST_ASSERT_EQUAL_UINT8(3, n);
}

/* A flapping PoE port looks like presses: the hotspot opens once, never again until cleared. */
static void test_quickreset_only_once(void)
{
    uint8_t n = 0;
    for (int i = 0; i < 3; i++) quickreset_on_start(&n);
    for (int i = 0; i < 20; i++) TEST_ASSERT_EQUAL_INT(QR_NONE, quickreset_on_start(&n));
    TEST_ASSERT_EQUAL_UINT8(23, n);
}

/* The count saturates at 255 and never wraps round to 3. */
static void test_quickreset_saturates(void)
{
    uint8_t n = 254;
    TEST_ASSERT_EQUAL_INT(QR_NONE, quickreset_on_start(&n));
    TEST_ASSERT_EQUAL_UINT8(255, n);
    TEST_ASSERT_EQUAL_INT(QR_NONE, quickreset_on_start(&n));
    TEST_ASSERT_EQUAL_UINT8(255, n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_quickreset_third_start_triggers);
    RUN_TEST(test_quickreset_only_once);
    RUN_TEST(test_quickreset_saturates);
    return UNITY_END();
}
