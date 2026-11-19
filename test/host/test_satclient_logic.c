#include <string.h>

#include "unity.h"
#include "satclient_logic.h"

void setUp(void) {}
void tearDown(void) {}

/* a queued key press must never wait for the fallback timeout once the session is ready. */
static void test_pending_work_wakes_the_loop(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, satc_wait_ms(1, true, 50));
    TEST_ASSERT_EQUAL_UINT32(0, satc_wait_ms(32, true, 50));
}

static void test_idle_waits_for_the_fallback(void)
{
    TEST_ASSERT_EQUAL_UINT32(50, satc_wait_ms(0, true, 50));
    TEST_ASSERT_EQUAL_UINT32(50, satc_wait_ms(0, false, 50));
}

/* Before READY queued items are dropped right after the wait, so they don't shorten it. */
static void test_not_ready_keeps_the_fallback(void)
{
    TEST_ASSERT_EQUAL_UINT32(50, satc_wait_ms(3, false, 50));
}

/* ---- satc_write_all's budget counts only the waits since the last progress ---- */

typedef struct {
    int every;    /* takes up to `take` bytes on every `every`-th call, else would block (0: never takes any) */
    size_t take;
    size_t limit; /* takes nothing more once it has this many (0: no limit) */
    int calls, waits, waits_since; /* waits in all, and since its last bytes */
    bool fail;    /* a hard error instead of blocking */
    size_t got;
    char sink[8192];
} sink_t;

static long sink_send(void *ctx, const char *p, size_t n, bool *again)
{
    sink_t *k = ctx;
    k->calls++;
    *again = false;
    if (k->fail) return -1;
    if (!k->every || k->calls % k->every || (k->limit && k->got >= k->limit)) {
        *again = true;
        return -1;
    }
    const size_t t = n < k->take ? n : k->take;
    memcpy(k->sink + k->got, p, t);
    k->got += t;
    k->waits_since = 0;
    return (long)t;
}

static void sink_wait(void *ctx)
{
    sink_t *k = ctx;
    k->waits++;
    k->waits_since++;
}

static sink_t K;

/* data: lines of `line` bytes, each ending in '\n' (a rotation batch is ~70 B lines) */
static void lines(char *data, size_t len, size_t line)
{
    for (size_t i = 0; i < len; i++) data[i] = (i + 1) % line ? (char)('a' + i % 26) : '\n';
}

/* A reader that takes 80 bytes every 1.45 s (29 waits of 50 ms), a whole 70 B line each time, gets the whole 7 KB
 * batch (a 100-step rotation), the same bytes in order, though it takes far longer in all than one 2 s budget: each
 * line gets its own budget, as when each line was its own write. */
static void test_write_all_slow_reader_never_times_out(void)
{
    static char data[7000];
    lines(data, sizeof data, 70);
    memset(&K, 0, sizeof K);
    K.every = 30;
    K.take = 80;
    const satc_io_t io = {sink_send, sink_wait, &K};
    TEST_ASSERT_EQUAL_size_t(sizeof data, satc_write_all(&io, data, sizeof data, 2000, 50));
    TEST_ASSERT_EQUAL_MEMORY(data, K.sink, sizeof data);
    TEST_ASSERT_TRUE(K.waits * 50 > 2000 * 10);
}

/* a peer trickling a few bytes every 1.45 s never ends a line in 2 s: the write times out on that line, as
 * when every line was its own write (it could otherwise hold the client task for minutes). */
static void test_write_all_trickle_times_out_on_a_line(void)
{
    static char data[7000];
    lines(data, sizeof data, 70);
    memset(&K, 0, sizeof K);
    K.every = 30;
    K.take = 3;
    const satc_io_t io = {sink_send, sink_wait, &K};
    const size_t sent = satc_write_all(&io, data, sizeof data, 2000, 50);
    TEST_ASSERT_TRUE(sent < 70);          /* not even the first line */
    TEST_ASSERT_TRUE(K.waits <= 2000 / 50 + 1); /* about 2 s of waits, not minutes */
}

/* A reader that takes nothing gets the budget, then the write ends (nothing sent); one that stops at a line end gets
 * the full budget from it; a hard error ends it at once. */
static void test_write_all_stalled_reader_times_out(void)
{
    memset(&K, 0, sizeof K);
    const satc_io_t io = {sink_send, sink_wait, &K};
    TEST_ASSERT_EQUAL_size_t(0, satc_write_all(&io, "KEY-ROTATE\n", 11, 2000, 50));
    TEST_ASSERT_EQUAL_INT(2000 / 50, K.waits);
    static char data[600];
    lines(data, sizeof data, 50);
    memset(&K, 0, sizeof K);
    K.every = 10;
    K.take = 50;
    K.limit = 200; /* 4 lines, then nothing */
    TEST_ASSERT_EQUAL_size_t(200, satc_write_all(&io, data, sizeof data, 2000, 50));
    TEST_ASSERT_EQUAL_INT(2000 / 50, K.waits_since); /* the full budget after the last line end */
    TEST_ASSERT_TRUE(K.waits > 2000 / 50);           /* more waits in all */
    memset(&K, 0, sizeof K);
    K.fail = true;
    TEST_ASSERT_EQUAL_size_t(0, satc_write_all(&io, data, sizeof data, 2000, 50));
    TEST_ASSERT_EQUAL_INT(0, K.waits);
}

/* ---- the outcome of a connection that has ended ---- */

static void test_end_event(void)
{
    TEST_ASSERT_EQUAL_INT(SATC_EV_ATTEMPT_FAILED, satc_end_event(false, false, false, false));
    TEST_ASSERT_EQUAL_INT(SATC_EV_ATTEMPT_FAILED, satc_end_event(true, false, false, false));
    TEST_ASSERT_EQUAL_INT(SATC_EV_DROPPED, satc_end_event(true, true, false, false));
}

/* a deliberate change of target, or a too-old Companion, is no outcome to report. */
static void test_end_event_silent_cases(void)
{
    for (int c = 0; c < 2; c++)
        for (int r = 0; r < 2; r++) {
            TEST_ASSERT_EQUAL_INT(-1, satc_end_event(c, r, true, false));
            TEST_ASSERT_EQUAL_INT(-1, satc_end_event(c, r, false, true));
            TEST_ASSERT_EQUAL_INT(-1, satc_end_event(c, r, true, true));
        }
}

/* The wait between connection attempts: 500 ms, doubled up to 2 s, then 5 s each time. */
static void test_backoff_grows(void)
{
    TEST_ASSERT_EQUAL_UINT32(500, SATC_BACKOFF_FIRST_MS);
    TEST_ASSERT_EQUAL_UINT32(1000, satc_next_backoff(500, false));
    TEST_ASSERT_EQUAL_UINT32(2000, satc_next_backoff(1000, false));
    TEST_ASSERT_EQUAL_UINT32(5000, satc_next_backoff(2000, false));
    TEST_ASSERT_EQUAL_UINT32(5000, satc_next_backoff(5000, false));
}

/* A wait cut short by a change of target starts the new target's waits from the first step: its next attempts come
 * quickly instead of one attempt per 5 s. */
static void test_backoff_resets_on_target_change(void)
{
    TEST_ASSERT_EQUAL_UINT32(SATC_BACKOFF_FIRST_MS, satc_next_backoff(5000, true));
    TEST_ASSERT_EQUAL_UINT32(SATC_BACKOFF_FIRST_MS, satc_next_backoff(2000, true));
    TEST_ASSERT_EQUAL_UINT32(SATC_BACKOFF_FIRST_MS, satc_next_backoff(500, true));
}

/* A run of failed attempts to one target: the first is logged, the rest are counted, the count comes out when a
 * connection is made; the next failure starts a new run. */
static void test_streak_logs_once(void)
{
    satc_streak_t s;
    memset(&s, 0, sizeof s);
    uint32_t ended = 99;
    TEST_ASSERT_EQUAL_INT(SATC_LOG_FIRST, satc_streak_step(&s, "10.0.0.9:16622", true, &ended));
    TEST_ASSERT_EQUAL_UINT32(0, ended);
    for (int i = 0; i < 4; i++) {
        ended = 99;
        TEST_ASSERT_EQUAL_INT(SATC_LOG_QUIET, satc_streak_step(&s, "10.0.0.9:16622", true, &ended));
        TEST_ASSERT_EQUAL_UINT32(0, ended);
    }
    ended = 99;
    TEST_ASSERT_EQUAL_INT(SATC_LOG_NONE, satc_streak_step(&s, "10.0.0.9:16622", false, &ended));
    TEST_ASSERT_EQUAL_UINT32(5, ended);
    ended = 99;
    TEST_ASSERT_EQUAL_INT(SATC_LOG_NONE, satc_streak_step(&s, "10.0.0.9:16622", false, &ended));
    TEST_ASSERT_EQUAL_UINT32(0, ended); /* no run to end */
    ended = 99;
    TEST_ASSERT_EQUAL_INT(SATC_LOG_FIRST, satc_streak_step(&s, "10.0.0.9:16622", true, &ended));
    TEST_ASSERT_EQUAL_UINT32(0, ended);
}

/* Another target ends the run: its first failure is logged and the old run's count handed back. */
static void test_streak_new_target(void)
{
    satc_streak_t s;
    memset(&s, 0, sizeof s);
    uint32_t ended = 99;
    TEST_ASSERT_EQUAL_INT(SATC_LOG_FIRST, satc_streak_step(&s, "a:1", true, &ended));
    TEST_ASSERT_EQUAL_INT(SATC_LOG_QUIET, satc_streak_step(&s, "a:1", true, &ended));
    TEST_ASSERT_EQUAL_UINT32(0, ended);
    TEST_ASSERT_EQUAL_INT(SATC_LOG_FIRST, satc_streak_step(&s, "b:2", true, &ended));
    TEST_ASSERT_EQUAL_UINT32(2, ended);
    TEST_ASSERT_EQUAL_INT(SATC_LOG_QUIET, satc_streak_step(&s, "b:2", true, NULL)); /* NULL ended accepted */
    TEST_ASSERT_EQUAL_INT(SATC_LOG_NONE, satc_streak_step(&s, "c:3", false, &ended)); /* a success elsewhere ends it */
    TEST_ASSERT_EQUAL_UINT32(2, ended);

    /* a key longer than the struct holds is cut, and the cut key is what later attempts are compared with */
    char longkey[73];
    memset(longkey, 'h', 67);
    memcpy(longkey + 67, ":1662", 6); /* 72 characters */
    TEST_ASSERT_EQUAL_size_t(72, strlen(longkey));
    TEST_ASSERT_EQUAL_INT(SATC_LOG_FIRST, satc_streak_step(&s, longkey, true, &ended));
    TEST_ASSERT_EQUAL_UINT32(0, ended);
    TEST_ASSERT_EQUAL_size_t(sizeof s.key - 1, strlen(s.key));
    TEST_ASSERT_EQUAL_INT(SATC_LOG_QUIET, satc_streak_step(&s, longkey, true, &ended));
    TEST_ASSERT_EQUAL_INT(SATC_LOG_QUIET, satc_streak_step(&s, longkey, true, &ended));
    TEST_ASSERT_EQUAL_UINT32(0, ended);
    TEST_ASSERT_EQUAL_INT(SATC_LOG_NONE, satc_streak_step(&s, longkey, false, NULL));
    TEST_ASSERT_EQUAL_INT(SATC_LOG_FIRST, satc_streak_step(&s, "a:1", true, &ended));
    TEST_ASSERT_EQUAL_UINT32(0, ended); /* NULL took the long run's count; nothing left over */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_write_all_slow_reader_never_times_out);
    RUN_TEST(test_write_all_stalled_reader_times_out);
    RUN_TEST(test_write_all_trickle_times_out_on_a_line);
    RUN_TEST(test_pending_work_wakes_the_loop);
    RUN_TEST(test_idle_waits_for_the_fallback);
    RUN_TEST(test_not_ready_keeps_the_fallback);
    RUN_TEST(test_end_event);
    RUN_TEST(test_end_event_silent_cases);
    RUN_TEST(test_backoff_grows);
    RUN_TEST(test_backoff_resets_on_target_change);
    RUN_TEST(test_streak_logs_once);
    RUN_TEST(test_streak_new_target);
    return UNITY_END();
}
