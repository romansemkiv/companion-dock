/* The status light and the status sounds, decided: see fbstatus.h. */
#include "fbstatus.h"

#include <string.h>

#define BIT(e) (1u << (e))

static const char *const EVENT_NAMES[FB_EV_COUNT] = {
    [FB_EV_READY] = "ready",       [FB_EV_CONNECTED] = "connected", [FB_EV_PROBLEM] = "problem",
    [FB_EV_DECK_IN] = "deckIn",    [FB_EV_DECK_OUT] = "deckOut",    [FB_EV_KEY_CLICK] = "keyClick",
    [FB_EV_CONFIRM] = "confirm",   [FB_EV_LOCKED] = "locked",       [FB_EV_IDENTIFY] = "identify",
};

const char *fb_event_name(fb_event_t e)
{
    return (unsigned)e < FB_EV_COUNT ? EVENT_NAMES[e] : "";
}

bool fb_event_parse(const char *name, fb_event_t *out)
{
    if (!name) return false;
    for (int e = 0; e < FB_EV_COUNT; e++) {
        if (strcmp(name, EVENT_NAMES[e]) == 0) {
            *out = (fb_event_t)e;
            return true;
        }
    }
    return false;
}

const char *fb_led_name(fb_led_t s)
{
    static const char *const NAMES[] = {"starting",   "connected",    "no_companion", "setup_ap",
                                        "no_network", "deck_problem", "update",       "identify"};
    return (unsigned)s < sizeof NAMES / sizeof *NAMES ? NAMES[s] : "";
}

/* ---- which status wins, which colour, on or off now ---- */

fb_led_t fb_led_state(const fb_led_in_t *in)
{
    if (in->identify) return FB_LED_IDENTIFY;
    if (in->update) return FB_LED_UPDATE;
    if (in->deck_problem) return FB_LED_DECK_PROBLEM;
    if (!in->booted) return FB_LED_STARTING;
    if (!in->net_up) return in->start_grace ? FB_LED_STARTING : FB_LED_NO_NETWORK;
    if (in->setup_ap) return FB_LED_SETUP_AP;
    if (!in->companion) return FB_LED_NO_COMPANION;
    return FB_LED_CONNECTED;
}

/* The status colours; "dim" ones well below full */
static const fb_rgb_t BLACK = {0, 0, 0}, WHITE = {255, 255, 255}, WHITE_DIM = {60, 60, 60}, GREEN_DIM = {0, 120, 0},
                      AMBER = {255, 110, 0}, BLUE = {0, 0, 255}, RED = {255, 0, 0}, PURPLE = {160, 0, 255},
                      GREEN = {0, 255, 0}, CYAN = {0, 200, 255};

fb_rgb_t fb_led_color(fb_led_t s, int64_t now_us)
{
    if (now_us < 0) now_us = 0;
    const bool slow = (now_us / FB_SLOW_US) % 2 == 0, fast = (now_us / FB_FAST_US) % 2 == 0;
    switch (s) {
    case FB_LED_STARTING: return WHITE_DIM;
    case FB_LED_CONNECTED: return GREEN_DIM;
    case FB_LED_NO_COMPANION: return slow ? AMBER : BLACK;
    case FB_LED_SETUP_AP: return slow ? BLUE : BLACK;
    case FB_LED_NO_NETWORK: return slow ? RED : BLACK;
    case FB_LED_DECK_PROBLEM: return slow ? RED : PURPLE;
    case FB_LED_UPDATE: return fast ? PURPLE : BLACK;
    case FB_LED_IDENTIFY: return fast ? WHITE : BLACK;
    default: return BLACK;
    }
}

fb_rgb_t fb_rgb_scale(fb_rgb_t c, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (fb_rgb_t){(uint8_t)(c.r * pct / 100), (uint8_t)(c.g * pct / 100), (uint8_t)(c.b * pct / 100)};
}

fb_flash_t fb_event_flash(fb_event_t e)
{
    switch (e) {
    case FB_EV_READY: return (fb_flash_t){WHITE, BLACK, 2, 150, 150};
    case FB_EV_CONNECTED: return (fb_flash_t){GREEN, BLACK, 2, 150, 150};
    case FB_EV_PROBLEM: return (fb_flash_t){RED, PURPLE, 4, 250, 250};
    case FB_EV_DECK_IN: return (fb_flash_t){CYAN, BLACK, 1, 300, 0};
    case FB_EV_DECK_OUT: return (fb_flash_t){CYAN, BLACK, 2, 100, 100};
    case FB_EV_KEY_CLICK: return (fb_flash_t){WHITE, BLACK, 1, 60, 0};
    case FB_EV_CONFIRM: return (fb_flash_t){GREEN, BLACK, 1, 200, 0};
    case FB_EV_LOCKED: return (fb_flash_t){RED, BLACK, 1, 200, 0};
    default: return (fb_flash_t){WHITE, BLACK, 4, 125, 125}; /* identify */
    }
}

bool fb_flash_wanted(fb_event_t e, bool led_on, bool flash_on)
{
    return led_on && flash_on && (unsigned)e < FB_EV_IDENTIFY;
}

/* ---- the tracker ---- */

void fb_track_init(fb_track_t *t)
{
    memset(t, 0, sizeof *t);
}

uint32_t fb_track_step(fb_track_t *t, const fb_status_t *st, int64_t now_us)
{
    const fb_status_t *was = &t->last;
    uint32_t ev = 0;
    if (st->booted && !t->ready_done) {
        t->ready_done = true;
        t->booted_us = now_us;
        ev |= BIT(FB_EV_READY);
    }
    if (st->companion && !was->companion) ev |= BIT(FB_EV_CONNECTED);
    if (st->deck_alerts != was->deck_alerts) t->alert_until_us = now_us + FB_DECK_ALERT_US;
    const bool deck = st->deck_trouble || now_us < t->alert_until_us;
    const bool onset = (st->companion_lost && !was->companion_lost) || (deck && !t->deck_problem) ||
                       (!st->net_up && was->net_up);
    if (onset && (!t->problem_sounded || now_us - t->problem_us >= FB_PROBLEM_QUIET_US)) {
        ev |= BIT(FB_EV_PROBLEM);
        t->problem_sounded = true;
        t->problem_us = now_us;
    }
    if (st->net_up) t->net_was_up = true;
    t->deck_problem = deck;
    t->last = *st;
    return ev;
}

void fb_track_identify(fb_track_t *t, int64_t now_us)
{
    t->identify_until_us = now_us + FB_IDENTIFY_US;
}

void fb_track_flash(fb_track_t *t, fb_event_t e, int64_t now_us)
{
    t->flash = fb_event_flash(e);
    t->flash_start_us = now_us;
    t->flash_end_us = now_us + ((int64_t)t->flash.count * (t->flash.on_ms + t->flash.off_ms) - t->flash.off_ms) * 1000;
}

fb_led_t fb_track_led(const fb_track_t *t, int64_t now_us)
{
    const fb_led_in_t in = {
        .identify = now_us < t->identify_until_us,
        .update = t->last.update,
        .deck_problem = t->last.deck_trouble || now_us < t->alert_until_us,
        .booted = t->last.booted,
        .net_up = t->last.net_up,
        .start_grace = t->ready_done && !t->net_was_up && now_us - t->booted_us < FB_START_GRACE_US,
        .setup_ap = t->last.setup_ap,
        .companion = t->last.companion,
    };
    return fb_led_state(&in);
}

fb_rgb_t fb_track_color(const fb_track_t *t, int64_t now_us)
{
    const fb_led_t s = fb_track_led(t, now_us);
    const int64_t period = ((int64_t)t->flash.on_ms + t->flash.off_ms) * 1000;
    if (s != FB_LED_IDENTIFY && s != FB_LED_UPDATE && period > 0 && now_us >= t->flash_start_us &&
        now_us < t->flash_end_us)
        return (now_us - t->flash_start_us) % period < (int64_t)t->flash.on_ms * 1000 ? t->flash.on : t->flash.off;
    return fb_led_color(s, now_us);
}
