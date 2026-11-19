#include "satclient_logic.h"

#include <stdio.h>
#include <string.h>

uint32_t satc_wait_ms(unsigned queued, bool ready, uint32_t idle_ms)
{
    return queued && ready ? 0 : idle_ms;
}

uint32_t satc_next_backoff(uint32_t backoff, bool target_changed)
{
    if (target_changed) return SATC_BACKOFF_FIRST_MS;
    return backoff >= 2000 ? 5000 : backoff * 2;
}

int satc_end_event(bool connected, bool got_ready, bool target_changed, bool too_old)
{
    (void)connected; /* a connection that ended before READY fails like one never made */
    if (target_changed || too_old) return -1;
    return got_ready ? SATC_EV_DROPPED : SATC_EV_ATTEMPT_FAILED;
}

size_t satc_write_all(const satc_io_t *io, const char *data, size_t len, int budget_ms, int step_ms)
{
    size_t off = 0;
    int waited = 0;
    while (off < len) {
        bool again = false;
        const long n = io->send(io->ctx, data + off, len - off, &again);
        if (n > 0) {
            if (memchr(data + off, '\n', (size_t)n)) waited = 0; /* a line out: the next one's budget starts */
            off += (size_t)n;
            continue;
        }
        if (n < 0 && again) {
            if (waited >= budget_ms) break;
            io->wait(io->ctx);
            waited += step_ms;
            continue;
        }
        break;
    }
    return off;
}

int satc_streak_step(satc_streak_t *s, const char *key, bool failed, uint32_t *ended)
{
    const bool same = s->n && strncmp(s->key, key, sizeof s->key - 1) == 0;
    const uint32_t over = !failed || !same ? s->n : 0; /* a success, or another target, ends the streak */
    if (ended) *ended = over;
    if (!failed) {
        s->n = 0;
        s->key[0] = '\0';
        return SATC_LOG_NONE;
    }
    if (same) {
        s->n++;
        return SATC_LOG_QUIET;
    }
    snprintf(s->key, sizeof s->key, "%s", key);
    s->n = 1;
    return SATC_LOG_FIRST;
}
