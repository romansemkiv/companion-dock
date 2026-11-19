#pragma once
/* The Satellite client's wait rule (pure: no ESP-IDF header; host-tested in
 * test/host/test_satclient_logic.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "satclient/satclient_event.h"

/* How long the connected loop may wait in select() for Companion's data: 0 while work for a ready
 * session is queued (a key press must go out now), otherwise idle_ms. A post() also writes the
 * loop's eventfd, which ends a wait already under way; this covers work queued before it began. */
uint32_t satc_wait_ms(unsigned queued, bool ready, uint32_t idle_ms);

/* A blocking write of data[0..len) over a non-blocking socket (sess_send): send() takes what it can (> 0 bytes), or
 * says it would block (again) or failed (neither); while it would block, wait() waits for room, up to step_ms. The
 * budget_ms counts the waits since the last line end ('\n') went out: each line of a batch gets its own
 * budget, as when every line was its own write, so a reader that takes whole lines in time never
 * times out, and one that trickles a few bytes now and then times out on a line as before. Returns the bytes written
 * (len: all). */
typedef struct {
    long (*send)(void *ctx, const char *p, size_t n, bool *again);
    void (*wait)(void *ctx);
    void *ctx;
} satc_io_t;
size_t satc_write_all(const satc_io_t *io, const char *data, size_t len, int budget_ms, int step_ms);

/* What a connection attempt that has ended tells the observer: SATC_EV_DROPPED when the session had been ready,
 * SATC_EV_ATTEMPT_FAILED otherwise (the lookup or connect failed, or the connection ended before READY), or -1
 * (nothing) for a deliberate change of target or a too-old Companion. `connected`: a TCP connection was made. */
int satc_end_event(bool connected, bool got_ready, bool target_changed, bool too_old);

/* The wait before the next connection attempt, after one that failed or ended: SATC_BACKOFF_FIRST_MS, doubled each
 * time up to 2 s, then 5 s. A wait cut short by a change of target (target_changed) starts again from
 * SATC_BACKOFF_FIRST_MS, so the new target gets quick attempts too, not one per 5 s. */
#define SATC_BACKOFF_FIRST_MS 500u
uint32_t satc_next_backoff(uint32_t backoff, bool target_changed);

/* Connection attempts to one target in a row that failed: the first is logged, the rest counted, the count logged when the
 * streak ends (a connection made, or another target). key: "host:port". A zeroed struct has no streak. */
typedef struct { char key[72]; uint32_t n; } satc_streak_t;
enum { SATC_LOG_FIRST, SATC_LOG_QUIET, SATC_LOG_NONE };
/* failed: this attempt failed. Returns SATC_LOG_FIRST (log this failure as today), SATC_LOG_QUIET (say nothing) or
 * SATC_LOG_NONE (a success: nothing to log for it). *ended (may be NULL): the failures of a streak that just ended (by a
 * success or another key), 0 none; the caller logs "... failed N times in a row" when it is more than 1. A key longer
 * than key[] holds is cut, and compared cut. */
int satc_streak_step(satc_streak_t *s, const char *key, bool failed, uint32_t *ended);
