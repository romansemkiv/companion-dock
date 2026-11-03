#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "fbsound.h"
#include "tonegen.h"
#include "tones.h"

void setUp(void) {}
void tearDown(void) {}

#define RATE 16000
#define SPMS (RATE / 1000) /* samples per ms */

/* ---- "freq:ms" steps ---- */

static int parse(const char *s, tone_t *t)
{
    const char *e = NULL;
    int n = tones_parse(s, t, &e);
    return n ? n : (e ? -1 : -2); /* -1: refused with a reason, -2: refused without one */
}

static void test_tones_parse(void)
{
    tone_t t[TONES_MAX];
    TEST_ASSERT_EQUAL_INT(3, parse("2000:80 0:40 3000:150", t));
    TEST_ASSERT_TRUE(t[0].hz == 2000 && t[0].ms == 80 && t[1].hz == 0 && t[1].ms == 40 && t[2].hz == 3000 && t[2].ms == 150);
    TEST_ASSERT_EQUAL_INT(1, parse("  4000:30  ", t));
    TEST_ASSERT_TRUE(t[0].hz == 4000 && t[0].ms == 30);
    TEST_ASSERT_EQUAL_INT(2, parse("2000:80,3000:100", t));
    TEST_ASSERT_EQUAL_INT(3000, t[1].hz);
    TEST_ASSERT_EQUAL_INT(2, parse("2000:80 , 3000:100", t));
    TEST_ASSERT_EQUAL_INT(3, parse("100:10 10000:2000 0:2000", t)); /* the limits themselves */
    TEST_ASSERT_EQUAL_INT(1, parse("0:500", t));                    /* a pause alone is a sound */

    static const char *const BAD[] = {"", "   ", "2000", "2000:", ":80", "2000:80x", "2000:80 abc", "-5:80", "2000:-80",
                                      "99:80", "10001:80", "2000:9", "2000:2001", "99999999999999999999:10",
                                      "2000:80::", "2000:80,,3000:80"};
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse(BAD[i], t), BAD[i]);
    TEST_ASSERT_EQUAL_INT(-1, parse(NULL, t));

    char buf[400] = "";
    for (int i = 0; i < 16; i++) strcat(buf, "1000:100 ");
    TEST_ASSERT_EQUAL_INT(16, parse(buf, t));
    strcat(buf, "1000:100");
    TEST_ASSERT_EQUAL_INT(-1, parse(buf, t)); /* 17 steps */
    TEST_ASSERT_EQUAL_INT(5, parse("1000:2000 1000:2000 1000:2000 1000:2000 1000:2000", t)); /* 10 s exactly */
    TEST_ASSERT_EQUAL_INT(-1, parse("1000:2000 1000:2000 1000:2000 1000:2000 1000:2000 0:10", t));
    TEST_ASSERT_EQUAL_INT(1, tones_parse("2000:80", t, NULL)); /* err may be NULL */
    TEST_ASSERT_EQUAL_INT(0, tones_parse("bad", t, NULL));
}

/* ---- the tone generator ---- */

static int crossings(const int16_t *b, int from, int to)
{
    int n = 0;
    for (int i = from + 1; i < to; i++) n += (b[i - 1] < 0) != (b[i] < 0);
    return n;
}

static int peak(const int16_t *b, int from, int to)
{
    int p = 0;
    for (int i = from; i < to; i++)
        if (abs(b[i]) > p) p = abs(b[i]);
    return p;
}

static void test_tonegen(void)
{
    tonegen_t g;
    static int16_t b[1600]; /* 100 ms at 16 kHz */
    tonegen_init(&g, RATE);
    TEST_ASSERT_TRUE(tonegen_idle(&g));
    tonegen_fill(&g, b, 1600);
    TEST_ASSERT_EQUAL_INT(0, peak(b, 0, 1600));
    tonegen_set(&g, 1000, 80);
    tonegen_fill(&g, b, 1600);
    const int c = crossings(b, 0, 1600);
    TEST_ASSERT_TRUE(c >= 196 && c <= 202); /* 1 kHz: 200 sign changes in 100 ms */
    const int p = peak(b, 0, 1600);
    TEST_ASSERT_TRUE(p <= tonegen_level(80) && p > tonegen_level(80) * 9 / 10);
    TEST_ASSERT_TRUE(abs(b[0]) < tonegen_level(80) / 10); /* a ramp in, not a click */
    TEST_ASSERT_FALSE(tonegen_idle(&g));
    /* a change of frequency or a stop: no step bigger than the sine itself makes (no click) */
    int16_t prev = b[1599];
    tonegen_set(&g, 3000, 80);
    tonegen_fill(&g, b, 1600);
    int jump = abs(b[0] - prev);
    for (int i = 1; i < 1600; i++)
        if (abs(b[i] - b[i - 1]) > jump) jump = abs(b[i] - b[i - 1]);
    TEST_ASSERT_TRUE(jump <= tonegen_level(80) * 6283 / 1000 * 3000 / 16000 + 64);
    const int c3 = crossings(b, 800, 1600);
    TEST_ASSERT_TRUE(c3 >= 296 && c3 <= 302); /* 3 kHz over the last 50 ms */
    tonegen_set(&g, 0, 80);
    tonegen_fill(&g, b, 1600);
    TEST_ASSERT_TRUE(abs(b[0]) <= tonegen_level(80));
    TEST_ASSERT_TRUE(b[1599] == 0 && tonegen_idle(&g)); /* ramped out: silent and idle */
    /* the level: 0 is silence, louder with the volume, never past full scale */
    TEST_ASSERT_TRUE(tonegen_level(0) == 0 && tonegen_level(100) <= 32767);
    int last = -1;
    for (int v = 0; v <= 100; v += 10) {
        TEST_ASSERT_TRUE(tonegen_level(v) > last);
        last = tonegen_level(v);
    }
    tonegen_set(&g, 2000, 0);
    tonegen_fill(&g, b, 1600);
    TEST_ASSERT_EQUAL_INT(0, b[1599]);
}

/* ---- the event sounds ---- */

static int total_ms(fb_event_t e)
{
    tone_t t[TONES_MAX];
    const int n = tones_parse(fb_sound(e)->tones, t, NULL);
    int ms = 0;
    for (int i = 0; i < n; i++) ms += t[i].ms;
    return ms;
}

/* the first step's frequency */
static int first_hz(fb_event_t e)
{
    tone_t t[TONES_MAX];
    TEST_ASSERT_TRUE(tones_parse(fb_sound(e)->tones, t, NULL) > 0);
    return t[0].hz;
}

static void test_sound_table(void)
{
    for (int e = 0; e < FB_EV_COUNT; e++) {
        const fb_sound_t *s = fb_sound((fb_event_t)e);
        TEST_ASSERT_NOT_NULL(s);
        tone_t t[TONES_MAX];
        const char *err = NULL;
        const int n = tones_parse(s->tones, t, &err);
        TEST_ASSERT_TRUE_MESSAGE(n > 0, err ? err : s->tones);
        TEST_ASSERT_TRUE(s->level > 0 && s->level <= 100);
        for (int i = 0; i < n; i++) /* 16 kHz: every tone well below 8 kHz */
            TEST_ASSERT_TRUE(t[i].hz <= 4000);
    }
    TEST_ASSERT_NULL(fb_sound(FB_EV_COUNT));
    /* the maintenance UI's: a finished hold, a locked button: short, under a second */
    TEST_ASSERT_TRUE(total_ms(FB_EV_CONFIRM) > 0 && total_ms(FB_EV_CONFIRM) < 400);
    TEST_ASSERT_TRUE(total_ms(FB_EV_LOCKED) > 0 && total_ms(FB_EV_LOCKED) < 400);
    /* the sound table: short chimes and blips, a 5-10 ms click, Find my box beeps for 3 s */
    TEST_ASSERT_TRUE(total_ms(FB_EV_KEY_CLICK) >= 5 && total_ms(FB_EV_KEY_CLICK) <= 10);
    TEST_ASSERT_TRUE(fb_sound(FB_EV_KEY_CLICK)->level < fb_sound(FB_EV_DECK_IN)->level); /* quiet */
    TEST_ASSERT_TRUE(total_ms(FB_EV_IDENTIFY) >= 2900 && total_ms(FB_EV_IDENTIFY) <= 3000);
    TEST_ASSERT_TRUE(total_ms(FB_EV_READY) <= 500 && total_ms(FB_EV_CONNECTED) <= 300);
    TEST_ASSERT_TRUE(total_ms(FB_EV_DECK_IN) <= 200 && total_ms(FB_EV_DECK_OUT) <= 200);
    TEST_ASSERT_TRUE(total_ms(FB_EV_PROBLEM) <= 600);
    /* ready rises (two notes), problem is low, deck in rises and deck out falls */
    tone_t t[TONES_MAX];
    int n = tones_parse(fb_sound(FB_EV_READY)->tones, t, NULL);
    TEST_ASSERT_TRUE(n >= 2 && t[n - 1].hz > t[0].hz);
    TEST_ASSERT_TRUE(first_hz(FB_EV_PROBLEM) < first_hz(FB_EV_READY) && first_hz(FB_EV_PROBLEM) <= 600);
    n = tones_parse(fb_sound(FB_EV_PROBLEM)->tones, t, NULL);
    int beeps = 0;
    for (int i = 0; i < n; i++) beeps += t[i].hz != 0;
    TEST_ASSERT_EQUAL_INT(2, beeps); /* a double beep */
    n = tones_parse(fb_sound(FB_EV_DECK_IN)->tones, t, NULL);
    TEST_ASSERT_TRUE(n >= 2 && t[n - 1].hz > t[0].hz);
    n = tones_parse(fb_sound(FB_EV_DECK_OUT)->tones, t, NULL);
    TEST_ASSERT_TRUE(n >= 2 && t[n - 1].hz < t[0].hz);
}

/* A new sound replaces the one playing; a key click never cuts another sound off (it is dropped),
 * and neither does a sound of a lower priority. */
static void test_sound_takes_over(void)
{
    for (int e = 0; e < FB_EV_COUNT; e++) TEST_ASSERT_TRUE(fb_sound_takes_over(false, FB_EV_COUNT, (fb_event_t)e));
    for (int e = 0; e < FB_EV_COUNT; e++)
        TEST_ASSERT_FALSE(fb_sound_takes_over(true, (fb_event_t)e, FB_EV_KEY_CLICK));
    for (int e = 0; e < FB_EV_COUNT; e++) TEST_ASSERT_TRUE(fb_sound_takes_over(true, (fb_event_t)e, FB_EV_IDENTIFY));
    TEST_ASSERT_TRUE(fb_sound_takes_over(true, FB_EV_KEY_CLICK, FB_EV_DECK_IN));
    TEST_ASSERT_TRUE(fb_sound_takes_over(true, FB_EV_READY, FB_EV_CONNECTED));
    TEST_ASSERT_TRUE(fb_sound_takes_over(true, FB_EV_DECK_IN, FB_EV_DECK_OUT));
    TEST_ASSERT_TRUE(fb_sound_takes_over(true, FB_EV_DECK_OUT, FB_EV_PROBLEM));
    /* the problem beep isn't cut off by the deck blips that follow a USB reset, nor ready by the
     * blips of decks found at start-up */
    TEST_ASSERT_FALSE(fb_sound_takes_over(true, FB_EV_PROBLEM, FB_EV_DECK_OUT));
    TEST_ASSERT_FALSE(fb_sound_takes_over(true, FB_EV_PROBLEM, FB_EV_DECK_IN));
    TEST_ASSERT_FALSE(fb_sound_takes_over(true, FB_EV_READY, FB_EV_DECK_IN));
    TEST_ASSERT_FALSE(fb_sound_takes_over(true, FB_EV_IDENTIFY, FB_EV_PROBLEM));
}

/* ---- the player (the speaker task's loop, without the codec) ---- */

static int16_t pcm[RATE * 4]; /* 4 s */

/* fills ms of samples in 10 ms blocks, as the speaker task does */
static void run(fb_player_t *p, int from_ms, int ms)
{
    for (int i = from_ms * SPMS; i < (from_ms + ms) * SPMS; i += RATE / 100) fb_player_fill(p, pcm + i, RATE / 100);
}

static void test_player_plays_steps_on_time(void)
{
    fb_player_t p;
    fb_player_init(&p, RATE);
    TEST_ASSERT_TRUE(fb_player_idle(&p));
    TEST_ASSERT_TRUE(fb_player_post(&p, FB_EV_DECK_IN, 100));
    fb_event_t ev = FB_EV_COUNT;
    TEST_ASSERT_TRUE(fb_player_playing(&p, &ev));
    TEST_ASSERT_EQUAL_INT(FB_EV_DECK_IN, ev);
    tone_t t[TONES_MAX];
    const int n = tones_parse(fb_sound(FB_EV_DECK_IN)->tones, t, NULL);
    const int total = total_ms(FB_EV_DECK_IN);
    run(&p, 0, total + 20);
    /* each step at its own frequency, starting exactly where the one before ends (allow the ramps) */
    int at = 0;
    for (int i = 0; i < n; i++) {
        const int from = (at + 4) * SPMS, to = (at + t[i].ms - 1) * SPMS;
        const int want = t[i].hz * 2 * (to - from) / RATE, got = crossings(pcm, from, to);
        TEST_ASSERT_TRUE_MESSAGE(got >= want - 3 && got <= want + 3, "a step at the wrong frequency or time");
        at += t[i].ms;
    }
    TEST_ASSERT_EQUAL_INT(0, peak(pcm, (total + 4) * SPMS, (total + 20) * SPMS)); /* then silence */
    TEST_ASSERT_FALSE(fb_player_playing(&p, NULL));
    TEST_ASSERT_TRUE(fb_player_idle(&p));
}

static void test_player_click_dropped_while_playing(void)
{
    fb_player_t p;
    fb_player_init(&p, RATE);
    TEST_ASSERT_TRUE(fb_player_post(&p, FB_EV_READY, 50));
    run(&p, 0, 30);
    TEST_ASSERT_FALSE(fb_player_post(&p, FB_EV_KEY_CLICK, 50));
    fb_event_t ev = FB_EV_COUNT;
    TEST_ASSERT_TRUE(fb_player_playing(&p, &ev));
    TEST_ASSERT_EQUAL_INT(FB_EV_READY, ev);
    run(&p, 30, total_ms(FB_EV_READY) + 20);
    TEST_ASSERT_TRUE(fb_player_idle(&p));
    TEST_ASSERT_TRUE(fb_player_post(&p, FB_EV_KEY_CLICK, 50)); /* nothing playing: it plays */
    run(&p, 0, 10);
    TEST_ASSERT_TRUE(peak(pcm, 0, 10 * SPMS) > 0);
}

static void test_player_replaces(void)
{
    fb_player_t p;
    fb_player_init(&p, RATE);
    fb_event_t ev = FB_EV_COUNT;
    TEST_ASSERT_TRUE(fb_player_post(&p, FB_EV_DECK_IN, 50));
    run(&p, 0, 20);
    TEST_ASSERT_TRUE(fb_player_post(&p, FB_EV_PROBLEM, 50)); /* replaces the blip, from its first step */
    TEST_ASSERT_TRUE(fb_player_playing(&p, &ev));
    TEST_ASSERT_EQUAL_INT(FB_EV_PROBLEM, ev);
    TEST_ASSERT_FALSE(fb_player_post(&p, FB_EV_DECK_OUT, 50)); /* lower: dropped */
    run(&p, 0, total_ms(FB_EV_PROBLEM) - 10);
    TEST_ASSERT_TRUE(fb_player_playing(&p, &ev));
    TEST_ASSERT_EQUAL_INT(FB_EV_PROBLEM, ev); /* still the whole problem beep */
    run(&p, 0, 40);
    TEST_ASSERT_FALSE(fb_player_playing(&p, NULL));
}

static void test_player_volume(void)
{
    fb_player_t p;
    fb_player_init(&p, RATE);
    fb_player_post(&p, FB_EV_CONNECTED, 0); /* muted */
    run(&p, 0, total_ms(FB_EV_CONNECTED) + 10);
    TEST_ASSERT_EQUAL_INT(0, peak(pcm, 0, (total_ms(FB_EV_CONNECTED) + 10) * SPMS));
    fb_player_post(&p, FB_EV_CONNECTED, 100);
    run(&p, 0, 60);
    const int loud = peak(pcm, 10 * SPMS, 60 * SPMS);
    run(&p, 60, total_ms(FB_EV_CONNECTED));
    fb_player_post(&p, FB_EV_CONNECTED, 50);
    run(&p, 0, 60);
    const int half = peak(pcm, 10 * SPMS, 60 * SPMS);
    TEST_ASSERT_TRUE(loud > half && half > 0);
    TEST_ASSERT_TRUE(loud <= tonegen_level(fb_sound(FB_EV_CONNECTED)->level));
    run(&p, 60, total_ms(FB_EV_CONNECTED));
    fb_player_post(&p, FB_EV_KEY_CLICK, 100); /* the click is quiet */
    run(&p, 0, 10);
    TEST_ASSERT_TRUE(peak(pcm, 0, 10 * SPMS) < loud / 2);
}

/* ---- the codec failing after the start ----
 * A failed open or write is logged once per streak; the codec is tried again only when a sound comes
 * and the back-off has run out: FB_RETRY_FIRST_US, doubling up to FB_RETRY_MAX_US. */

#define S_US 1000000LL

static void test_retry_backoff(void)
{
    fb_retry_t r;
    fb_retry_init(&r);
    TEST_ASSERT_TRUE(fb_retry_due(&r, 0)); /* a working codec: always */
    TEST_ASSERT_FALSE(fb_retry_ok(&r));    /* no streak to end */
    const int64_t t0 = 100 * S_US;
    TEST_ASSERT_TRUE(fb_retry_failed(&r, t0)); /* the streak begins: log it */
    TEST_ASSERT_FALSE(fb_retry_due(&r, t0));
    TEST_ASSERT_FALSE(fb_retry_due(&r, t0 + FB_RETRY_FIRST_US - 1));
    TEST_ASSERT_TRUE(fb_retry_due(&r, t0 + FB_RETRY_FIRST_US));
    /* each further failure: quiet, twice the wait, up to the cap */
    int64_t now = t0 + FB_RETRY_FIRST_US, wait = FB_RETRY_FIRST_US;
    for (int i = 0; i < 12; i++) {
        TEST_ASSERT_FALSE(fb_retry_failed(&r, now));
        wait = wait * 2 > FB_RETRY_MAX_US ? FB_RETRY_MAX_US : wait * 2;
        TEST_ASSERT_FALSE(fb_retry_due(&r, now + wait - 1));
        TEST_ASSERT_TRUE(fb_retry_due(&r, now + wait));
        now += wait;
    }
    TEST_ASSERT_EQUAL_INT64(FB_RETRY_MAX_US, wait);
    TEST_ASSERT_TRUE(fb_retry_ok(&r));  /* it answers again: log the recovery, once */
    TEST_ASSERT_FALSE(fb_retry_ok(&r));
    TEST_ASSERT_TRUE(fb_retry_due(&r, now));
    TEST_ASSERT_TRUE(fb_retry_failed(&r, now)); /* a new streak starts from the first wait again */
    TEST_ASSERT_FALSE(fb_retry_due(&r, now + FB_RETRY_FIRST_US - 1));
    TEST_ASSERT_TRUE(fb_retry_due(&r, now + FB_RETRY_FIRST_US));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tones_parse);
    RUN_TEST(test_tonegen);
    RUN_TEST(test_sound_table);
    RUN_TEST(test_sound_takes_over);
    RUN_TEST(test_player_plays_steps_on_time);
    RUN_TEST(test_player_click_dropped_while_playing);
    RUN_TEST(test_player_replaces);
    RUN_TEST(test_player_volume);
    RUN_TEST(test_retry_backoff);
    return UNITY_END();
}
