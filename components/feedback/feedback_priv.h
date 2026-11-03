#pragma once
/* Between feedback.c, speaker.c and led.c. ESP-only. */
#include <stdbool.h>
#include "esp_err.h"
#include "fbstatus.h"
#include "feedback/feedback.h"

/* ---- speaker.c ---- */

/* GPIO53 (the amplifier's enable) an output, driven low. Idempotent; before the codec exists. */
void speaker_amp_off(void);
/* Starts the speaker task, which brings the codec up (the board's I2C bus, I2S) and plays what
 * speaker_play queues. Drops the amplifier first, and at every esp_restart from then on. */
esp_err_t speaker_start(void);
/* Queues ev's sound at volume 0..100 for the speaker task. Never blocks: false when the queue is full
 * (dropped). */
bool speaker_play(fb_event_t ev, int volume);
/* NULL while the codec works, else why it doesn't ("starting" until it is up). */
const char *speaker_error(void);

/* ---- led.c (the feedback task only) ---- */

/* The NeoPixel on gpio through RMT (a pin already open is closed first). false when no RMT channel
 * could be had; nothing is shown then. */
bool led_open(int gpio);
/* Releases the pin and the RMT channel. */
void led_close(void);
/* Shows c (sent in GRB order; nothing is sent when it equals the colour shown). */
void led_show(fb_rgb_t c);
