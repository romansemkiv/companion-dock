#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "fbstatus.h"

void setUp(void) {}
void tearDown(void) {}

#define S_US 1000000LL
#define MS_US 1000LL

/* ---- which status wins ---- */

/* Everything fine: Companion drives the decks. */
static fb_led_in_t all_good(void)
{
    return (fb_led_in_t){.booted = true, .net_up = true, .companion = true};
}

/* The light's table, lowest priority first: each row's condition, added on top of every row below it,
 * wins over all of them. */
static void test_led_priority_table(void)
{
    fb_led_in_t in = all_good();
    TEST_ASSERT_EQUAL_INT(FB_LED_CONNECTED, fb_led_state(&in));
    in.companion = false;
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_COMPANION, fb_led_state(&in));
    in.setup_ap = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_SETUP_AP, fb_led_state(&in));
    in.net_up = false;
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_NETWORK, fb_led_state(&in));
    in.deck_problem = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_DECK_PROBLEM, fb_led_state(&in));
    in.update = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_UPDATE, fb_led_state(&in));
    in.identify = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_IDENTIFY, fb_led_state(&in));
}

static void test_led_priority_pairs(void)
{
    fb_led_in_t in = all_good();
    in.setup_ap = true; /* the setup hotspot outranks Companion connected */
    TEST_ASSERT_EQUAL_INT(FB_LED_SETUP_AP, fb_led_state(&in));
    in = all_good();
    in.net_up = false; /* without a network Companion can't be there: no network wins */
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_NETWORK, fb_led_state(&in));
    in = all_good();
    in.deck_problem = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_DECK_PROBLEM, fb_led_state(&in));
    in = all_good();
    in.update = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_UPDATE, fb_led_state(&in));
    in = all_good();
    in.identify = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_IDENTIFY, fb_led_state(&in));
}

/* Starting up: before the boot finished, and after it while the network has never come up yet (for
 * FB_START_GRACE_US): a box mid-DHCP isn't "no network". Anything higher still shows. */
static void test_led_starting(void)
{
    fb_led_in_t in = {0};
    TEST_ASSERT_EQUAL_INT(FB_LED_STARTING, fb_led_state(&in));
    in.booted = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_NETWORK, fb_led_state(&in));
    in.start_grace = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_STARTING, fb_led_state(&in));
    in.net_up = true; /* up: the grace no longer matters */
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_COMPANION, fb_led_state(&in));
    in.net_up = false;
    in.deck_problem = true;
    TEST_ASSERT_EQUAL_INT(FB_LED_DECK_PROBLEM, fb_led_state(&in));
    in = (fb_led_in_t){.update = true};
    TEST_ASSERT_EQUAL_INT(FB_LED_UPDATE, fb_led_state(&in));
    in = (fb_led_in_t){.identify = true};
    TEST_ASSERT_EQUAL_INT(FB_LED_IDENTIFY, fb_led_state(&in));
}

/* ---- which colour, on or off now ---- */

static bool black(fb_rgb_t c) { return !c.r && !c.g && !c.b; }
static bool same(fb_rgb_t a, fb_rgb_t b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

/* lit at the first time of each half period, black at the second (half = the blink's on time) */
static void assert_blink(fb_led_t s, int64_t half_us, const char *what)
{
    for (int64_t t = 0; t < 4 * half_us; t += half_us) {
        const fb_rgb_t a = fb_led_color(s, t), z = fb_led_color(s, t + half_us - 1);
        TEST_ASSERT_TRUE_MESSAGE(same(a, z), what); /* steady within a half */
        if ((t / half_us) % 2 == 0) TEST_ASSERT_FALSE_MESSAGE(black(a), what);
        else TEST_ASSERT_TRUE_MESSAGE(black(a), what);
    }
}

static void test_led_colors(void)
{
    /* steady and dim */
    for (int64_t t = 0; t < 3 * S_US; t += 70 * MS_US) {
        const fb_rgb_t g = fb_led_color(FB_LED_CONNECTED, t), w = fb_led_color(FB_LED_STARTING, t);
        TEST_ASSERT_TRUE(g.g > 0 && g.g < 255 && !g.r && !g.b);
        TEST_ASSERT_TRUE(w.r > 0 && w.r < 255 && w.r == w.g && w.g == w.b);
    }
    /* slow (500 ms) and fast (125 ms) blinks */
    assert_blink(FB_LED_NO_COMPANION, 500 * MS_US, "amber");
    assert_blink(FB_LED_SETUP_AP, 500 * MS_US, "blue");
    assert_blink(FB_LED_NO_NETWORK, 500 * MS_US, "red");
    assert_blink(FB_LED_UPDATE, 125 * MS_US, "purple");
    assert_blink(FB_LED_IDENTIFY, 125 * MS_US, "white");
    fb_rgb_t c = fb_led_color(FB_LED_NO_COMPANION, 0); /* amber: red with some green */
    TEST_ASSERT_TRUE(c.r == 255 && c.g > 0 && c.g < c.r && !c.b);
    c = fb_led_color(FB_LED_SETUP_AP, 0);
    TEST_ASSERT_TRUE(!c.r && !c.g && c.b == 255);
    c = fb_led_color(FB_LED_NO_NETWORK, 0);
    TEST_ASSERT_TRUE(c.r == 255 && !c.g && !c.b);
    c = fb_led_color(FB_LED_UPDATE, 0);
    TEST_ASSERT_TRUE(c.r > 0 && !c.g && c.b == 255);
    c = fb_led_color(FB_LED_IDENTIFY, 0);
    TEST_ASSERT_TRUE(c.r == 255 && c.g == 255 && c.b == 255);
    /* the deck problem alternates red and purple every 500 ms: never dark */
    const fb_rgb_t red = fb_led_color(FB_LED_DECK_PROBLEM, 0), purple = fb_led_color(FB_LED_DECK_PROBLEM, 500 * MS_US);
    TEST_ASSERT_TRUE(red.r == 255 && !red.g && !red.b);
    TEST_ASSERT_TRUE(purple.r > 0 && !purple.g && purple.b == 255);
    TEST_ASSERT_TRUE(same(red, fb_led_color(FB_LED_DECK_PROBLEM, 499 * MS_US)));
    TEST_ASSERT_TRUE(same(red, fb_led_color(FB_LED_DECK_PROBLEM, 1000 * MS_US)));
    TEST_ASSERT_TRUE(same(purple, fb_led_color(FB_LED_DECK_PROBLEM, 1999 * MS_US)));
}

static void test_rgb_scale(void)
{
    const fb_rgb_t amber = {255, 110, 0};
    fb_rgb_t c = fb_rgb_scale(amber, 30);
    TEST_ASSERT_EQUAL_UINT8(76, c.r);
    TEST_ASSERT_EQUAL_UINT8(33, c.g);
    TEST_ASSERT_EQUAL_UINT8(0, c.b);
    TEST_ASSERT_TRUE(same(amber, fb_rgb_scale(amber, 100)));
    TEST_ASSERT_TRUE(black(fb_rgb_scale(amber, 0)));
    TEST_ASSERT_TRUE(same(amber, fb_rgb_scale(amber, 150))); /* clamped */
    TEST_ASSERT_TRUE(black(fb_rgb_scale(amber, -5)));
}

/* ---- the tracker: the status over time, the sounds it starts ---- */

#define BIT(e) (1u << (e))

static fb_status_t up(void) /* booted, network up, Companion driving */
{
    return (fb_status_t){.booted = true, .net_up = true, .companion = true};
}

static void test_track_ready_once(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = {0};
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, 0));
    TEST_ASSERT_EQUAL_INT(FB_LED_STARTING, fb_track_led(&t, 0));
    st.booted = true;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_READY), fb_track_step(&t, &st, 100 * MS_US));
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, 200 * MS_US));
    st.booted = false; /* never again in this run */
    fb_track_step(&t, &st, 300 * MS_US);
    st.booted = true;
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, 400 * MS_US));
}

static void test_track_connected_edge(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = {.booted = true, .net_up = true};
    fb_track_step(&t, &st, 0);
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_COMPANION, fb_track_led(&t, 0));
    st.companion = true;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_CONNECTED), fb_track_step(&t, &st, S_US));
    TEST_ASSERT_EQUAL_INT(FB_LED_CONNECTED, fb_track_led(&t, S_US));
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, 2 * S_US));
    st.companion = false;
    st.companion_lost = true;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, 3 * S_US));
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_COMPANION, fb_track_led(&t, 3 * S_US));
    st.companion = true;
    st.companion_lost = false;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_CONNECTED), fb_track_step(&t, &st, 4 * S_US));
}

/* A problem sounds once at its onset, never again while it lasts; a new onset sounds again. */
static void test_track_problem_once_per_onset(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = up();
    fb_track_step(&t, &st, 0);
    st.companion = false;
    st.companion_lost = true;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, S_US));
    for (int64_t now = 2 * S_US; now < 120 * S_US; now += S_US) /* lasts two minutes */
        TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, now));
    st = up(); /* back */
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_CONNECTED), fb_track_step(&t, &st, 121 * S_US));
    st.companion = false;
    st.companion_lost = true; /* lost again */
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, 130 * S_US));
}

/* Network lost: only once it had been up (a box starting without one isn't losing it). */
static void test_track_network_lost(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = {.booted = true};
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_READY), fb_track_step(&t, &st, 0));
    for (int64_t now = S_US; now < 30 * S_US; now += S_US) TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, now));
    st.net_up = true;
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, 31 * S_US));
    st.net_up = false;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, 32 * S_US));
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_NETWORK, fb_track_led(&t, 32 * S_US));
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, 33 * S_US));
}

/* One cause often brings the next (the network goes, Companion is lost 5 s later): problems starting
 * within FB_PROBLEM_QUIET_US of one that sounded stay quiet. */
static void test_track_problem_quiet_window(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = up();
    fb_track_step(&t, &st, 0);
    st.net_up = false;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, S_US));
    st.companion = false;
    st.companion_lost = true;
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, 6 * S_US));
    st.deck_trouble = true;
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, S_US + FB_PROBLEM_QUIET_US - 1));
    st.deck_trouble = false;
    fb_track_step(&t, &st, S_US + FB_PROBLEM_QUIET_US);
    st.deck_trouble = true; /* a new onset after the window */
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, S_US + FB_PROBLEM_QUIET_US + 1));
}

/* A deck in trouble (quarantined, not responding): the light while it lasts, one sound at its onset.
 * A trouble report (a USB reset that cured it) shows for FB_DECK_ALERT_US. */
static void test_track_deck_problem(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = up();
    fb_track_step(&t, &st, 0);
    st.deck_trouble = true;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, S_US));
    TEST_ASSERT_EQUAL_INT(FB_LED_DECK_PROBLEM, fb_track_led(&t, S_US));
    for (int64_t now = 2 * S_US; now < 60 * S_US; now += S_US) {
        TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, now));
        TEST_ASSERT_EQUAL_INT(FB_LED_DECK_PROBLEM, fb_track_led(&t, now));
    }
    st.deck_trouble = false;
    fb_track_step(&t, &st, 60 * S_US);
    TEST_ASSERT_EQUAL_INT(FB_LED_CONNECTED, fb_track_led(&t, 60 * S_US));

    /* a report: 10 s of deck problem, one sound; a second report meanwhile extends it, quietly */
    const int64_t t0 = 100 * S_US;
    st.deck_alerts = 2;
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, t0));
    TEST_ASSERT_EQUAL_INT(FB_LED_DECK_PROBLEM, fb_track_led(&t, t0 + FB_DECK_ALERT_US - 1));
    st.deck_alerts = 3;
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, t0 + 5 * S_US));
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, t0 + 5 * S_US + FB_DECK_ALERT_US - 1));
    TEST_ASSERT_EQUAL_INT(FB_LED_DECK_PROBLEM, fb_track_led(&t, t0 + 5 * S_US + FB_DECK_ALERT_US - 1));
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, t0 + 5 * S_US + FB_DECK_ALERT_US));
    TEST_ASSERT_EQUAL_INT(FB_LED_CONNECTED, fb_track_led(&t, t0 + 5 * S_US + FB_DECK_ALERT_US));
}

/* A deck trouble reported before the supervisor's first report counts too (the box restarted with a
 * quarantined deck, or one failed at start-up). */
static void test_track_deck_alert_at_start(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = {.booted = true, .net_up = true, .deck_alerts = 1};
    TEST_ASSERT_EQUAL_HEX32(BIT(FB_EV_READY) | BIT(FB_EV_PROBLEM), fb_track_step(&t, &st, S_US));
    TEST_ASSERT_EQUAL_INT(FB_LED_DECK_PROBLEM, fb_track_led(&t, S_US));
}

/* "Starting" lasts while the network has never been up, FB_START_GRACE_US after the boot at most. */
static void test_track_start_grace(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = {.booted = true};
    const int64_t t0 = 5 * S_US;
    fb_track_step(&t, &st, t0);
    TEST_ASSERT_EQUAL_INT(FB_LED_STARTING, fb_track_led(&t, t0));
    TEST_ASSERT_EQUAL_INT(FB_LED_STARTING, fb_track_led(&t, t0 + FB_START_GRACE_US - 1));
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_NETWORK, fb_track_led(&t, t0 + FB_START_GRACE_US));
    fb_track_init(&t); /* up within the grace, then down: no network at once */
    fb_track_step(&t, &st, t0);
    st.net_up = true;
    fb_track_step(&t, &st, t0 + S_US);
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_COMPANION, fb_track_led(&t, t0 + S_US));
    st.net_up = false;
    fb_track_step(&t, &st, t0 + 2 * S_US);
    TEST_ASSERT_EQUAL_INT(FB_LED_NO_NETWORK, fb_track_led(&t, t0 + 2 * S_US));
}

static void test_track_setup_ap_and_update(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = up();
    st.setup_ap = true;
    fb_track_step(&t, &st, 0);
    TEST_ASSERT_EQUAL_INT(FB_LED_SETUP_AP, fb_track_led(&t, 0));
    st.update = true;
    fb_track_step(&t, &st, S_US);
    TEST_ASSERT_EQUAL_INT(FB_LED_UPDATE, fb_track_led(&t, S_US));
}

/* A firmware update shows also before the boot finished: the C6's update runs inside the network
 * stage, before the supervisor reports anything. No ready chime until the boot finished. */
static void test_track_update_before_boot(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = {.update = true};
    TEST_ASSERT_EQUAL_HEX32(0, fb_track_step(&t, &st, S_US));
    TEST_ASSERT_EQUAL_INT(FB_LED_UPDATE, fb_track_led(&t, S_US));
    st.update = false;
    fb_track_step(&t, &st, 2 * S_US);
    TEST_ASSERT_EQUAL_INT(FB_LED_STARTING, fb_track_led(&t, 2 * S_US));
}

/* Find my box: the white fast blink for 30 s, over everything, then the status again. */
static void test_track_identify(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = up();
    st.update = true;
    fb_track_step(&t, &st, 0);
    fb_track_identify(&t, S_US);
    TEST_ASSERT_EQUAL_INT(FB_LED_IDENTIFY, fb_track_led(&t, S_US));
    TEST_ASSERT_EQUAL_INT(FB_LED_IDENTIFY, fb_track_led(&t, S_US + FB_IDENTIFY_US - 1));
    TEST_ASSERT_EQUAL_INT(FB_LED_UPDATE, fb_track_led(&t, S_US + FB_IDENTIFY_US));
    TEST_ASSERT_TRUE(same(fb_track_color(&t, S_US), fb_led_color(FB_LED_IDENTIFY, S_US)));
    fb_track_identify(&t, 10 * S_US); /* again: 30 s from then */
    TEST_ASSERT_EQUAL_INT(FB_LED_IDENTIFY, fb_track_led(&t, 10 * S_US + FB_IDENTIFY_US - 1));
    TEST_ASSERT_EQUAL_INT(30 * S_US, FB_IDENTIFY_US);
}

/* Feedback.Test's light: the event's flash over the status, then the status; never over Find my box
 * or a firmware update. */
static void test_track_flash(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = up();
    st.deck_trouble = true;
    fb_track_step(&t, &st, 0);
    const int64_t t0 = S_US;
    const fb_flash_t f = fb_event_flash(FB_EV_CONNECTED);
    TEST_ASSERT_TRUE(f.count >= 1 && f.on_ms > 0);
    fb_track_flash(&t, FB_EV_CONNECTED, t0);
    TEST_ASSERT_TRUE(same(f.on, fb_track_color(&t, t0)));
    TEST_ASSERT_TRUE(same(f.on, fb_track_color(&t, t0 + f.on_ms * MS_US - 1)));
    if (f.count > 1) TEST_ASSERT_TRUE(same(f.off, fb_track_color(&t, t0 + f.on_ms * MS_US)));
    const int64_t end = t0 + ((int64_t)f.count * (f.on_ms + f.off_ms) - f.off_ms) * MS_US;
    TEST_ASSERT_TRUE(same(fb_led_color(FB_LED_DECK_PROBLEM, end), fb_track_color(&t, end)));
    /* not over a firmware update */
    st.update = true;
    fb_track_step(&t, &st, 2 * S_US);
    fb_track_flash(&t, FB_EV_CONNECTED, 2 * S_US);
    TEST_ASSERT_TRUE(same(fb_led_color(FB_LED_UPDATE, 2 * S_US), fb_track_color(&t, 2 * S_US)));
    /* every event has a flash that lights */
    for (int e = 0; e < FB_EV_COUNT; e++) {
        const fb_flash_t x = fb_event_flash((fb_event_t)e);
        TEST_ASSERT_TRUE(x.count >= 1 && x.on_ms >= 20 && !black(x.on));
    }
}

/* real events flash the light when it is on and ledFlash is on; Feedback.Test and Find my box
 * are not this question's business. */
static void test_flash_wanted(void)
{
    for (int e = 0; e < FB_EV_IDENTIFY; e++) {
        TEST_ASSERT_TRUE_MESSAGE(fb_flash_wanted((fb_event_t)e, true, true), fb_event_name((fb_event_t)e));
        TEST_ASSERT_FALSE_MESSAGE(fb_flash_wanted((fb_event_t)e, false, true), fb_event_name((fb_event_t)e));
        TEST_ASSERT_FALSE_MESSAGE(fb_flash_wanted((fb_event_t)e, true, false), fb_event_name((fb_event_t)e));
        TEST_ASSERT_FALSE_MESSAGE(fb_flash_wanted((fb_event_t)e, false, false), fb_event_name((fb_event_t)e));
    }
    TEST_ASSERT_FALSE(fb_flash_wanted(FB_EV_IDENTIFY, true, true)); /* Find my box has its own blink */
    TEST_ASSERT_FALSE(fb_flash_wanted(FB_EV_COUNT, true, true));
}

/* A flash over Find my box never shows; a second flash restarts the pattern. */
static void test_flash_not_over_identify_and_restarts(void)
{
    fb_track_t t;
    fb_track_init(&t);
    fb_status_t st = up();
    fb_track_step(&t, &st, 0);
    fb_track_identify(&t, S_US);
    fb_track_flash(&t, FB_EV_KEY_CLICK, S_US);
    TEST_ASSERT_TRUE(same(fb_led_color(FB_LED_IDENTIFY, S_US), fb_track_color(&t, S_US)));
    /* key clicks re-trigger the 60 ms blink */
    fb_track_t k;
    fb_track_init(&k);
    fb_track_step(&k, &st, 0);
    const int64_t t0 = 5 * S_US, w = 40 * MS_US;
    fb_track_flash(&k, FB_EV_KEY_CLICK, t0);
    TEST_ASSERT_TRUE(same(fb_event_flash(FB_EV_KEY_CLICK).on, fb_track_color(&k, t0 + w)));
    fb_track_flash(&k, FB_EV_KEY_CLICK, t0 + w);
    TEST_ASSERT_TRUE(same(fb_event_flash(FB_EV_KEY_CLICK).on, fb_track_color(&k, t0 + 2 * w))); /* 80 ms after the first */
    TEST_ASSERT_FALSE(same(fb_event_flash(FB_EV_KEY_CLICK).on, fb_track_color(&k, t0 + w + 61 * MS_US)));
}

static void test_event_names(void)
{
    static const char *const NAMES[FB_EV_COUNT] = {"ready",    "connected", "problem", "deckIn",  "deckOut",
                                                   "keyClick", "confirm",   "locked",  "identify"};
    for (int e = 0; e < FB_EV_COUNT; e++) {
        TEST_ASSERT_EQUAL_STRING(NAMES[e], fb_event_name((fb_event_t)e));
        fb_event_t got = FB_EV_COUNT;
        TEST_ASSERT_TRUE(fb_event_parse(NAMES[e], &got));
        TEST_ASSERT_EQUAL_INT(e, got);
    }
    fb_event_t got = FB_EV_READY;
    TEST_ASSERT_FALSE(fb_event_parse("Ready", &got));
    TEST_ASSERT_FALSE(fb_event_parse("", &got));
    TEST_ASSERT_FALSE(fb_event_parse(NULL, &got));
    TEST_ASSERT_EQUAL_STRING("", fb_event_name(FB_EV_COUNT));
    TEST_ASSERT_EQUAL_STRING("connected", fb_led_name(FB_LED_CONNECTED));
    TEST_ASSERT_EQUAL_STRING("deck_problem", fb_led_name(FB_LED_DECK_PROBLEM));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_led_priority_table);
    RUN_TEST(test_led_priority_pairs);
    RUN_TEST(test_led_starting);
    RUN_TEST(test_led_colors);
    RUN_TEST(test_rgb_scale);
    RUN_TEST(test_track_ready_once);
    RUN_TEST(test_track_connected_edge);
    RUN_TEST(test_track_problem_once_per_onset);
    RUN_TEST(test_track_network_lost);
    RUN_TEST(test_track_problem_quiet_window);
    RUN_TEST(test_track_deck_problem);
    RUN_TEST(test_track_deck_alert_at_start);
    RUN_TEST(test_track_start_grace);
    RUN_TEST(test_track_setup_ap_and_update);
    RUN_TEST(test_track_update_before_boot);
    RUN_TEST(test_track_identify);
    RUN_TEST(test_track_flash);
    RUN_TEST(test_flash_wanted);
    RUN_TEST(test_flash_not_over_identify_and_restarts);
    RUN_TEST(test_event_names);
    return UNITY_END();
}
