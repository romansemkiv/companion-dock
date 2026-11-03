#include <stdbool.h>
#include <stdint.h>
#include "unity.h"
#include "svcbtn_logic.h"

void setUp(void) {}
void tearDown(void) {}

#define STEP_US 10000LL /* svcbtn.c polls every 10 ms */

typedef struct {
    int setup_stage, reset_stage, other;
    int64_t setup_at, reset_at; /* time held when each stage fired */
} held_t;

/* Holds the (armed, already pressed at t0) button down until t0 + hold_us, polling every STEP_US. */
static held_t hold(svcbtn_t *b, int64_t *t, int64_t t0, int64_t hold_us)
{
    held_t h = {0};
    while (*t + STEP_US <= t0 + hold_us) {
        *t += STEP_US;
        svc_event_t e = svcbtn_step(b, true, *t);
        if (e == SVC_SETUP_STAGE) {
            h.setup_stage++;
            h.setup_at = *t - t0;
        } else if (e == SVC_RESET_STAGE) {
            h.reset_stage++;
            h.reset_at = *t - t0;
        } else if (e != SVC_NONE) {
            h.other++;
        }
    }
    return h;
}

/* Held (or miswired to GND) from the start: never a press, however long; nor its release. */
static void test_svcbtn_down_from_start_never_counts(void)
{
    svcbtn_t b;
    svcbtn_init(&b);
    int64_t t = 0;
    for (int i = 0; i < 400; i++) { /* 20 s */
        t += STEP_US;
        TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, true, t));
    }
    TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, false, t += STEP_US));
    /* armed now: the next press counts */
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
}

/* Pressed at start-up and let go after a second: not a short press either. */
static void test_svcbtn_down_from_start_short_never_counts(void)
{
    svcbtn_t b;
    svcbtn_init(&b);
    TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, true, STEP_US));
    TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, true, 500000));
    TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, false, 1000000));
}

/* Released at the start: armed at once. */
static void test_svcbtn_released_at_start_arms(void)
{
    svcbtn_t b;
    svcbtn_init(&b);
    TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, false, STEP_US));
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, 2 * STEP_US));
}

/* Released held_us after the press (polled at release only): what the release says. */
static svc_event_t press_for(int64_t held_us)
{
    svcbtn_t b;
    svcbtn_init(&b);
    int64_t t = STEP_US;
    svcbtn_step(&b, false, t);
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
    return svcbtn_step(&b, false, t + held_us);
}

/* A short press: released after 50 ms up to (not including) 2 s: SVC_SHORT; shorter is a bounce. */
static void test_svcbtn_short_press(void)
{
    TEST_ASSERT_EQUAL_INT(SVC_NONE, press_for(0));
    TEST_ASSERT_EQUAL_INT(SVC_NONE, press_for(49000));
    TEST_ASSERT_EQUAL_INT(SVC_SHORT, press_for(50000));
    TEST_ASSERT_EQUAL_INT(SVC_SHORT, press_for(1000000));
    TEST_ASSERT_EQUAL_INT(SVC_SHORT, press_for(1999000));
    TEST_ASSERT_EQUAL_INT(SVC_SHORT, press_for(SVC_SHORT_MAX_US - 1));
    TEST_ASSERT_EQUAL_INT(SVC_NONE, press_for(SVC_SHORT_MAX_US));
    /* polled every 10 ms: released at the poll 1910 ms in */
    svcbtn_t b;
    svcbtn_init(&b);
    int64_t t = STEP_US;
    svcbtn_step(&b, false, t);
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
    held_t h = hold(&b, &t, t, 1900000LL);
    TEST_ASSERT_EQUAL_INT(0, h.setup_stage + h.reset_stage + h.other);
    TEST_ASSERT_EQUAL_INT(SVC_SHORT, svcbtn_step(&b, false, t += STEP_US));
    TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, false, t += STEP_US));
}

/* Held 2 s up to the 5 s stage: nothing on release (neither short nor the hotspot). */
static void test_svcbtn_between_short_and_setup_does_nothing(void)
{
    TEST_ASSERT_EQUAL_INT(SVC_NONE, press_for(2000000));
    TEST_ASSERT_EQUAL_INT(SVC_NONE, press_for(3000000));
    svcbtn_t b;
    svcbtn_init(&b);
    int64_t t = STEP_US;
    svcbtn_step(&b, false, t);
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
    held_t h = hold(&b, &t, t, 4950000LL);
    TEST_ASSERT_EQUAL_INT(0, h.setup_stage + h.reset_stage + h.other);
    TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, false, t += STEP_US));
}

/* Held 5 s: the setup stage exactly at 5 s, once; released before 10 s: setup. */
static void test_svcbtn_hold_5s_setup(void)
{
    svcbtn_t b;
    svcbtn_init(&b);
    int64_t t = STEP_US;
    svcbtn_step(&b, false, t);
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
    const int64_t t0 = t;
    held_t h = hold(&b, &t, t0, 9950000LL);
    TEST_ASSERT_EQUAL_INT(1, h.setup_stage);
    TEST_ASSERT_EQUAL_INT(0, h.reset_stage);
    TEST_ASSERT_EQUAL_INT(0, h.other);
    TEST_ASSERT_EQUAL_INT64(SVC_SETUP_US, h.setup_at);
    TEST_ASSERT_EQUAL_INT(SVC_SETUP, svcbtn_step(&b, false, t += STEP_US));
    TEST_ASSERT_EQUAL_INT(SVC_NONE, svcbtn_step(&b, false, t += STEP_US));
}

/* Held 10 s: both stages once (reset exactly at 10 s); released: the factory reset. */
static void test_svcbtn_hold_10s_reset(void)
{
    svcbtn_t b;
    svcbtn_init(&b);
    int64_t t = STEP_US;
    svcbtn_step(&b, false, t);
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
    const int64_t t0 = t;
    held_t h = hold(&b, &t, t0, 30000000LL); /* held on well past 10 s: still one reset */
    TEST_ASSERT_EQUAL_INT(1, h.setup_stage);
    TEST_ASSERT_EQUAL_INT(1, h.reset_stage);
    TEST_ASSERT_EQUAL_INT(0, h.other);
    TEST_ASSERT_EQUAL_INT64(SVC_SETUP_US, h.setup_at);
    TEST_ASSERT_EQUAL_INT64(SVC_RESET_US, h.reset_at);
    TEST_ASSERT_EQUAL_INT(SVC_RESET, svcbtn_step(&b, false, t += STEP_US));
}

/* After an action the next press starts from zero again. */
static void test_svcbtn_next_press_starts_over(void)
{
    svcbtn_t b;
    svcbtn_init(&b);
    int64_t t = STEP_US;
    svcbtn_step(&b, false, t);
    svcbtn_step(&b, true, t += STEP_US);
    hold(&b, &t, t, 6000000LL);
    TEST_ASSERT_EQUAL_INT(SVC_SETUP, svcbtn_step(&b, false, t += STEP_US));
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
    held_t h = hold(&b, &t, t, 1000000LL);
    TEST_ASSERT_EQUAL_INT(0, h.setup_stage + h.reset_stage + h.other);
    TEST_ASSERT_EQUAL_INT(SVC_SHORT, svcbtn_step(&b, false, t += STEP_US));
    /* and after the reset stage too: a short press is short again */
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
    hold(&b, &t, t, 11000000LL);
    TEST_ASSERT_EQUAL_INT(SVC_RESET, svcbtn_step(&b, false, t += STEP_US));
    TEST_ASSERT_EQUAL_INT(SVC_PRESSED, svcbtn_step(&b, true, t += STEP_US));
    TEST_ASSERT_EQUAL_INT(SVC_SHORT, svcbtn_step(&b, false, t += 200000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_svcbtn_down_from_start_never_counts);
    RUN_TEST(test_svcbtn_released_at_start_arms);
    RUN_TEST(test_svcbtn_short_press);
    RUN_TEST(test_svcbtn_between_short_and_setup_does_nothing);
    RUN_TEST(test_svcbtn_down_from_start_short_never_counts);
    RUN_TEST(test_svcbtn_hold_5s_setup);
    RUN_TEST(test_svcbtn_hold_10s_reset);
    RUN_TEST(test_svcbtn_next_press_starts_over);
    return UNITY_END();
}
