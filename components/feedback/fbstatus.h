#pragma once
/* What the status light shows and which status changes sound (pure: no ESP-IDF header; host-tested
 * in test/host/test_feedback_logic.c). feedback.c steps the tracker every 20 ms with the supervisor's
 * last report (fb_status_t) and shows / plays what it says.
 *
 * The light, highest priority first:
 *   1. Find my box        white, fast blink, FB_IDENTIFY_US
 *   2. firmware update    purple, fast blink
 *   3. deck problem       red / purple alternating (a deck quarantined or not responding)
 *   4. no network         red, slow blink
 *   5. setup hotspot      blue, slow blink (held on by RESET x3 or the service button; the hotspot
 *                         that is simply on by default doesn't count, or it would hide 6 and 7)
 *   6. no Companion       amber, slow blink (network up; dock mode: Companion drives no deck)
 *   7. connected          green, steady, dim
 *   8. starting           white, steady, dim: until the boot finished, and after it while the network
 *                         has not come up yet, FB_START_GRACE_US at most (a box getting its address
 *                         isn't "no network")
 * Slow is 500 ms on and off, fast 125 ms. Colours are at full brightness;
 * feedback.c scales them by ledBrightness.
 *
 * Sounds: ready once the boot finished; connected when Companion comes; problem at the onset of each
 * problem (Companion lost after it was there, a deck problem, the network lost after it was up),
 * never again while it lasts, and quiet when it starts within FB_PROBLEM_QUIET_US of a problem that
 * sounded (the network going takes Companion with it a few seconds later: one beep). A deck trouble
 * report (fb_status_t.deck_alerts) counts as a deck problem for FB_DECK_ALERT_US: a deck reset that
 * cured it at once still beeps and shows. */
#include <stdbool.h>
#include <stdint.h>
#include "feedback/feedback.h"

#define FB_SLOW_US 500000LL                  /* slow blink: on, then off, this long each */
#define FB_FAST_US 125000LL                  /* fast blink */
#define FB_START_GRACE_US (15 * 1000000LL)   /* "starting" after the boot while the network never came up */
#define FB_DECK_ALERT_US (10 * 1000000LL)    /* a deck trouble report shows this long */
#define FB_PROBLEM_QUIET_US (10 * 1000000LL) /* a problem starting this soon after a beep stays quiet */
#define FB_IDENTIFY_US (30 * 1000000LL)      /* Find my box: the light */

typedef enum {
    FB_LED_STARTING,
    FB_LED_CONNECTED,
    FB_LED_NO_COMPANION,
    FB_LED_SETUP_AP,
    FB_LED_NO_NETWORK,
    FB_LED_DECK_PROBLEM,
    FB_LED_UPDATE,
    FB_LED_IDENTIFY,
} fb_led_t;

/* "starting", "connected", "no_companion", "setup_ap", "no_network", "deck_problem", "update",
 * "identify". */
const char *fb_led_name(fb_led_t s);

/* What the light decides on. */
typedef struct {
    bool identify;     /* Find my box is on */
    bool update;       /* a firmware upload is being written */
    bool deck_problem; /* a deck in trouble now, or reported less than FB_DECK_ALERT_US ago */
    bool booted;
    bool net_up;
    bool start_grace;  /* booted less than FB_START_GRACE_US ago, the network never up since */
    bool setup_ap;
    bool companion;
} fb_led_in_t;

/* Which status wins. */
fb_led_t fb_led_state(const fb_led_in_t *in);

typedef struct {
    uint8_t r, g, b;
} fb_rgb_t;

/* State s's colour at now_us, at full brightness: black in a blink's off half; the deck problem is red
 * in the first half of each second and purple in the second. */
fb_rgb_t fb_led_color(fb_led_t s, int64_t now_us);
/* c at pct percent (clamped to 0..100). */
fb_rgb_t fb_rgb_scale(fb_rgb_t c, int pct);

/* Feedback.Test's light for an event: count flashes of on_ms in colour `on`, each followed by off_ms
 * of colour `off` (but the last), over the status. */
typedef struct {
    fb_rgb_t on, off;
    uint8_t count;
    uint16_t on_ms, off_ms;
} fb_flash_t;

fb_flash_t fb_event_flash(fb_event_t e);
/* whether a real event e flashes the light: the light is on (ledEnabled), ledFlash is on and e is not
 * Find my box (its own blink). Feedback.Test flashes whatever this says. */
bool fb_flash_wanted(fb_event_t e, bool led_on, bool flash_on);

/* The status over time. Zero it with fb_track_init; step it with the latest report. */
typedef struct {
    fb_status_t last;          /* the report at the last step */
    bool ready_done;           /* FB_EV_READY sounded */
    bool net_was_up;           /* the network has been up in this run */
    int64_t booted_us;         /* when the boot was first seen finished */
    int64_t alert_until_us;    /* a deck trouble report shows until then */
    bool deck_problem;         /* at the last step */
    bool problem_sounded;      /* problem_us is set */
    int64_t problem_us;        /* when a problem last sounded */
    int64_t identify_until_us; /* Find my box until then (0: off) */
    fb_flash_t flash;          /* Feedback.Test's flash */
    int64_t flash_start_us, flash_end_us;
} fb_track_t;

void fb_track_init(fb_track_t *t);
/* One step at now_us with the supervisor's latest report: returns the sounds to play now, a bit
 * (1u << fb_event_t) each (FB_EV_READY, FB_EV_CONNECTED, FB_EV_PROBLEM). */
uint32_t fb_track_step(fb_track_t *t, const fb_status_t *st, int64_t now_us);
/* Find my box from now_us on, for FB_IDENTIFY_US (again: from now on). */
void fb_track_identify(fb_track_t *t, int64_t now_us);
/* e's flash (fb_event_flash) from now_us on. */
void fb_track_flash(fb_track_t *t, fb_event_t e, int64_t now_us);
/* The status the light shows at now_us (from the last step's report). */
fb_led_t fb_track_led(const fb_track_t *t, int64_t now_us);
/* The light's colour at now_us, at full brightness: the status, with a test flash over it unless Find
 * my box or a firmware update shows. */
fb_rgb_t fb_track_color(const fb_track_t *t, int64_t now_us);
