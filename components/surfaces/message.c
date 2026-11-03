/* Pure: validation and state of the custom deck message. */
#include "surfaces/surfaces.h"

bool surf_message_valid(const char *text, uint32_t seconds)
{
    if (!text || !text[0] || seconds > SURF_MSG_MAX_S) return false;
    for (size_t i = 0; text[i]; i++) {
        const unsigned char ch = (unsigned char)text[i];
        if (i >= SURF_MSG_MAX) return false;
        if (ch != '\n' && (ch < 0x20 || ch > 0x7E)) return false;
    }
    return true;
}

void surf_message_show(surf_message_t *m, int64_t until_us)
{
    m->on = true;
    m->until_us = until_us;
    m->seq++;
}

bool surf_message_clear(surf_message_t *m)
{
    if (!m->on) return false;
    m->on = false;
    m->seq++;
    return true;
}

/* Every show, clear and expiry bumps seq, so a message that came and went between two steps still
 * leaves seq != seen and ends with a redraw. */
surf_message_action_t surf_message_step(surf_message_t *m, int64_t now_us, bool *expired)
{
    const bool exp = m->on && m->until_us && now_us >= m->until_us;
    if (exp) {
        m->on = false;
        m->seq++;
    }
    if (expired) *expired = exp;
    if (m->on) {
        m->seen = m->seq;
        return SURF_MSG_DRAW;
    }
    if (m->seen != m->seq) {
        m->seen = m->seq;
        return SURF_MSG_END;
    }
    return SURF_MSG_IDLE;
}
