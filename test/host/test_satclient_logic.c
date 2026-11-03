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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_write_all_slow_reader_never_times_out);
    RUN_TEST(test_write_all_stalled_reader_times_out);
    RUN_TEST(test_write_all_trickle_times_out_on_a_line);
    RUN_TEST(test_pending_work_wakes_the_loop);
    RUN_TEST(test_idle_waits_for_the_fallback);
    RUN_TEST(test_not_ready_keeps_the_fallback);
    return UNITY_END();
}
