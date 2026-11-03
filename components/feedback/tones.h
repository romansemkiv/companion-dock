#pragma once
/* Sounds as text: space- (or comma-) separated "freq:ms" steps, 0 Hz = a pause, e.g.
 * "2000:80 0:40 3000:150". Pure C, no ESP-IDF: also built by the host tests. */
#include <stdint.h>

#define TONES_MAX 16 /* steps in one sound */
#define TONES_MIN_HZ 100
#define TONES_MAX_HZ 10000
#define TONES_MIN_MS 10
#define TONES_MAX_MS 2000
#define TONES_TOTAL_MS 10000 /* the whole sound */

typedef struct {
    uint16_t hz; /* 0: silence */
    uint16_t ms;
} tone_t;

/* Parses s into out[]. Returns the number of steps (1..TONES_MAX), or 0 with *err (if given) set to
 * the reason. */
int tones_parse(const char *s, tone_t out[TONES_MAX], const char **err);
