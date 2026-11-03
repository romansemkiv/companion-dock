/* The sounds and their player: see fbsound.h. */
#include "fbsound.h"

#include <string.h>

/* The sound table, at 16 kHz: chimes in the 1-2 kHz range, a low double beep for a
 * problem, short blips for a deck, a 10 ms click (the shortest step tones.h takes; its 3 ms ramps
 * make it a tick), and Find my box beeping for 3 s. */
static const fb_sound_t SOUNDS[FB_EV_COUNT] = {
    [FB_EV_READY] = {"1047:90 0:20 1568:180", 100, 2},     /* C6, G6: a rising two-note chime */
    [FB_EV_CONNECTED] = {"2093:160", 100, 2},              /* C7: one bright chime */
    [FB_EV_PROBLEM] = {"440:160 0:90 440:160", 100, 3},    /* a low double beep */
    [FB_EV_DECK_IN] = {"988:50 1480:70", 80, 1},           /* a rising blip */
    [FB_EV_DECK_OUT] = {"1480:50 988:70", 80, 1},          /* a falling one */
    [FB_EV_KEY_CLICK] = {"3000:10", 40, 0},                /* a quiet tick */
    [FB_EV_CONFIRM] = {"1319:60 0:20 1760:90", 60, 1},     /* E6, A6: a short rising pair (a hold done) */
    [FB_EV_LOCKED] = {"330:80 0:30 247:110", 60, 1},       /* a low falling pair (locked) */
    [FB_EV_IDENTIFY] = {"2000:150 0:100 3000:150 0:350 2000:150 0:100 3000:150 0:350 "
                        "2000:150 0:100 3000:150 0:350 2000:150 0:100 3000:150 0:350",
                        100, 4},                           /* low-high, four times: 3 s */
};

const fb_sound_t *fb_sound(fb_event_t e)
{
    return (unsigned)e < FB_EV_COUNT ? &SOUNDS[e] : NULL;
}

bool fb_sound_takes_over(bool playing, fb_event_t now, fb_event_t next)
{
    const fb_sound_t *n = fb_sound(next), *p = fb_sound(now);
    if (!n) return false;
    if (!playing || !p) return true;
    if (next == FB_EV_KEY_CLICK) return false;
    return n->prio >= p->prio;
}

void fb_player_init(fb_player_t *p, uint32_t rate)
{
    memset(p, 0, sizeof *p);
    tonegen_init(&p->gen, rate);
    p->ev = FB_EV_COUNT;
}

static void start_step(fb_player_t *p)
{
    const tone_t *s = &p->step[p->i];
    p->left = (uint32_t)s->ms * p->gen.rate / 1000;
    tonegen_set(&p->gen, s->hz, p->volume);
}

bool fb_player_post(fb_player_t *p, fb_event_t ev, int volume)
{
    const fb_sound_t *s = fb_sound(ev);
    if (!s || !fb_sound_takes_over(p->playing, p->ev, ev)) return false;
    const int n = tones_parse(s->tones, p->step, NULL);
    if (n <= 0) return false;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    p->n = n;
    p->i = 0;
    p->ev = ev;
    p->playing = true;
    p->volume = volume * s->level / 100;
    start_step(p); /* from the tone sounding now: the phase is kept, the level ramps (no click) */
    return true;
}

void fb_player_fill(fb_player_t *p, int16_t *out, size_t n)
{
    size_t at = 0;
    while (at < n) {
        if (!p->playing) { /* the last tone ramping out, then silence */
            tonegen_fill(&p->gen, out + at, n - at);
            return;
        }
        const size_t k = n - at < p->left ? n - at : p->left;
        tonegen_fill(&p->gen, out + at, k);
        at += k;
        p->left -= (uint32_t)k;
        if (p->left) continue;
        if (++p->i < p->n) {
            start_step(p);
        } else {
            p->playing = false;
            tonegen_set(&p->gen, 0, 0);
        }
    }
}

bool fb_player_playing(const fb_player_t *p, fb_event_t *ev)
{
    if (ev) *ev = p->ev;
    return p->playing;
}

bool fb_player_idle(const fb_player_t *p)
{
    return !p->playing && tonegen_idle(&p->gen);
}

void fb_retry_init(fb_retry_t *r)
{
    *r = (fb_retry_t){0};
}

bool fb_retry_failed(fb_retry_t *r, int64_t now_us)
{
    const bool first = !r->wait_us;
    r->wait_us = first ? FB_RETRY_FIRST_US : r->wait_us * 2 > FB_RETRY_MAX_US ? FB_RETRY_MAX_US : r->wait_us * 2;
    r->retry_at_us = now_us + r->wait_us;
    return first;
}

bool fb_retry_due(const fb_retry_t *r, int64_t now_us)
{
    return !r->wait_us || now_us >= r->retry_at_us;
}

bool fb_retry_ok(fb_retry_t *r)
{
    const bool streak = r->wait_us != 0;
    *r = (fb_retry_t){0};
    return streak;
}
