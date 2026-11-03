/* PCM for the tones: see tonegen.h. */
#include "tonegen.h"

/* sin(i * pi / 128) * 32767, i = 0..64: a quarter of the 256-step wave */
static const int16_t QUARTER[65] = {
    0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,  7962,  8739,  9512,
    10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
    19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
    26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
    31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767};

static int sine(uint32_t phase)
{
    const unsigned i = phase >> 24, k = i & 63;
    switch (i >> 6) {
    case 0: return QUARTER[k];
    case 1: return QUARTER[64 - k];
    case 2: return -QUARTER[k];
    default: return -QUARTER[64 - k];
    }
}

void tonegen_init(tonegen_t *g, uint32_t rate)
{
    *g = (tonegen_t){.rate = rate ? rate : 16000};
}

int tonegen_level(int volume)
{
    if (volume <= 0) return 0;
    if (volume > 100) volume = 100;
    return 32000 * volume * volume / 10000; /* loudness grows about with the square */
}

void tonegen_set(tonegen_t *g, uint16_t hz, int volume)
{
    g->target = hz ? tonegen_level(volume) : 0;
    if (hz) g->step = (uint32_t)(((uint64_t)hz << 32) / g->rate); /* a stop keeps the old one while it ramps out */
    const int samples = (int)(g->rate * TONEGEN_RAMP_MS / 1000);
    const int d = g->target > g->amp ? g->target - g->amp : g->amp - g->target;
    g->ramp = d / (samples > 0 ? samples : 1);
    if (g->ramp < 1) g->ramp = 1;
}

void tonegen_fill(tonegen_t *g, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (g->amp < g->target) g->amp = g->amp + g->ramp < g->target ? g->amp + g->ramp : g->target;
        else if (g->amp > g->target) g->amp = g->amp - g->ramp > g->target ? g->amp - g->ramp : g->target;
        if (!g->amp) {
            out[i] = 0;
            if (!g->target) g->phase = 0;
            continue;
        }
        out[i] = (int16_t)(sine(g->phase) * g->amp / 32767);
        g->phase += g->step;
    }
}

bool tonegen_idle(const tonegen_t *g)
{
    return !g->amp && !g->target;
}
