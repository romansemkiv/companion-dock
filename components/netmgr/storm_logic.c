#include "netmgr/storm.h"

#include <string.h>

void storm_init(storm_t *s)
{
    memset(s, 0, sizeof *s);
    s->hold_ms = STORM_HOLD_MS;
}

bool storm_blocked(const storm_t *s) { return s->st == STORM_BLOCKED; }

static storm_act_t block(storm_t *s, int64_t now_ms)
{
    s->st = STORM_BLOCKED;
    s->until_ms = now_ms + s->hold_ms;
    return STORM_ACT_BLOCK;
}

storm_act_t storm_tick(storm_t *s, int64_t now_ms, uint32_t bcast, uint32_t lost, storm_ev_t *ev)
{
    *ev = STORM_EV_NONE;
    const bool storm = bcast >= STORM_ON_PER_TICK && lost >= STORM_LOST_PER_TICK;
    switch (s->st) {
    case STORM_OPEN:
        s->over = storm ? s->over + 1 : 0;
        if (s->over < STORM_ON_TICKS) return STORM_ACT_NONE;
        s->over = 0;
        s->storms++;
        s->hold_ms = STORM_HOLD_MS;
        s->began_ms = now_ms;
        s->peak = bcast;
        *ev = STORM_EV_BEGAN;
        return block(s, now_ms);
    case STORM_BLOCKED:
        if (now_ms < s->until_ms) return STORM_ACT_NONE;
        s->st = STORM_PROBE;
        s->probe_left = STORM_PROBE_TICKS;
        return STORM_ACT_OPEN;
    case STORM_PROBE:
    default:
        if (bcast > s->peak) s->peak = bcast;
        if (storm) {
            s->hold_ms = s->hold_ms * 2 > STORM_HOLD_MAX_MS ? STORM_HOLD_MAX_MS : s->hold_ms * 2;
            return block(s, now_ms);
        }
        if (--s->probe_left > 0) return STORM_ACT_NONE;
        s->st = STORM_OPEN;
        *ev = STORM_EV_ENDED;
        return STORM_ACT_NONE;
    }
}
