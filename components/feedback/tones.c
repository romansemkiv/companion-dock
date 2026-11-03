/* Sounds as text ("2000:80 0:40 3000:150"): see tones.h. */
#include "tones.h"

#include <stddef.h>

static int fail(const char **err, const char *why)
{
    if (err) *err = why;
    return 0;
}

/* a decimal number of up to 5 digits; false if there is none or it is longer */
static int number(const char **p, long *v)
{
    long n = 0;
    int digits = 0;
    while (**p >= '0' && **p <= '9') {
        if (++digits > 5) return 0;
        n = n * 10 + (**p - '0');
        (*p)++;
    }
    *v = n;
    return digits > 0;
}

static int space(char c) { return c == ' ' || c == '\t'; }

int tones_parse(const char *s, tone_t out[TONES_MAX], const char **err)
{
    if (!s) return fail(err, "tones must be text like \"2000:80 0:40 3000:150\"");
    const char *p = s;
    int n = 0;
    long total = 0;
    for (;;) {
        while (space(*p)) p++;
        if (!*p) break;
        if (n && *p == ',') { /* a comma between steps, once */
            p++;
            while (space(*p)) p++;
            if (!*p || *p == ',') return fail(err, "an empty step between commas");
        }
        long hz, ms;
        if (!number(&p, &hz) || *p++ != ':' || !number(&p, &ms))
            return fail(err, "each step is freq:ms, e.g. 2000:80 (0 Hz is a pause)");
        if (*p && !space(*p) && *p != ',') return fail(err, "each step is freq:ms, e.g. 2000:80 (0 Hz is a pause)");
        if (hz && (hz < TONES_MIN_HZ || hz > TONES_MAX_HZ)) return fail(err, "freq must be 0 (pause) or 100..10000 Hz");
        if (ms < TONES_MIN_MS || ms > TONES_MAX_MS) return fail(err, "each step must be 10..2000 ms");
        if (n == TONES_MAX) return fail(err, "at most 16 steps");
        total += ms;
        if (total > TONES_TOTAL_MS) return fail(err, "the whole sound must be at most 10 s");
        out[n].hz = (uint16_t)hz;
        out[n].ms = (uint16_t)ms;
        n++;
    }
    if (!n) return fail(err, "tones must have at least one freq:ms step");
    return n;
}
