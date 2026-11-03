#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *buf;
    size_t cap, len;
    bool discarding;
} sat_lineasm_t;

void sat_lineasm_init(sat_lineasm_t *la, char *buf, size_t cap);

/* Splits a TCP byte stream into lines. '\r' is stripped, empty lines are skipped.
 * on_line receives a NUL-terminated, mutable line. A line that would exceed
 * cap-1 characters is discarded up to its terminating '\n'. */
void sat_lineasm_feed(sat_lineasm_t *la, const char *data, size_t n,
                      void (*on_line)(void *ctx, char *line), void *ctx);
