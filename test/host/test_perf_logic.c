/* Sys.GetPerf's task shares (components/webui/perf_logic.c): run time over wall time, per mille of a core. */
#include "perf_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const perf_sample_t prev[] = {{0x1000, 5000}, {0x2000, 1000000}, {0x3000, 0}};

static void test_share_of_the_window(void)
{
    /* 250 ms of 1 s: 25.0 % */
    TEST_ASSERT_EQUAL_INT(250, perf_share_permille(prev, 3, true, 0x2000, 1250000, 1000000));
    /* nothing ran */
    TEST_ASSERT_EQUAL_INT(0, perf_share_permille(prev, 3, true, 0x1000, 5000, 1000000));
    /* tenths: 1.5 ms of 1 s */
    TEST_ASSERT_EQUAL_INT(1, perf_share_permille(prev, 3, true, 0x3000, 1500, 1000000));
}

static void test_never_over_a_whole_core(void)
{
    /* run time a little over the wall time (the two clocks are read apart) */
    TEST_ASSERT_EQUAL_INT(1000, perf_share_permille(prev, 3, true, 0x1000, 5000 + 1000100, 1000000));
}

static void test_a_new_task_counts_all_its_run_time(void)
{
    TEST_ASSERT_EQUAL_INT(100, perf_share_permille(prev, 3, true, 0x9000, 200000, 2000000));
}

static void test_a_counter_that_went_back_counts_from_zero(void)
{
    /* the handle 0x2000 reused by a task that has run 300 ms */
    TEST_ASSERT_EQUAL_INT(300, perf_share_permille(prev, 3, true, 0x2000, 300000, 1000000));
}

static void test_unknown_without_an_earlier_sample_or_time(void)
{
    TEST_ASSERT_EQUAL_INT(-1, perf_share_permille(NULL, 0, false, 0x1000, 5000, 1000000));
    TEST_ASSERT_EQUAL_INT(-1, perf_share_permille(prev, 3, true, 0x1000, 9000, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_share_of_the_window);
    RUN_TEST(test_never_over_a_whole_core);
    RUN_TEST(test_a_new_task_counts_all_its_run_time);
    RUN_TEST(test_a_counter_that_went_back_counts_from_zero);
    RUN_TEST(test_unknown_without_an_earlier_sample_or_time);
    return UNITY_END();
}
