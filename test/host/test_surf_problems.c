/* The problem list behind surfaces_problem_list (surfaces/problems.c) and the quarantine-text match
 * it relies on (usbdeck/problem.h). */
#include <stdio.h>
#include <string.h>
#include "problems.h"
#include "unity.h"
#include "usbdeck/problem.h"

void setUp(void) {}
void tearDown(void) {}

/* A quarantine text as usbdeck words it (usbdeck_policy.c upol_quarantine_msg). */
static const char *qtext(char *buf, size_t cap, const char *serial, int min)
{
    snprintf(buf, cap, USBDECK_QUARANTINE_PREFIX "images off, retrying in %d min", serial, min);
    return buf;
}

static int find(const surf_problems_t *p, const char *text)
{
    for (int i = 0; i < p->n; i++)
        if (strcmp(p->text[i], text) == 0) return i;
    return -1;
}

/* ---- usbdeck_quarantine_key_len ---- */

static void test_key_len_of_quarantine_texts(void)
{
    char b[USBDECK_PROBLEM_MAX];
    TEST_ASSERT_EQUAL(strlen("Stream Deck AB1 keeps stalling USB: "), usbdeck_quarantine_key_len(qtext(b, sizeof b, "AB1", 10)));
    TEST_ASSERT_EQUAL(strlen("Stream Deck AB12 keeps stalling USB: "), usbdeck_quarantine_key_len(qtext(b, sizeof b, "AB12", 7)));
    TEST_ASSERT_EQUAL(strlen("Stream Deck unknown-5 keeps stalling USB: "),
                      usbdeck_quarantine_key_len("Stream Deck unknown-5 keeps stalling USB: off, retrying in 1 min"));
}

static void test_key_len_other_texts_are_not_quarantine(void)
{
    TEST_ASSERT_EQUAL(0, usbdeck_quarantine_key_len(NULL));
    TEST_ASSERT_EQUAL(0, usbdeck_quarantine_key_len(""));
    TEST_ASSERT_EQUAL(0, usbdeck_quarantine_key_len("Stream Deck AB1 stopped taking data — resetting the USB port"));
    TEST_ASSERT_EQUAL(0, usbdeck_quarantine_key_len("Stream Deck AB1 stopped responding — unplug and replug it if it doesn't recover"));
    TEST_ASSERT_EQUAL(0, usbdeck_quarantine_key_len("Stream Deck on USB address 3: input stopped"));
    TEST_ASSERT_EQUAL(0, usbdeck_quarantine_key_len("Companion refused streamdeck:AB1: Stream Deck AB1 keeps stalling USB: "));
    TEST_ASSERT_EQUAL(0, usbdeck_quarantine_key_len("Stream Deck  keeps stalling USB: images off")); /* no serial */
    TEST_ASSERT_EQUAL(0, usbdeck_quarantine_key_len("Stream Deck AB1 keeps stalling USB")); /* cut short */
}

/* ---- the list ---- */

static void test_newest_first_no_duplicates_bounded(void)
{
    surf_problems_t p;
    memset(&p, 0, sizeof p);
    surf_problems_add(&p, NULL);
    surf_problems_add(&p, "");
    TEST_ASSERT_EQUAL(0, p.n);
    surf_problems_add(&p, "a");
    surf_problems_add(&p, "b");
    surf_problems_add(&p, "a"); /* again: moves to the front, once */
    TEST_ASSERT_EQUAL(2, p.n);
    TEST_ASSERT_EQUAL_STRING("a", p.text[0]);
    TEST_ASSERT_EQUAL_STRING("b", p.text[1]);
    char t[8];
    for (int i = 0; i < SURF_PROBLEMS + 3; i++) {
        snprintf(t, sizeof t, "p%d", i);
        surf_problems_add(&p, t);
    }
    TEST_ASSERT_EQUAL(SURF_PROBLEMS, p.n);
    TEST_ASSERT_EQUAL_STRING("p10", p.text[0]);
    TEST_ASSERT_EQUAL_STRING("p3", p.text[SURF_PROBLEMS - 1]); /* the oldest went */
    char longer[200];
    memset(longer, 'x', sizeof longer - 1);
    longer[sizeof longer - 1] = 0;
    surf_problems_add(&p, longer);
    TEST_ASSERT_EQUAL(SURF_PROBLEM_LEN - 1, strlen(p.text[0])); /* cut, terminated */
}

/* one quarantine entry per deck. A newer text for the same deck (the minutes changed) replaces
 * it; other decks' texts, including a serial that extends this one, and other problems stay. */
static void test_quarantine_text_replaces_the_decks_previous_one(void)
{
    surf_problems_t p;
    memset(&p, 0, sizeof p);
    char a10[USBDECK_PROBLEM_MAX], a7[USBDECK_PROBLEM_MAX], b[USBDECK_PROBLEM_MAX];
    surf_problems_add(&p, "Companion refused streamdeck:AB1: nope");
    surf_problems_add(&p, qtext(a10, sizeof a10, "AB1", 10));
    surf_problems_add(&p, qtext(b, sizeof b, "AB12", 10));
    surf_problems_add(&p, "Stream Deck AB1 stopped taking data — resetting the USB port");
    surf_problems_add(&p, qtext(a7, sizeof a7, "AB1", 7));
    TEST_ASSERT_EQUAL(4, p.n);
    TEST_ASSERT_EQUAL(0, find(&p, a7));
    TEST_ASSERT_EQUAL(-1, find(&p, a10));
    TEST_ASSERT_EQUAL(1, find(&p, "Stream Deck AB1 stopped taking data — resetting the USB port"));
    TEST_ASSERT_EQUAL(2, find(&p, b));
    TEST_ASSERT_EQUAL(3, find(&p, "Companion refused streamdeck:AB1: nope"));
}

/* A quarantine whose minutes change at every re-attach doesn't push unrelated problems out. */
static void test_changing_minutes_dont_flood_the_list(void)
{
    surf_problems_t p;
    memset(&p, 0, sizeof p);
    char t[32], q[USBDECK_PROBLEM_MAX];
    for (int i = 0; i < SURF_PROBLEMS - 1; i++) {
        snprintf(t, sizeof t, "unrelated %d", i);
        surf_problems_add(&p, t);
    }
    for (int min = 60; min >= 1; min--) surf_problems_add(&p, qtext(q, sizeof q, "AB1", min));
    TEST_ASSERT_EQUAL(SURF_PROBLEMS, p.n);
    TEST_ASSERT_EQUAL_STRING(qtext(q, sizeof q, "AB1", 1), p.text[0]);
    for (int i = 0; i < SURF_PROBLEMS - 1; i++) {
        snprintf(t, sizeof t, "unrelated %d", i);
        TEST_ASSERT_TRUE(find(&p, t) > 0);
    }
}

/* At every end of a quarantine (on_restored, on_detach, on_attach) the deck's text goes; nothing
 * else does, and the rest keep their order. */
static void test_drop_quarantine_by_serial(void)
{
    surf_problems_t p;
    memset(&p, 0, sizeof p);
    char a[USBDECK_PROBLEM_MAX], b[USBDECK_PROBLEM_MAX];
    surf_problems_add(&p, "first");
    surf_problems_add(&p, qtext(a, sizeof a, "AB1", 10));
    surf_problems_add(&p, "Companion refused streamdeck:AB1: nope");
    surf_problems_add(&p, qtext(b, sizeof b, "AB12", 20));
    surf_problems_add(&p, "Stream Deck AB1 stopped taking data — resetting the USB port");
    TEST_ASSERT_EQUAL(0, surf_problems_drop_quarantine(&p, "AB"));  /* a prefix of both serials */
    TEST_ASSERT_EQUAL(0, surf_problems_drop_quarantine(&p, ""));
    TEST_ASSERT_EQUAL(0, surf_problems_drop_quarantine(&p, NULL));
    TEST_ASSERT_EQUAL(0, surf_problems_drop_quarantine(&p, "AB1 x")); /* not a serial */
    TEST_ASSERT_EQUAL(1, surf_problems_drop_quarantine(&p, "AB1"));
    TEST_ASSERT_EQUAL(4, p.n);
    TEST_ASSERT_EQUAL_STRING("Stream Deck AB1 stopped taking data — resetting the USB port", p.text[0]);
    TEST_ASSERT_EQUAL_STRING(b, p.text[1]);
    TEST_ASSERT_EQUAL_STRING("Companion refused streamdeck:AB1: nope", p.text[2]);
    TEST_ASSERT_EQUAL_STRING("first", p.text[3]);
    TEST_ASSERT_EQUAL(0, surf_problems_drop_quarantine(&p, "AB1")); /* already gone */
    TEST_ASSERT_EQUAL(1, surf_problems_drop_quarantine(&p, "AB12"));
    TEST_ASSERT_EQUAL(3, p.n);
}

/* a deck over the deck limit, the limit named: 2, or 4 unlocked; it is no quarantine text. */
static void test_too_many_names_the_limit(void)
{
    char m[SURF_PROBLEM_LEN];
    surf_problem_too_many(m, sizeof m, 2, "CL12K1A00042");
    TEST_ASSERT_EQUAL_STRING("Too many Stream Decks (max 2): CL12K1A00042 ignored", m);
    surf_problem_too_many(m, sizeof m, 4, "AB1CD2345678EF");
    TEST_ASSERT_EQUAL_STRING("Too many Stream Decks (max 4): AB1CD2345678EF ignored", m);
    surf_problems_t p = {0};
    surf_problems_add(&p, m);
    TEST_ASSERT_EQUAL(0, surf_problems_drop_quarantine(&p, "AB1CD2345678EF"));
    TEST_ASSERT_EQUAL(1, p.n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_len_of_quarantine_texts);
    RUN_TEST(test_key_len_other_texts_are_not_quarantine);
    RUN_TEST(test_newest_first_no_duplicates_bounded);
    RUN_TEST(test_quarantine_text_replaces_the_decks_previous_one);
    RUN_TEST(test_changing_minutes_dont_flood_the_list);
    RUN_TEST(test_drop_quarantine_by_serial);
    RUN_TEST(test_too_many_names_the_limit);
    return UNITY_END();
}
