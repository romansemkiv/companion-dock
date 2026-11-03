/* The dock input gate (components/elgdock/ingate.c): the hold is box-wide and a deck assigned during
 * it starts held; turning it on releases what Companion saw pressed (gen1 keys, gen2 keys, encoder
 * pushes); nothing passes while on; turning it off masks what is still held until it is let go. */
#include <string.h>
#include "unity.h"
#include "ingate.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t out[INGATE_MAX];

/* gen2 key report: 01 00 n 00 + n key states */
static size_t gen2_keys(uint8_t *r, const uint8_t *st, int n)
{
    memset(r, 0, INGATE_MAX);
    r[0] = 0x01;
    r[1] = 0x00;
    r[2] = (uint8_t)n;
    memcpy(r + 4, st, (size_t)n);
    return (size_t)(4 + n);
}

/* gen2 encoder report: 01 03 05 00 type + 4 states */
static size_t gen2_enc(uint8_t *r, uint8_t type, const uint8_t st[4])
{
    memset(r, 0, INGATE_MAX);
    r[0] = 0x01;
    r[1] = 0x03;
    r[2] = 0x05;
    r[4] = type;
    memcpy(r + 5, st, 4);
    return 9;
}

static bool pass(ingate_t *g, const uint8_t *r, size_t n) { return ingate_report(g, r, n, out); }

static void test_passes_untouched_without_hold(void)
{
    ingate_t g;
    ingate_init(&g, true, true, false);
    uint8_t r[INGATE_MAX];
    const uint8_t k[32] = {[3] = 1};
    const size_t n = gen2_keys(r, k, 32);
    TEST_ASSERT_TRUE(pass(&g, r, n));
    TEST_ASSERT_EQUAL_MEMORY(r, out, n);
}

/* On: Companion gets the key report with every key released; then nothing passes; off with nothing
 * held: everything passes again. */
static void test_hold_releases_then_keeps(void)
{
    ingate_t g;
    ingate_init(&g, true, true, false);
    uint8_t r[INGATE_MAX], rel[2][INGATE_MAX], rl[2];
    const uint8_t k[32] = {[5] = 1, [20] = 1};
    const size_t n = gen2_keys(r, k, 32);
    TEST_ASSERT_TRUE(pass(&g, r, n));
    TEST_ASSERT_EQUAL(1, ingate_hold(&g, true, rel, rl));
    TEST_ASSERT_EQUAL(n, rl[0]);
    TEST_ASSERT_EQUAL_MEMORY(r, rel[0], 4); /* the header kept */
    for (size_t i = 4; i < n; i++) TEST_ASSERT_EQUAL_UINT8(0, rel[0][i]);
    TEST_ASSERT_EQUAL(0, ingate_hold(&g, true, rel, rl)); /* again: nothing more */
    TEST_ASSERT_FALSE(pass(&g, r, n));
    const uint8_t none[32] = {0};
    const size_t m = gen2_keys(r, none, 32);
    TEST_ASSERT_FALSE(pass(&g, r, m));
    uint8_t turn[INGATE_MAX];
    const uint8_t d[4] = {1, 0, 0, 0};
    const size_t tn = gen2_enc(turn, 1, d); /* an encoder turn: kept too */
    TEST_ASSERT_FALSE(pass(&g, turn, tn));
    TEST_ASSERT_EQUAL(0, ingate_hold(&g, false, rel, rl));
    TEST_ASSERT_TRUE(pass(&g, r, m));
    TEST_ASSERT_TRUE(pass(&g, turn, tn));
}

/* Nothing pressed when it opens: nothing to send. */
static void test_hold_nothing_pressed(void)
{
    ingate_t g;
    ingate_init(&g, true, true, false);
    uint8_t r[INGATE_MAX], rel[2][INGATE_MAX], rl[2];
    TEST_ASSERT_EQUAL(0, ingate_hold(&g, true, rel, rl)); /* no report seen */
    TEST_ASSERT_EQUAL(0, ingate_hold(&g, false, rel, rl));
    const uint8_t none[32] = {0};
    TEST_ASSERT_TRUE(pass(&g, r, gen2_keys(r, none, 32)));
    TEST_ASSERT_EQUAL(0, ingate_hold(&g, true, rel, rl));
}

/* gen1 (01 + keys) and the encoder pushes (01 03 05 00 00 + states): both released. */
static void test_gen1_and_encoder_pushes(void)
{
    ingate_t g;
    ingate_init(&g, false, true, false);
    uint8_t r[INGATE_MAX] = {0x01, 0, 0, 1, 0, 0, 0}, rel[2][INGATE_MAX], rl[2];
    TEST_ASSERT_TRUE(pass(&g, r, 16));
    TEST_ASSERT_EQUAL(1, ingate_hold(&g, true, rel, rl));
    TEST_ASSERT_EQUAL(16, rl[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, rel[0][0]);
    for (int i = 1; i < 16; i++) TEST_ASSERT_EQUAL_UINT8(0, rel[0][i]);

    ingate_init(&g, true, true, false);
    const uint8_t k[8] = {[2] = 1};
    const uint8_t push[4] = {0, 1, 0, 0};
    uint8_t kr[INGATE_MAX], er[INGATE_MAX];
    const size_t kn = gen2_keys(kr, k, 8), en = gen2_enc(er, 0, push);
    TEST_ASSERT_TRUE(pass(&g, kr, kn));
    TEST_ASSERT_TRUE(pass(&g, er, en));
    TEST_ASSERT_EQUAL(2, ingate_hold(&g, true, rel, rl));
    int enc_at = rel[0][1] == 0x03 ? 0 : 1;
    TEST_ASSERT_EQUAL(en, rl[enc_at]);
    TEST_ASSERT_EQUAL_MEMORY(er, rel[enc_at], 5);
    for (size_t i = 5; i < en; i++) TEST_ASSERT_EQUAL_UINT8(0, rel[enc_at][i]);
    for (size_t i = 4; i < kn; i++) TEST_ASSERT_EQUAL_UINT8(0, rel[1 - enc_at][i]);
}

/* Off while a key is still held: it reaches Companion as released until the deck lets it go; other keys
 * pass; a new press of it after that passes. */
static void test_held_through_masked_until_released(void)
{
    ingate_t g;
    ingate_init(&g, true, true, false);
    uint8_t r[INGATE_MAX], rel[2][INGATE_MAX], rl[2];
    ingate_hold(&g, true, rel, rl);
    const uint8_t held[32] = {[7] = 1};
    TEST_ASSERT_FALSE(pass(&g, r, gen2_keys(r, held, 32))); /* pressed during the session */
    ingate_hold(&g, false, rel, rl);
    const uint8_t also[32] = {[7] = 1, [9] = 1}; /* still held, another pressed */
    size_t n = gen2_keys(r, also, 32);
    TEST_ASSERT_TRUE(pass(&g, r, n));
    TEST_ASSERT_EQUAL_UINT8(0, out[4 + 7]);
    TEST_ASSERT_EQUAL_UINT8(1, out[4 + 9]);
    const uint8_t let_go[32] = {[9] = 1};
    n = gen2_keys(r, let_go, 32);
    TEST_ASSERT_TRUE(pass(&g, r, n));
    TEST_ASSERT_EQUAL_UINT8(0, out[4 + 7]);
    TEST_ASSERT_EQUAL_UINT8(1, out[4 + 9]);
    const uint8_t again[32] = {[7] = 1, [9] = 1}; /* pressed anew: Companion's */
    n = gen2_keys(r, again, 32);
    TEST_ASSERT_TRUE(pass(&g, r, n));
    TEST_ASSERT_EQUAL_UINT8(1, out[4 + 7]);
    /* an encoder pushed through the session: the same */
    ingate_hold(&g, true, rel, rl);
    const uint8_t push[4] = {1, 0, 0, 0}, none[4] = {0};
    TEST_ASSERT_FALSE(pass(&g, r, gen2_enc(r, 0, push)));
    ingate_hold(&g, false, rel, rl);
    TEST_ASSERT_TRUE(pass(&g, r, gen2_enc(r, 0, push)));
    TEST_ASSERT_EQUAL_UINT8(0, out[5]);
    TEST_ASSERT_TRUE(pass(&g, r, gen2_enc(r, 0, none)));
    TEST_ASSERT_TRUE(pass(&g, r, gen2_enc(r, 0, push)));
    TEST_ASSERT_EQUAL_UINT8(1, out[5]);
}

/* A deck assigned while the box-wide hold is on starts held (a replug, a plug-in during on demand); one
 * without a screen (the Pedal) is never held. */
static void test_assigned_during_hold_and_pedal(void)
{
    ingate_t g;
    uint8_t r[INGATE_MAX], rel[2][INGATE_MAX], rl[2];
    const uint8_t k[32] = {[1] = 1};
    const size_t n = gen2_keys(r, k, 32);
    ingate_init(&g, true, true, true);
    TEST_ASSERT_FALSE(pass(&g, r, n));
    TEST_ASSERT_EQUAL(0, ingate_hold(&g, false, rel, rl));
    TEST_ASSERT_TRUE(pass(&g, r, n));
    TEST_ASSERT_EQUAL_UINT8(0, out[4 + 1]); /* held when it opened: masked */
    ingate_init(&g, true, false, true); /* the Pedal */
    TEST_ASSERT_TRUE(pass(&g, r, n));
    TEST_ASSERT_EQUAL(0, ingate_hold(&g, true, rel, rl));
    TEST_ASSERT_TRUE(pass(&g, r, n));
    TEST_ASSERT_EQUAL_UINT8(1, out[4 + 1]);
}

/* the screen on demand on one deck: only the chosen deck (by its serial) is held; the others, and a
 * deck plugged in while it is open, go to Companion untouched; the chosen deck reassigned to a dock
 * (replugged, or a dock handed over) starts held again; closing releases the chosen one only. */
static void test_hold_one_deck(void)
{
    const char *one[] = {"AL12K1A00001"}, *two[] = {"CL34K1B00002", "AL12K1A00001"}, *blank[] = {""};
    TEST_ASSERT_TRUE(ingate_held("AL12K1A00001", one, 1));
    TEST_ASSERT_FALSE(ingate_held("CL34K1B00002", one, 1));
    TEST_ASSERT_FALSE(ingate_held("AL12K1A00001", one, 0));
    TEST_ASSERT_FALSE(ingate_held("AL12K1A00001", NULL, 0));
    TEST_ASSERT_FALSE(ingate_held("", blank, 1));
    /* a reopen on another deck while the first still hands back: both held */
    TEST_ASSERT_TRUE(ingate_held("AL12K1A00001", two, 2));
    TEST_ASSERT_TRUE(ingate_held("CL34K1B00002", two, 2));
    TEST_ASSERT_FALSE(ingate_held("DL56K1C00003", two, 2));
    ingate_t a, b, c;
    uint8_t r[INGATE_MAX], rel[2][INGATE_MAX], rl[2];
    const uint8_t k[32] = {[3] = 1};
    const size_t n = gen2_keys(r, k, 32);
    const char *const *chosen = one;
    ingate_init(&a, true, true, ingate_held("AL12K1A00001", chosen, 1)); /* the chosen deck */
    ingate_init(&b, true, true, ingate_held("CL34K1B00002", chosen, 1)); /* another, Companion's */
    TEST_ASSERT_FALSE(pass(&a, r, n));
    TEST_ASSERT_TRUE(pass(&b, r, n));
    TEST_ASSERT_EQUAL_UINT8(1, out[4 + 3]);
    ingate_init(&c, true, true, ingate_held("DL56K1C00003", chosen, 1)); /* plugged in while open: joins nothing */
    TEST_ASSERT_TRUE(pass(&c, r, n));
    ingate_init(&a, true, true, ingate_held("AL12K1A00001", chosen, 1)); /* the chosen one reassigned: held */
    TEST_ASSERT_FALSE(pass(&a, r, n));
    /* reopened on b while a still hands back: b held too, a stays held */
    TEST_ASSERT_EQUAL(1, ingate_hold(&b, ingate_held("CL34K1B00002", two, 2), rel, rl)); /* b's key released */
    TEST_ASSERT_EQUAL(0, ingate_hold(&a, ingate_held("AL12K1A00001", two, 2), rel, rl));
    TEST_ASSERT_FALSE(pass(&a, r, n));
    TEST_ASSERT_FALSE(pass(&b, r, n));
    /* a's hand-back over: a released (its held key masked), b stays held */
    TEST_ASSERT_EQUAL(0, ingate_hold(&a, ingate_held("AL12K1A00001", two, 1), rel, rl));
    TEST_ASSERT_TRUE(pass(&a, r, n));
    TEST_ASSERT_EQUAL_UINT8(0, out[4 + 3]);
    TEST_ASSERT_FALSE(pass(&b, r, n));
    /* closing: each dock follows its own deck; only the held one changes */
    TEST_ASSERT_EQUAL(0, ingate_hold(&b, ingate_held("CL34K1B00002", NULL, 0), rel, rl));
    TEST_ASSERT_TRUE(pass(&b, r, n));
    TEST_ASSERT_EQUAL_UINT8(0, out[4 + 3]); /* held through it: masked until let go */
    TEST_ASSERT_EQUAL(0, ingate_hold(&c, ingate_held("DL56K1C00003", NULL, 0), rel, rl));
    TEST_ASSERT_TRUE(pass(&c, r, n));
    TEST_ASSERT_EQUAL_UINT8(1, out[4 + 3]); /* never held: nothing masked */
    ingate_init(&a, true, false, ingate_held("AL12K1A00001", chosen, 1)); /* the Pedal never, even by serial */
    TEST_ASSERT_TRUE(pass(&a, r, n));
}

/* Odd reports: another report ID, longer than kept (cut, as before): never kept as state; passed only
 * without a hold. */
static void test_odd_reports(void)
{
    ingate_t g;
    ingate_init(&g, true, true, false);
    uint8_t r[INGATE_MAX + 8] = {0x02, 1, 2, 3}, rel[2][INGATE_MAX], rl[2];
    TEST_ASSERT_TRUE(ingate_report(&g, r, 4, out));
    TEST_ASSERT_TRUE(ingate_report(&g, r, sizeof r, out)); /* longer than kept: its first INGATE_MAX bytes */
    TEST_ASSERT_EQUAL_MEMORY(r, out, INGATE_MAX);
    TEST_ASSERT_EQUAL(0, ingate_hold(&g, true, rel, rl));
    TEST_ASSERT_FALSE(ingate_report(&g, r, 4, out));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_passes_untouched_without_hold);
    RUN_TEST(test_hold_releases_then_keeps);
    RUN_TEST(test_hold_nothing_pressed);
    RUN_TEST(test_gen1_and_encoder_pushes);
    RUN_TEST(test_held_through_masked_until_released);
    RUN_TEST(test_assigned_during_hold_and_pedal);
    RUN_TEST(test_odd_reports);
    RUN_TEST(test_hold_one_deck);
    return UNITY_END();
}
