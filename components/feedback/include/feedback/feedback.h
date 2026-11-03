#pragma once
/* The board's speaker and the status light (one WS2812B-type NeoPixel, GRB, on a header pin).
 *
 * Sounds (speaker.c, on the board's ES8311 codec): ready (the boot finished), connected (Companion
 * connected / drives a deck), problem (Companion lost, a deck in trouble, the network lost: once per
 * onset), deck plugged / unplugged, a click on every key-down, the maintenance UI's confirm (a hold
 * done) and locked (a button the settings password locks), and "Find my box". Each but Find my box
 * can be turned off (boxconfig's sound* keys; confirm and locked go with soundKeyClick); the speaker
 * as a whole too (speakerEnabled).
 * The light (led.c) shows the status, highest priority first: Find my box (white, fast blink, 30 s),
 * firmware update (purple, fast), a deck problem (red / purple), no network (red, slow), the setup
 * hotspot (blue, slow), no Companion (amber, slow), connected (green, dim), starting (white, dim).
 *
 * The event and status types are pure (also used by the host tests); the functions are ESP-only. */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What makes a sound. */
typedef enum {
    FB_EV_READY,     /* the boot finished (once) */
    FB_EV_CONNECTED, /* Satellite: Companion connected; dock mode: Companion drives a deck */
    FB_EV_PROBLEM,   /* a problem began: Companion lost, a deck in trouble, the network lost */
    FB_EV_DECK_IN,   /* a Stream Deck plugged in */
    FB_EV_DECK_OUT,  /* ... removed */
    FB_EV_KEY_CLICK, /* a key-down Companion gets, or the maintenance UI */
    FB_EV_CONFIRM,   /* the maintenance UI: a hold finished (soundKeyClick) */
    FB_EV_LOCKED,    /* the maintenance UI: a button the settings password locks (soundKeyClick) */
    FB_EV_IDENTIFY,  /* Find my box */
    FB_EV_COUNT
} fb_event_t;

/* "ready", "connected", "problem", "deckIn", "deckOut", "keyClick", "confirm", "locked", "identify" ("" for
 * others). */
const char *fb_event_name(fb_event_t e);
/* The event with that name (exact, case-sensitive); false for none. */
bool fb_event_parse(const char *name, fb_event_t *out);

/* What the box is doing, as the supervisor reports it (every tick, and at network changes). */
typedef struct {
    bool booted;          /* the boot finished (the supervisor runs) */
    bool update;          /* a firmware upload is being written (POST /ota) */
    bool net_up;          /* a route: Ethernet, or the Wi-Fi station, with an address */
    bool setup_ap;        /* the setup hotspot is held on (RESET x3, the service button) */
    bool companion;       /* Satellite: Companion's session is ready; dock mode: it drives a deck */
    bool companion_lost;  /* Companion went away after it drove the decks, until it does again */
    bool deck_trouble;    /* a deck is quarantined, stopped responding or isn't taking images */
    uint32_t deck_alerts; /* counts every such report since the start (a USB reset among them) */
} fb_status_t;

#ifdef ESP_PLATFORM
#include "boxconfig/config.h"
#include "esp_err.h"

/* The speaker's amplifier off (GPIO53 low): the first thing in the boot, before anything else runs,
 * so it never floats on longer than the bootloader takes. Idempotent. */
void feedback_amp_off(void);
/* Starts the speaker task (it brings the codec up) and the light's task with cfg's settings. Call
 * once, early in the boot: the light shows "starting" from then on. */
esp_err_t feedback_start(const box_config_t *cfg);
/* A firmware update other than the P4's own upload (the C6's, c6fw) is running: the light's purple
 * blink while on, also before the boot finished. Any task. */
void feedback_firmware_update(bool on);
/* The settings changed (cfg copied): volume, sounds, the light on or off, its pin and brightness
 * apply at once. */
void feedback_apply(const box_config_t *cfg);
/* The supervisor's report. Never blocks. */
void feedback_status(const fb_status_t *st);
/* ev's sound, when the speaker and that sound are on (deck in / out: soundDeckPlug). Never blocks:
 * dropped when the speaker's queue is full. Any task. Status sounds (ready, connected, problem) come
 * from feedback_status; Find my box from feedback_identify. */
void feedback_event(fb_event_t ev);
/* Find my box: the identify beeps (3 s) and the light's white fast blink (30 s), whatever the
 * settings say, at no less than half volume. */
void feedback_identify(void);
/* Feedback.Test: ev's sound, also when it (or the speaker) is turned off, at volume 0..100 (below 0:
 * the set volume), and its flash on the light (identify: Find my box). */
void feedback_test(fb_event_t ev, int volume);
/* NULL while the speaker works, else why it doesn't ("starting" until the codec is up). */
const char *feedback_speaker_error(void);
#endif

#ifdef __cplusplus
}
#endif
