#pragma once
#include <stdbool.h>
#include <stddef.h>

#define SAT_MAX_ARGS 40

typedef struct {
    const char *key;
    const char *val; /* NULL = valueless flag */
} sat_arg_t;

typedef struct {
    const char *cmd;
    sat_arg_t args[SAT_MAX_ARGS];
    int argc;
} sat_msg_t;

/* Parses in place (unescapes into the same buffer). Returns arg count.
 * Drops empty keys and prototype-polluting keys; args beyond max are dropped. */
int sat_parse_params(char *params, sat_arg_t *args, int max);

/* First space-delimited token is cmd, the rest are params. False for an empty line.
 * `line` must have no trailing newline and is modified in place. */
bool sat_parse_line(char *line, sat_msg_t *out);

/* Value ("" for a flag), or NULL if absent. */
const char *sat_msg_get(const sat_msg_t *m, const char *key);

typedef struct {
    char *buf;
    size_t cap, len;
    bool err;
} sat_writer_t;

/* status and device_id may be NULL (empty device_id is treated as absent). */
void sat_w_begin(sat_writer_t *w, char *buf, size_t cap, const char *cmd, const char *status, const char *device_id);
void sat_w_str(sat_writer_t *w, const char *key, const char *val);
void sat_w_int(sat_writer_t *w, const char *key, long v);
void sat_w_bool(sat_writer_t *w, const char *key, bool v);
/* Appends " \n" and NUL. False if any key/value was invalid or the buffer overflowed. */
bool sat_w_end(sat_writer_t *w);
