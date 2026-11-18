#include "svcbtn_logic.h"

void svcbtn_init(svcbtn_t *b)
{
    *b = (svcbtn_t){0};
}

svc_event_t svcbtn_step(svcbtn_t *b, bool down, int64_t now_us)
{
    if (!b->armed) { /* pressed since the start: not a press until released once */
        b->armed = !down;
        return SVC_NONE;
    }
    if (down && !b->down) {
        b->down = true;
        b->pressed_at = now_us;
        b->stage = 0;
        return SVC_PRESSED;
    }
    if (down) {
        int64_t held = now_us - b->pressed_at;
        /* stages: 1 long, 2 setup, 3 reset; one per poll, in order */
        if (b->stage == 0 && held >= SVC_LONG_US) {
            b->stage = 1;
            return SVC_LONG_STAGE;
        }
        if (b->stage == 1 && held >= SVC_SETUP_US) {
            b->stage = 2;
            return SVC_SETUP_STAGE;
        }
        if (b->stage == 2 && held >= SVC_RESET_US) {
            b->stage = 3;
            return SVC_RESET_STAGE;
        }
        return SVC_NONE;
    }
    if (!b->down) return SVC_NONE;
    b->down = false;
    if (b->stage == 3) return SVC_RESET;
    if (b->stage == 2) return SVC_SETUP;
    if (b->stage == 1) return SVC_LONG;
    const int64_t held = now_us - b->pressed_at;
    if (held >= SVC_SHORT_MIN_US && held < SVC_SHORT_MAX_US) return SVC_SHORT;
    /* a long press no poll saw reach its stage (a stalled poll) is still one; the hotspot and the reset are never
     * done without their stage */
    return held >= SVC_LONG_US && held < SVC_SETUP_US ? SVC_LONG : SVC_NONE;
}
