#include <string.h>

#include "breadcrumb/breadcrumb.h"
#include "breadcrumb_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_encode_then_valid(void)
{
    bc_record_t r;
    memset(&r, 0xA5, sizeof r);
    bc_encode(&r, "mode switch", BC_STAGE_RESTARTING);
    TEST_ASSERT_TRUE(bc_valid(&r));
    TEST_ASSERT_EQUAL_STRING("mode switch", r.reason);
    TEST_ASSERT_EQUAL_UINT32(BC_STAGE_RESTARTING, r.stage);
}

static void test_random_memory_is_not_a_record(void)
{
    bc_record_t r;
    memset(&r, 0x00, sizeof r);
    TEST_ASSERT_FALSE(bc_valid(&r));
    memset(&r, 0xFF, sizeof r);
    TEST_ASSERT_FALSE(bc_valid(&r));
    uint32_t x = 0x12345678u; /* a pseudo-random fill */
    for (size_t i = 0; i < sizeof r; i++) {
        x = x * 1103515245u + 12345u;
        ((unsigned char *)&r)[i] = (unsigned char)(x >> 16);
    }
    TEST_ASSERT_FALSE(bc_valid(&r));
}

static void test_any_changed_byte_invalidates(void)
{
    bc_record_t good;
    bc_encode(&good, "OTA", BC_STAGE_SCHEDULED);
    for (size_t i = 0; i < sizeof good; i++) {
        for (int bit = 0; bit < 8; bit++) {
            bc_record_t r = good;
            ((unsigned char *)&r)[i] ^= (unsigned char)(1u << bit);
            TEST_ASSERT_FALSE_MESSAGE(bc_valid(&r), "a flipped bit passed");
        }
    }
}

static void test_clear_invalidates(void)
{
    bc_record_t r;
    bc_encode(&r, "Device.Restart", BC_STAGE_RESTARTING);
    bc_clear(&r);
    TEST_ASSERT_FALSE(bc_valid(&r));
}

static void test_unknown_stage_invalid(void)
{
    bc_record_t r;
    bc_encode(&r, "x", (bc_stage_t)7);
    TEST_ASSERT_FALSE(bc_valid(&r));
}

static void test_reason_cut_and_sanitised(void)
{
    bc_record_t r;
    bc_encode(&r, "a very long restart reason that does not fit", BC_STAGE_RESTARTING);
    TEST_ASSERT_TRUE(bc_valid(&r));
    TEST_ASSERT_EQUAL_size_t(BC_REASON_MAX - 1, strlen(r.reason));
    TEST_ASSERT_EQUAL_STRING_LEN("a very long restart rea", r.reason, BC_REASON_MAX - 1);
    bc_encode(&r, "bad\n\x01\xffname", BC_STAGE_RESTARTING);
    TEST_ASSERT_TRUE(bc_valid(&r));
    TEST_ASSERT_EQUAL_STRING("badname", r.reason);
    bc_encode(&r, NULL, BC_STAGE_RESTARTING);
    TEST_ASSERT_TRUE(bc_valid(&r));
    TEST_ASSERT_EQUAL_STRING("", r.reason);
}

static void test_no_nul_in_reason_invalid(void)
{
    /* a record whose checksum matches but whose reason runs to the end must not be read as a string */
    bc_record_t r;
    bc_encode(&r, "abc", BC_STAGE_RESTARTING);
    memset(r.reason, 'x', sizeof r.reason);
    TEST_ASSERT_FALSE(bc_valid(&r));
}

/* Every reason the firmware records (breadcrumb.h) is kept whole: printable and short enough. */
static void test_every_reason_fits(void)
{
    static const char *const why[] = {BC_WHY_API_RESTART,  BC_WHY_FACTORY_RESET, BC_WHY_MODE_SWITCH,
                                      BC_WHY_OTA,          BC_WHY_OTA_ROLLBACK,  BC_WHY_BUTTON_RESET,
                                      BC_WHY_USB_POWER,    BC_WHY_C6_UPDATE,     BC_WHY_C6_LOST};
    for (size_t i = 0; i < sizeof why / sizeof why[0]; i++) {
        bc_record_t r;
        bc_encode(&r, why[i], BC_STAGE_RESTARTING);
        TEST_ASSERT_TRUE(bc_valid(&r));
        TEST_ASSERT_EQUAL_STRING(why[i], r.reason);
    }
    char out[96];
    bc_record_t r;
    bc_encode(&r, BC_WHY_OTA_ROLLBACK, BC_STAGE_RESTARTING);
    bc_summary(out, sizeof out, "software restart", BC_RESET_DELIBERATE, &r);
    TEST_ASSERT_EQUAL_STRING("software restart: OTA self-check failed", out);
}

static void test_summary_deliberate(void)
{
    bc_record_t r;
    char out[96];
    bc_encode(&r, "mode switch", BC_STAGE_RESTARTING);
    bc_summary(out, sizeof out, "software restart", BC_RESET_DELIBERATE, &r);
    TEST_ASSERT_EQUAL_STRING("software restart: mode switch", out);
    bc_summary(out, sizeof out, "software restart", BC_RESET_DELIBERATE, NULL);
    TEST_ASSERT_EQUAL_STRING("software restart, no reason recorded", out);
}

static void test_summary_crash_while_pending(void)
{
    bc_record_t r;
    char out[96];
    bc_encode(&r, "mode switch", BC_STAGE_RESTARTING);
    bc_summary(out, sizeof out, "panic", BC_RESET_CRASH, &r);
    TEST_ASSERT_EQUAL_STRING("panic, last deliberate restart: mode switch pending", out);
    bc_encode(&r, "mode switch", BC_STAGE_SCHEDULED);
    bc_summary(out, sizeof out, "task watchdog", BC_RESET_CRASH, &r);
    TEST_ASSERT_EQUAL_STRING("task watchdog, last deliberate restart: mode switch pending (scheduled)", out);
}

static void test_summary_without_record(void)
{
    char out[96];
    bc_summary(out, sizeof out, "panic", BC_RESET_CRASH, NULL);
    TEST_ASSERT_EQUAL_STRING("panic", out);
    bc_record_t r;
    bc_encode(&r, "OTA", BC_STAGE_RESTARTING);
    bc_clear(&r);
    bc_summary(out, sizeof out, "panic", BC_RESET_CRASH, &r);
    TEST_ASSERT_EQUAL_STRING("panic", out);
}

static void test_summary_poweron_ignores_record(void)
{
    bc_record_t r;
    char out[96];
    bc_encode(&r, "OTA", BC_STAGE_RESTARTING);
    bc_summary(out, sizeof out, "power-on", BC_RESET_POWERON, &r);
    TEST_ASSERT_EQUAL_STRING("power-on", out);
}

static void test_summary_other_reset_with_record(void)
{
    bc_record_t r;
    char out[96];
    bc_encode(&r, "factory reset", BC_STAGE_SCHEDULED);
    bc_summary(out, sizeof out, "RESET pin", BC_RESET_OTHER, &r);
    TEST_ASSERT_EQUAL_STRING("RESET pin, last deliberate restart: factory reset pending (scheduled)", out);
}

static void test_summary_truncates_safely(void)
{
    bc_record_t r;
    char out[12];
    bc_encode(&r, "mode switch", BC_STAGE_RESTARTING);
    const size_t n = bc_summary(out, sizeof out, "panic", BC_RESET_CRASH, &r);
    TEST_ASSERT_EQUAL_size_t(sizeof out - 1, n);
    TEST_ASSERT_EQUAL_STRING("panic, last", out);
    char one[1] = {'z'};
    TEST_ASSERT_EQUAL_size_t(0, bc_summary(one, sizeof one, "panic", BC_RESET_CRASH, &r));
    TEST_ASSERT_EQUAL_CHAR('\0', one[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_then_valid);
    RUN_TEST(test_random_memory_is_not_a_record);
    RUN_TEST(test_any_changed_byte_invalidates);
    RUN_TEST(test_clear_invalidates);
    RUN_TEST(test_unknown_stage_invalid);
    RUN_TEST(test_reason_cut_and_sanitised);
    RUN_TEST(test_no_nul_in_reason_invalid);
    RUN_TEST(test_every_reason_fits);
    RUN_TEST(test_summary_deliberate);
    RUN_TEST(test_summary_crash_while_pending);
    RUN_TEST(test_summary_without_record);
    RUN_TEST(test_summary_poweron_ignores_record);
    RUN_TEST(test_summary_other_reset_with_record);
    RUN_TEST(test_summary_truncates_safely);
    return UNITY_END();
}
