/* The recovery slot's decisions (slotsync_logic.h). Pure: also built by the host tests. */
#include "slotsync_logic.h"

#include <string.h>

const char *slotsync_state_name(slotsync_state_t s)
{
    switch (s) {
    case SLOTSYNC_IN_SYNC: return "in_sync";
    case SLOTSYNC_SYNCING: return "syncing";
    case SLOTSYNC_DIFFERS: return "differs";
    case SLOTSYNC_OTHER_NEWER: return "other_newer";
    case SLOTSYNC_EMPTY: return "empty";
    case SLOTSYNC_INVALID: return "invalid";
    default: return "waiting";
    }
}

/* A copy is due but must wait. */
static bool held(const ss_in_t *in)
{
    return in->other == SS_OTHER_UNKNOWN || in->pending || in->flash_busy || in->uptime_us < SS_DELAY_US ||
           (in->fail_us >= 0 && in->uptime_us - in->fail_us < SS_RETRY_US);
}

/* The reason a copy can't start now, whoever asks (SS_GO: it can). */
static ss_why_t blocked(const ss_in_t *in)
{
    if (in->copying) return SS_E_SYNCING;
    if (in->flash_busy) return SS_E_FLASH_BUSY;
    if (in->pending) return SS_E_PENDING;
    if (in->other == SS_OTHER_UNKNOWN) return SS_E_NOT_READY;
    return SS_GO;
}

/* What the other slot holds, when a copy is due. */
static slotsync_state_t base_state(ss_other_t other)
{
    return other == SS_OTHER_EMPTY ? SLOTSYNC_EMPTY
           : other == SS_OTHER_INVALID || other == SS_OTHER_FOREIGN ? SLOTSYNC_INVALID
                                                                       : SLOTSYNC_DIFFERS;
}

ss_out_t ss_decide(const ss_in_t *in, ss_req_t req)
{
    ss_out_t o = {.state = SLOTSYNC_WAITING, .copy = false, .why = SS_NOT_DUE};
    const bool valid = in->other == SS_OTHER_VALID;
    if (in->copying) o.state = SLOTSYNC_SYNCING;
    else if (valid && in->same) o.state = SLOTSYNC_IN_SYNC;
    else if (valid && in->newer) o.state = SLOTSYNC_OTHER_NEWER;
    else if (in->other == SS_OTHER_UNKNOWN || (!in->no_source && held(in))) o.state = SLOTSYNC_WAITING;
    else o.state = base_state(in->other); /* without a source nothing is held: no copy will come */

    if (req == SS_AUTO) {
        /* due: empty, invalid or differs, and not held; once per boot */
        const bool due = o.state == SLOTSYNC_EMPTY || o.state == SLOTSYNC_INVALID || o.state == SLOTSYNC_DIFFERS;
        if (due && !in->done && !in->no_source) o.why = SS_GO;
    } else {
        o.why = blocked(in);
        if (o.why == SS_GO && in->no_source) o.why = SS_E_NO_SOURCE;
        if (o.why == SS_GO && valid && in->newer && !in->same && req != SS_FORCE) o.why = SS_E_NEWER;
    }
    o.copy = o.why == SS_GO;
    return o;
}

ss_why_t ss_boot_other(const ss_in_t *in)
{
    const ss_why_t why = blocked(in);
    if (why != SS_GO) return why;
    if (in->other == SS_OTHER_EMPTY) return SS_E_EMPTY;
    if (in->other == SS_OTHER_INVALID) return SS_E_INVALID;
    if (in->other == SS_OTHER_FOREIGN) return SS_E_FOREIGN;
    return SS_GO;
}

/* n decimal digits at s (leading spaces allowed when lead_space); -1 if not. */
static int digits(const char *s, int n, bool lead_space)
{
    int v = 0;
    bool seen = false;
    for (int i = 0; i < n; i++) {
        const char c = s[i];
        if (c == ' ' && lead_space && !seen && i < n - 1) continue;
        if (c < '0' || c > '9') return -1;
        seen = true;
        v = v * 10 + (c - '0');
    }
    return v;
}

int64_t ss_build_stamp(const char *date, const char *time)
{
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    /* "Mmm dd yyyy" (the day padded with a space) and "hh:mm:ss", exactly */
    if (!date || !time || strlen(date) != 11 || strlen(time) != 8) return -1;
    if (date[3] != ' ' || date[6] != ' ' || time[2] != ':' || time[5] != ':') return -1;
    int mon = 0;
    while (mon < 12 && strncmp(MONTHS + 3 * mon, date, 3) != 0) mon++;
    const int day = digits(date + 4, 2, true), year = digits(date + 7, 4, false);
    const int hh = digits(time, 2, false), mm = digits(time + 3, 2, false), ss = digits(time + 6, 2, false);
    if (mon == 12 || day < 1 || day > 31 || year < 0 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59)
        return -1;
    return ((((int64_t)year * 100 + mon + 1) * 100 + day) * 100 + hh) * 10000 + mm * 100 + ss;
}

bool ss_counts_newer(const char *other_date, const char *other_time, const char *run_date, const char *run_time)
{
    const int64_t o = ss_build_stamp(other_date, other_time), r = ss_build_stamp(run_date, run_time);
    return o < 0 || r < 0 || o >= r;
}
