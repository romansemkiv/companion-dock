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
        if (b->stage == 0 && held >= SVC_SETUP_US) {
            b->stage = 1;
            return SVC_SETUP_STAGE;
        }
        if (b->stage == 1 && held >= SVC_RESET_US) {
            b->stage = 2;
            return SVC_RESET_STAGE;
        }
        return SVC_NONE;
    }
    if (!b->down) return SVC_NONE;
    b->down = false;
    if (b->stage == 2) return SVC_RESET;
    if (b->stage == 1) return SVC_SETUP;
    const int64_t held = now_us - b->pressed_at;
    return held >= SVC_SHORT_MIN_US && held < SVC_SHORT_MAX_US ? SVC_SHORT : SVC_NONE;
}
