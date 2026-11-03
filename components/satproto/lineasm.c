#include "satproto/lineasm.h"

#include <string.h>

void sat_lineasm_init(sat_lineasm_t *la, char *buf, size_t cap)
{
    la->buf = buf;
    la->cap = cap;
    la->len = 0;
    la->discarding = false;
}

void sat_lineasm_feed(sat_lineasm_t *la, const char *data, size_t n,
                      void (*on_line)(void *ctx, char *line), void *ctx)
{
    /* a line's chars copied a run at a time (memchr / memcpy), not one branchy store a byte. As before: a CR
     * dropped (never counted), an empty line skipped, a line longer than cap - 1 dropped to its LF (cap - 1 fits),
     * and a line carried over from one feed to the next. */
    size_t i = 0;
    while (i < n) {
        const char *lf = memchr(data + i, '\n', n - i);
        const size_t end = lf ? (size_t)(lf - data) : n; /* this feed's part of the line: data[i..end) */
        for (size_t j = i; j < end && !la->discarding;) {
            const char *cr = memchr(data + j, '\r', end - j);
            const size_t stop = cr ? (size_t)(cr - data) : end, run = stop - j;
            const size_t room = la->cap == 0 || la->len >= la->cap - 1 ? 0 : la->cap - 1 - la->len;
            if (run > room) { /* too long: what fits, as before, then the rest dropped to the LF */
                if (room) memcpy(la->buf + la->len, data + j, room);
                la->discarding = true;
                la->len = 0;
                break;
            }
            if (run) memcpy(la->buf + la->len, data + j, run); /* never through a NULL buffer (cap 0) */
            la->len += run;
            j = stop + 1; /* past the CR */
        }
        if (!lf) break;
        if (!la->discarding && la->len > 0) {
            la->buf[la->len] = '\0';
            la->len = 0;
            if (on_line) on_line(ctx, la->buf);
        }
        la->len = 0;
        la->discarding = false;
        i = end + 1;
    }
}
