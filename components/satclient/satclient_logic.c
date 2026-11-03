#include "satclient_logic.h"

#include <string.h>

uint32_t satc_wait_ms(unsigned queued, bool ready, uint32_t idle_ms)
{
    return queued && ready ? 0 : idle_ms;
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
