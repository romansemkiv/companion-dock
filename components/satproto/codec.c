#include "satproto/codec.h"

#include <stdio.h>
#include <string.h>

static const char *const BANNED[] = {
    "__proto__", "constructor", "prototype", "__defineGetter__",
    "__defineSetter__", "__lookupGetter__", "__lookupSetter__",
};

static bool is_banned(const char *k)
{
    for (size_t i = 0; i < sizeof BANNED / sizeof BANNED[0]; i++) {
        if (strcmp(k, BANNED[i]) == 0) return true;
    }
    return false;
}

int sat_parse_params(char *params, sat_arg_t *args, int max)
{
    int argc = 0;
    char *r = params;
    char *w = params;
    bool quoted = false;
    char *frag = params;  /* start of current fragment in output */
    char *eq = NULL;      /* first '=' written in current fragment */

    for (;;) {
        char c = *r;
        bool end = (c == '\0');
        if (end || (!quoted && c == ' ')) {
            /* finish fragment; r is consumed so w <= r and writing *w is safe */
            *w = '\0';
            if (w != frag) {
                const char *key = frag;
                const char *val = NULL;
                if (eq) {
                    *eq = '\0';
                    val = eq + 1;
                }
                if (key[0] != '\0' && !is_banned(key) && argc < max) {
                    args[argc].key = key;
                    args[argc].val = val;
                    argc++;
                }
            }
            if (end) break;
            r++;
            frag = w + 1;
            w = frag;
            eq = NULL;
            continue;
        }
        r++;
        if (c == '\\') {
            if (*r == '\0') continue; /* dangling backslash dropped */
            c = *r++;
        } else if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c == '=' && !eq) eq = w;
        if (w != r - 1) *w = c; /* w == r - 1 (nothing compacted yet): the byte is c already */
        w++;
    }
    return argc;
}

bool sat_parse_line(char *line, sat_msg_t *out)
{
    out->cmd = NULL;
    out->argc = 0;
    if (!line || line[0] == '\0') return false;
    char *sp = strchr(line, ' ');
    char *rest;
    if (sp) {
        *sp = '\0';
        rest = sp + 1;
    } else {
        rest = line + strlen(line); /* empty, writable */
    }
    if (line[0] == '\0') return false;
    out->cmd = line;
    out->argc = sat_parse_params(rest, out->args, SAT_MAX_ARGS);
    return true;
}

const char *sat_msg_get(const sat_msg_t *m, const char *key)
{
    for (int i = 0; i < m->argc; i++) {
        if (strcmp(m->args[i].key, key) == 0) return m->args[i].val ? m->args[i].val : "";
    }
    return NULL;
}

/* ---- writer ---- */

static void put(sat_writer_t *w, const char *s, size_t n)
{
    if (w->err) return;
    if (w->len + n + 1 > w->cap) { /* keep room for NUL */
        w->err = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void put_c(sat_writer_t *w, char c) { put(w, &c, 1); }

static void put_quoted(sat_writer_t *w, const char *v)
{
    put_c(w, '"');
    for (; *v; v++) {
        if (*v == '\n' || *v == '\r') {
            w->err = true;
            return;
        }
        if (*v == '"' || *v == '\\') put_c(w, '\\');
        put_c(w, *v);
    }
    put_c(w, '"');
}

static void put_key(sat_writer_t *w, const char *key)
{
    if (!key || key[0] == '\0') {
        w->err = true;
        return;
    }
    for (const char *p = key; *p; p++) {
        if (*p == ' ' || *p == '=' || *p == '"' || *p == '\n' || *p == '\r') {
            w->err = true;
            return;
        }
    }
    put_c(w, ' ');
    put(w, key, strlen(key));
    put_c(w, '=');
}

void sat_w_begin(sat_writer_t *w, char *buf, size_t cap, const char *cmd, const char *status, const char *device_id)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = (cap == 0 || !cmd || cmd[0] == '\0');
    if (w->err) return;
    put(w, cmd, strlen(cmd));
    if (status && status[0]) {
        put_c(w, ' ');
        put(w, status, strlen(status));
    }
    if (device_id && device_id[0]) {
        put(w, " DEVICEID=", 10);
        put_quoted(w, device_id);
    }
}

void sat_w_str(sat_writer_t *w, const char *key, const char *val)
{
    put_key(w, key);
    if (!val) {
        w->err = true;
        return;
    }
    put_quoted(w, val);
}

void sat_w_int(sat_writer_t *w, const char *key, long v)
{
    put_key(w, key);
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%ld", v);
    put(w, tmp, (size_t)n);
}

void sat_w_bool(sat_writer_t *w, const char *key, bool v)
{
    put_key(w, key);
    put_c(w, v ? '1' : '0');
}

bool sat_w_end(sat_writer_t *w)
{
    put(w, " \n", 2);
    if (w->cap > 0) w->buf[w->err ? 0 : w->len] = '\0';
    return !w->err;
}
