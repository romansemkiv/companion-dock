#pragma once
/* The sounds (pure: no ESP-IDF header; host-tested in test/host/test_feedback_sound.c): each event's
 * tones and level, which sound plays when another comes, and the player the speaker task runs (it
 * makes the samples; speaker.c only hands them to the codec).
 *
 * A new sound replaces the one playing, unless it ranks lower (prio): a key click (lowest) never
 * cuts another sound off, and the deck blips that follow a USB reset don't cut the problem beep, nor
 * those of the decks found at start-up the ready chime. A sound dropped is gone, not queued. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "feedback/feedback.h"
#include "tonegen.h"
#include "tones.h"

typedef struct {
    const char *tones; /* "freq:ms" steps (tones.h) */
    uint8_t level;     /* percent of the volume setting it plays at */
    uint8_t prio;      /* it cuts off a sound of the same or a lower prio, and is dropped for a higher one */
} fb_sound_t;

/* e's sound; NULL for no event. */
const fb_sound_t *fb_sound(fb_event_t e);
/* Whether next starts now: always when nothing plays (playing false), else when `now` (the sound
 * playing) doesn't rank above it. A key click never starts while another sound plays. */
bool fb_sound_takes_over(bool playing, fb_event_t now, fb_event_t next);

typedef struct {
    tonegen_t gen;
    tone_t step[TONES_MAX];
    int n, i;       /* steps of the sound, the one playing */
    uint32_t left;  /* samples of step i still to come */
    int volume;     /* tonegen volume of the sound: the setting times its level */
    fb_event_t ev;
    bool playing;   /* steps still to come */
} fb_player_t;

void fb_player_init(fb_player_t *p, uint32_t rate);
/* ev's sound at volume (0..100) from its first step: true when it plays now, false when it was dropped
 * (fb_sound_takes_over). */
bool fb_player_post(fb_player_t *p, fb_event_t ev, int volume);
/* The next n samples: the steps on time to the sample, then the last tone ramping out, then silence. */
void fb_player_fill(fb_player_t *p, int16_t *out, size_t n);
/* A sound's steps are still under way (*ev, if not NULL, is which). */
bool fb_player_playing(const fb_player_t *p, fb_event_t *ev);
/* Nothing plays and the last tone has ramped out: the codec may close. */
bool fb_player_idle(const fb_player_t *p);

/* The codec failing after the start (speaker.c): a failed open or write closes it and drops the
 * amplifier; it is tried again only when a sound comes and the wait has run out, FB_RETRY_FIRST_US
 * after the first failure, then twice as long after each one, up to FB_RETRY_MAX_US. Sounds meanwhile
 * are dropped without a word: a streak logs once when it begins and once when it ends. */
#define FB_RETRY_FIRST_US (10 * 1000000LL)
#define FB_RETRY_MAX_US (10 * 60 * 1000000LL)

typedef struct {
    int64_t wait_us;     /* the wait after the last failure; 0: the codec works */
    int64_t retry_at_us; /* no try before then */
} fb_retry_t;

void fb_retry_init(fb_retry_t *r);
/* The codec failed at now_us: true when that begins a streak (log it). */
bool fb_retry_failed(fb_retry_t *r, int64_t now_us);
/* The codec may be tried (opened) at now_us. */
bool fb_retry_due(const fb_retry_t *r, int64_t now_us);
/* It worked: true when that ends a streak (log the recovery). */
bool fb_retry_ok(fb_retry_t *r);
