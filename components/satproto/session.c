#include "satproto/session.h"

#include <stdlib.h>
#include <string.h>

#include "satproto/b64.h"
#include "satproto/codec.h"

#define PING_INTERVAL_MS 1000u
#define DEAD_AFTER_MS 5000u
#define MIN_MAJOR 1
#define MIN_MINOR 11
#define MIN_PATCH 0

void sat_session_init(sat_session_t *s, const sat_session_cb_t *cb, void *ctx, char *txbuf, size_t txcap)
{
    memset(s, 0, sizeof(*s));
    s->cb = *cb;
    s->ctx = ctx;
    s->txbuf = txbuf;
    s->txcap = txcap;
}

void sat_session_reset(sat_session_t *s, uint32_t now_ms)
{
    s->state = SAT_S_WAIT_BEGIN;
    memset(&s->caps, 0, sizeof(s->caps));
    s->last_rx_ms = now_ms;
    s->last_ping_ms = now_ms;
}

static void emit(sat_session_t *s, size_t len)
{
    if (s->cb.send) s->cb.send(s->ctx, s->txbuf, len);
}

static bool finish(sat_session_t *s, sat_writer_t *w)
{
    if (!sat_w_end(w)) return false;
    emit(s, w->len);
    return true;
}

/* Strict "N.N.N"; false on anything else. */
static bool parse_version(const char *v, int out[3])
{
    if (!v) return false;
    for (int i = 0; i < 3; i++) {
        if (*v < '0' || *v > '9') return false;
        long n = 0;
        while (*v >= '0' && *v <= '9') {
            n = n * 10 + (*v - '0');
            if (n > 1000000) return false;
            v++;
        }
        out[i] = (int)n;
        if (i < 2) {
            if (*v != '.') return false;
            v++;
        }
    }
    return *v == '\0';
}

static bool version_ok(const int v[3])
{
    if (v[0] != MIN_MAJOR) return v[0] > MIN_MAJOR;
    if (v[1] != MIN_MINOR) return v[1] > MIN_MINOR;
    return v[2] >= MIN_PATCH;
}

static bool truthy(const char *v)
{
    return v && strcmp(v, "0") != 0 && strcmp(v, "false") != 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_color(const char *v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!v || v[0] != '#' || strlen(v) != 7) return false;
    int d[6];
    for (int i = 0; i < 6; i++) {
        d[i] = hexval(v[1 + i]);
        if (d[i] < 0) return false;
    }
    *r = (uint8_t)(d[0] * 16 + d[1]);
    *g = (uint8_t)(d[2] * 16 + d[3]);
    *b = (uint8_t)(d[4] * 16 + d[5]);
    return true;
}

static void handle_begin(sat_session_t *s, const sat_msg_t *m, uint32_t now_ms)
{
    const char *api = sat_msg_get(m, "ApiVersion");
    const char *cv = sat_msg_get(m, "CompanionVersion");
    int v[3] = {0, 0, 0};
    memset(&s->caps, 0, sizeof(s->caps));
    if (cv) {
        strncpy(s->caps.companion_version, cv, sizeof(s->caps.companion_version) - 1);
    }
    if (!parse_version(api, v) || !version_ok(v)) {
        s->state = SAT_S_TOO_OLD;
        if (s->cb.on_too_old) s->cb.on_too_old(s->ctx, api ? api : "");
        return;
    }
    s->caps.major = v[0];
    s->caps.minor = v[1];
    s->caps.patch = v[2];
    s->state = SAT_S_WAIT_CAPS;
    s->last_ping_ms = now_ms;
}

/* list ("rgb,png,webp": comma-separated, spaces around a name allowed) names fmt */
static bool lists_format(const char *list, const char *fmt)
{
    const size_t n = strlen(fmt);
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > p && t[-1] == ' ') t--;
        if ((size_t)(t - p) == n && !strncmp(p, fmt, n)) return true;
        p = e;
    }
    return false;
}

static void handle_caps(sat_session_t *s, const sat_msg_t *m)
{
    s->caps.nonsquare = truthy(sat_msg_get(m, "NONSQUARE"));
    s->caps.rotary_amount = truthy(sat_msg_get(m, "ROTARY_AMOUNT"));
    s->caps.leds = truthy(sat_msg_get(m, "LEDS"));
    s->caps.png = lists_format(sat_msg_get(m, "BITMAP_FORMATS"), "png");
    s->state = SAT_S_READY;
    if (s->cb.on_ready) s->cb.on_ready(s->ctx, &s->caps);
}

static void handle_key_state(sat_session_t *s, const sat_msg_t *m)
{
    sat_key_state_t ks;
    memset(&ks, 0, sizeof(ks));
    ks.device_id = sat_msg_get(m, "DEVICEID");
    ks.control_id = sat_msg_get(m, "CONTROLID");
    if (!ks.control_id) ks.control_id = sat_msg_get(m, "KEY");
    if (!ks.device_id || !ks.control_id || !ks.device_id[0] || !ks.control_id[0]) return;
    const char *bmp = sat_msg_get(m, "BITMAP");
    if (bmp && bmp[0]) {
        ks.bitmap_b64 = bmp;
        ks.bitmap_b64_len = strlen(bmp);
    }
    ks.has_color = parse_color(sat_msg_get(m, "COLOR"), &ks.r, &ks.g, &ks.b);
    ks.pressed = truthy(sat_msg_get(m, "PRESSED"));
    if (s->cb.on_key_state) s->cb.on_key_state(s->ctx, &ks);
}

static void handle_add_result(sat_session_t *s, const sat_msg_t *m, const char *status)
{
    const char *id = sat_msg_get(m, "DEVICEID");
    if (!id) return;
    bool ok = strcmp(status, "OK") == 0;
    if (!ok && strcmp(status, "ERROR") != 0) return;
    if (s->cb.on_add_result) s->cb.on_add_result(s->ctx, id, ok, ok ? NULL : sat_msg_get(m, "MESSAGE"));
}

static void handle_ping(sat_session_t *s, const char *payload)
{
    size_t n = strlen(payload);
    if (n + 7 > s->txcap) return; /* "PONG " + payload + "\n" + NUL */
    memcpy(s->txbuf, "PONG ", 5);
    memcpy(s->txbuf + 5, payload, n);
    s->txbuf[5 + n] = '\n';
    s->txbuf[6 + n] = '\0';
    emit(s, n + 6);
}

void sat_session_on_line(sat_session_t *s, char *line, uint32_t now_ms)
{
    s->last_rx_ms = now_ms;

    /* PING is answered from the raw line, before parsing mutates it. */
    if (strncmp(line, "PING", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
        handle_ping(s, line[4] == ' ' ? line + 5 : "");
        return;
    }

    sat_msg_t m;
    if (!sat_parse_line(line, &m)) return;

    if (strcmp(m.cmd, "BEGIN") == 0) {
        handle_begin(s, &m, now_ms);
        return;
    }
    if (s->state == SAT_S_TOO_OLD) return;
    if (strcmp(m.cmd, "CAPS") == 0) {
        if (s->state == SAT_S_WAIT_CAPS) handle_caps(s, &m);
    } else if (strcmp(m.cmd, "KEY-STATE") == 0) {
        handle_key_state(s, &m);
    } else if (strcmp(m.cmd, "BRIGHTNESS") == 0) {
        const char *id = sat_msg_get(&m, "DEVICEID");
        const char *val = sat_msg_get(&m, "VALUE");
        if (id && val && val[0] && s->cb.on_brightness) {
            long pct = strtol(val, NULL, 10);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            s->cb.on_brightness(s->ctx, id, (int)pct);
        }
    } else if (strcmp(m.cmd, "KEYS-CLEAR") == 0) {
        const char *id = sat_msg_get(&m, "DEVICEID");
        if (id && s->cb.on_clear) s->cb.on_clear(s->ctx, id);
    } else if (strcmp(m.cmd, "ADD-DEVICE") == 0) {
        /* status is the first valueless arg: OK / ERROR */
        if (m.argc > 0 && (!m.args[0].val || !m.args[0].val[0])) {
            handle_add_result(s, &m, m.args[0].key);
        }
    }
}

void sat_session_tick(sat_session_t *s, uint32_t now_ms)
{
    if (s->state != SAT_S_WAIT_CAPS && s->state != SAT_S_READY) return;
    if ((uint32_t)(now_ms - s->last_ping_ms) < PING_INTERVAL_MS) return;
    s->last_ping_ms = now_ms;
    if (s->txcap < 7) return;
    memcpy(s->txbuf, "PING \n", 7);
    emit(s, 6);
}

bool sat_session_alive(const sat_session_t *s, uint32_t now_ms)
{
    return (uint32_t)(now_ms - s->last_rx_ms) < DEAD_AFTER_MS;
}

bool sat_session_add_device(sat_session_t *s, const sat_device_desc_t *d)
{
    if (!d || !d->device_id || !d->product_name || !d->manifest_json) return false;
    size_t mlen = strlen(d->manifest_json);
    size_t blen = 4 * ((mlen + 2) / 3);
    if (blen + 1 >= s->txcap) return false;
    /* Base64 manifest lives at the tail of txbuf; the line is built in front of it. */
    char *b64 = s->txbuf + s->txcap - (blen + 1);
    if (mlen > 0 && sat_b64_encode((const uint8_t *)d->manifest_json, mlen, b64, blen + 1) != blen) return false;
    if (mlen == 0) b64[0] = '\0';
    sat_writer_t w;
    sat_w_begin(&w, s->txbuf, s->txcap - (blen + 1), "ADD-DEVICE", NULL, d->device_id);
    sat_w_str(&w, "PRODUCT_NAME", d->product_name);
    sat_w_str(&w, "LAYOUT_MANIFEST", b64);
    sat_w_bool(&w, "BRIGHTNESS", d->brightness);
    sat_w_str(&w, "SERIAL", d->device_id);
    sat_w_bool(&w, "SERIAL_IS_UNIQUE", true);
    if (s->caps.png) sat_w_str(&w, "BITMAP_FORMAT", "png"); /* smaller than raw RGB: several times less to receive */
    if (d->change_page_label) sat_w_str(&w, "CAN_CHANGE_PAGE", d->change_page_label);
    return finish(s, &w);
}

void sat_session_remove_device(sat_session_t *s, const char *device_id)
{
    sat_writer_t w;
    sat_w_begin(&w, s->txbuf, s->txcap, "REMOVE-DEVICE", NULL, device_id);
    finish(s, &w);
}

void sat_session_key(sat_session_t *s, const char *device_id, const char *control_id, bool pressed)
{
    sat_writer_t w;
    sat_w_begin(&w, s->txbuf, s->txcap, "KEY-PRESS", NULL, device_id);
    sat_w_str(&w, "CONTROLID", control_id);
    sat_w_str(&w, "KEY", control_id);
    sat_w_bool(&w, "PRESSED", pressed);
    finish(s, &w);
}

#define MAX_ROTATION_STEPS 100 /* as the reference client: what Companion accepts for one turn */

/* A KEY-ROTATE line at txbuf + at; its length, or 0 when it doesn't fit there (or is invalid). */
static size_t rotate_line_at(sat_session_t *s, size_t at, const char *device_id, const char *control_id, int direction)
{
    sat_writer_t w;
    sat_w_begin(&w, s->txbuf + at, s->txcap - at, "KEY-ROTATE", NULL, device_id);
    sat_w_str(&w, "CONTROLID", control_id);
    sat_w_str(&w, "KEY", control_id);
    sat_w_int(&w, "DIRECTION", direction);
    return sat_w_end(&w) ? w.len : 0;
}

static void rotate_line(sat_session_t *s, const char *device_id, const char *control_id, int direction)
{
    const size_t n = rotate_line_at(s, 0, device_id, control_id, direction);
    if (n) emit(s, n);
}

void sat_session_rotate(sat_session_t *s, const char *device_id, const char *control_id, int delta)
{
    if (delta == 0) return;
    const int sign = delta > 0 ? 1 : -1;
    int steps = delta > 0 ? delta : -delta;
    if (steps > MAX_ROTATION_STEPS) steps = MAX_ROTATION_STEPS;
    if (s->caps.rotary_amount) {
        rotate_line(s, device_id, control_id, sign * steps);
        return;
    }
    /* no step lost. the steps' lines back to back in txbuf and sent together (flushed first when the next
     * one doesn't fit): the same bytes in the same order as one send a line, in one send() (TCP segment), not n */
    size_t used = 0;
    for (int i = 0; i < steps; i++) {
        size_t n = rotate_line_at(s, used, device_id, control_id, sign);
        if (!n && used) { /* txbuf full: what's there out, then this line at its start */
            emit(s, used);
            used = 0;
            n = rotate_line_at(s, 0, device_id, control_id, sign);
        }
        used += n; /* 0: a line that can't be written at all (nor could it be before) */
    }
    if (used) emit(s, used);
}

void sat_session_change_page(sat_session_t *s, const char *device_id, bool forward)
{
    sat_writer_t w;
    sat_w_begin(&w, s->txbuf, s->txcap, "CHANGE-PAGE", NULL, device_id);
    sat_w_bool(&w, "DIRECTION", forward);
    finish(s, &w);
}
