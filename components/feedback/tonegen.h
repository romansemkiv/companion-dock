#pragma once
/* PCM for the tones (tones.h) on the speaker: a sine at each step's frequency, its level from the
 * volume, with TONEGEN_RAMP_MS ramps so a tone starting or stopping does not click; a change of
 * frequency keeps the phase (no click either). Integers only. Pure (host-tested). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TONEGEN_RAMP_MS 3

typedef struct {
    uint32_t rate, phase, step;
    int amp, target, ramp;
} tonegen_t;

void tonegen_init(tonegen_t *g, uint32_t rate);
/* The next tone: hz 0 is silence (the sounding one ramps out); volume 0-100. */
void tonegen_set(tonegen_t *g, uint16_t hz, int volume);
void tonegen_fill(tonegen_t *g, int16_t *out, size_t n);
bool tonegen_idle(const tonegen_t *g); /* silent, nothing to ramp */
int tonegen_level(int volume);         /* the peak sample at a volume: 0 at 0, rising, <= 32767 */
