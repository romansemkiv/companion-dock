#pragma once
/* The Satellite client's wait rule (pure: no ESP-IDF header; host-tested in
 * test/host/test_satclient_logic.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
