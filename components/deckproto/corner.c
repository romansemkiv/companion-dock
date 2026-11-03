/* Pure: the gesture that opens the maintenance screen on demand (corner.h; the machine, armed, as configured). */
#include "deckproto/corner.h"

#include <stdio.h>
#include <string.h>

corner_cfg_t corner_cfg_default(void)
{
    return (corner_cfg_t){.on = true, .first = CORNER_TOP_RIGHT, .second = CORNER_BOTTOM_RIGHT, .taps = 1,
                          .hold_ms = CORNER_HOLD_DEFAULT_MS};
}

bool corner_cfg_ok(const corner_cfg_t *cfg)
{
    return cfg && cfg->first <= CORNER_BOTTOM_RIGHT && cfg->second <= CORNER_BOTTOM_RIGHT && cfg->first != cfg->second &&
           cfg->taps >= 1 && cfg->taps <= CORNER_TAPS_MAX && cfg->hold_ms > 0;
}

corner_cfg_t corner_cfg_use(const corner_cfg_t *cfg) { return corner_cfg_ok(cfg) ? *cfg : corner_cfg_default(); }

bool corner_cfg_eq(const corner_cfg_t *a, const corner_cfg_t *b)
{
    const corner_cfg_t x = corner_cfg_use(a), y = corner_cfg_use(b);
    return x.on == y.on && x.first == y.first && x.second == y.second && x.taps == y.taps && x.hold_ms == y.hold_ms;
}

void corner_cfg_describe(const corner_cfg_t *cfg, char *out, size_t n)
{
    static const char *const NAME[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
    static const char *const TIMES[] = {"", " twice", " 3 times"};
    if (!out || !n) return;
    const corner_cfg_t g = corner_cfg_use(cfg);
    if (!g.on) {
        snprintf(out, n, "off");
        return;
    }
    const char *unit = g.hold_ms % 1000 ? "ms" : "s";
    const unsigned hold = g.hold_ms % 1000 ? g.hold_ms : g.hold_ms / 1000u;
    snprintf(out, n, "tap %s%s, then hold %s + %s %u %s", NAME[g.first], TIMES[g.taps - 1], NAME[g.first],
             NAME[g.second], hold, unit);
}

/* Corner k's report index on model m: the left or right column (logical column cols - 1 is index cols - 1 of its
 * row, or 0 on a deck that reports each row right to left, the Original), the top or bottom row. */
static uint8_t corner_hw(const deck_model_t *m, int k)
{
    const bool right = k == CORNER_TOP_RIGHT || k == CORNER_BOTTOM_RIGHT;
    const bool bottom = k == CORNER_BOTTOM_LEFT || k == CORNER_BOTTOM_RIGHT;
    const int col = right ? m->cols - 1 : 0;
    return (uint8_t)((bottom ? m->rows - 1 : 0) * m->cols + (m->rtl ? m->cols - 1 - col : col));
}

bool corner_keys(const deck_model_t *m, const corner_cfg_t *cfg, uint8_t hw[2])
{
    const corner_cfg_t g = corner_cfg_use(cfg);
    if (!g.on || !m || m->pedal || m->fmt == DECK_IMG_NONE || m->cols < 1 || m->rows < 2) return false;
    hw[CORNER_FIRST] = corner_hw(m, g.first);
    hw[CORNER_SECOND] = corner_hw(m, g.second);
    return true;
}

int corner_which(const uint8_t hw[2], uint8_t idx)
{
    return idx == hw[CORNER_FIRST] ? CORNER_FIRST : idx == hw[CORNER_SECOND] ? CORNER_SECOND : -1;
}

void corner_init(corner_t *c, const corner_cfg_t *cfg)
{
    const corner_cfg_t g = corner_cfg_use(cfg); /* first: cfg may be c's own */
    memset(c, 0, sizeof *c);
    c->cfg = g;
}

/* armed at now: FIRST tapped cfg.taps times in a row, the last tap's window not over. */
static bool armed(const corner_t *c, int64_t now)
{
    return c->ntaps >= c->cfg.taps && c->armed_until && now < c->armed_until;
}

static void emit(corner_out_t *o, int which, bool down)
{
    if (o->n >= sizeof o->ev / sizeof o->ev[0]) return;
    o->ev[o->n].which = (uint8_t)which;
    o->ev[o->n].down = down;
    o->n++;
}

static void advance(corner_t *c, int64_t now, corner_out_t *o)
{
    for (int i = 0; i < 2; i++)
        if (c->st[i] == CORNER_WAIT && now - c->t[i] >= CORNER_WINDOW_MS) { /* a lone press: on, late */
            c->st[i] = CORNER_PASS;
            emit(o, i, true);
        }
    if (c->chord && !c->broken && !c->fired && c->st[0] == CORNER_EATEN && c->st[1] == CORNER_EATEN &&
        now - c->chord_t >= c->cfg.hold_ms) {
        c->fired = true;
        o->open = true;
    }
}

void corner_tick(corner_t *c, int64_t now, corner_out_t *o)
{
    memset(o, 0, sizeof *o);
    advance(c, now, o);
}

void corner_key(corner_t *c, int which, bool down, int64_t now, corner_out_t *o)
{
    memset(o, 0, sizeof *o);
    if (which != CORNER_FIRST && which != CORNER_SECOND) return;
    advance(c, now, o);
    const int other = 1 - which;
    if (down) {
        if (c->st[which] != CORNER_UP) return; /* already down */
        if (c->chord) { /* pressed again before both were up: swallowed */
            c->st[which] = CORNER_EATEN;
            return;
        }
        if (c->st[other] == CORNER_WAIT) { /* the chord: neither key-down is sent */
            c->st[which] = c->st[other] = CORNER_EATEN;
            c->chord = true;
            c->broken = c->fired = false;
            c->chord_t = now;
            c->armed_until = 0; /* used */
            c->ntaps = 0;
            return;
        }
        if (!armed(c, now)) { /* not armed: never delayed */
            if (which == CORNER_SECOND) c->ntaps = 0; /* between the taps: they start again */
            c->st[which] = CORNER_PASS;
            emit(o, which, true);
            return;
        }
        c->st[which] = CORNER_WAIT; /* armed: held back for the chord */
        c->t[which] = now;
        return;
    }
    const bool tapped = which == CORNER_FIRST && (c->st[which] == CORNER_WAIT || c->st[which] == CORNER_PASS);
    switch (c->st[which]) {
    case CORNER_WAIT: /* let go inside the window: the whole press now */
        emit(o, which, true);
        emit(o, which, false);
        break;
    case CORNER_EATEN:
        if (!c->fired) c->broken = true; /* let go before the hold: nothing opens */
        break;
    default: /* passed on, or a press the gate never saw: its release goes on */
        emit(o, which, false);
        break;
    }
    c->st[which] = CORNER_UP;
    if (c->chord && c->st[other] == CORNER_UP) c->chord = c->broken = c->fired = false; /* both up: over */
    if (tapped && !c->chord) { /* a FIRST tap (it reached Companion) arms; the taps in a row counted */
        const bool in_row = c->armed_until && now < c->armed_until;
        c->ntaps = in_row ? (uint8_t)(c->ntaps < CORNER_TAPS_MAX ? c->ntaps + 1 : CORNER_TAPS_MAX) : 1;
        c->armed_until = now + CORNER_ARM_MS;
    }
}

int64_t corner_due(const corner_t *c)
{
    int64_t due = 0;
    for (int i = 0; i < 2; i++)
        if (c->st[i] == CORNER_WAIT && (!due || c->t[i] + CORNER_WINDOW_MS < due)) due = c->t[i] + CORNER_WINDOW_MS;
    if (c->chord && !c->broken && !c->fired && (!due || c->chord_t + c->cfg.hold_ms < due))
        due = c->chord_t + c->cfg.hold_ms;
    return due;
}

bool corner_held(const corner_t *c, int which)
{
    return (which == CORNER_FIRST || which == CORNER_SECOND) && (c->st[which] == CORNER_WAIT || c->st[which] == CORNER_EATEN);
}

/* ---- Satellite ---- */

static int sat_out(const corner_out_t *co, const uint8_t hw[2], deck_event_t out[3])
{
    int n = 0;
    for (int i = 0; i < co->n && n < 3; i++)
        out[n++] = (deck_event_t){.kind = DECK_EV_KEY, .index = hw[co->ev[i].which], .pressed = co->ev[i].down};
    return n;
}

int corner_sat_event(corner_t *c, bool has, const uint8_t hw[2], const deck_event_t *ev, int64_t now,
                     deck_event_t out[3], bool *open)
{
    *open = false;
    const int w = has && ev->kind == DECK_EV_KEY ? corner_which(hw, ev->index) : -1;
    if (w < 0) {
        out[0] = *ev;
        return 1;
    }
    corner_out_t co;
    corner_key(c, w, ev->pressed, now, &co);
    *open = co.open;
    return sat_out(&co, hw, out);
}

int corner_sat_tick(corner_t *c, bool has, const uint8_t hw[2], int64_t now, deck_event_t out[3], bool *open)
{
    *open = false;
    if (!has) return 0;
    corner_out_t co;
    corner_tick(c, now, &co);
    *open = co.open;
    return sat_out(&co, hw, out);
}

/* ---- the dock-mode gate ---- */

void corner_gate_init(corner_gate_t *g, const deck_model_t *m, const corner_cfg_t *cfg)
{
    const corner_cfg_t use = corner_cfg_use(cfg); /* first: cfg may be g's own */
    memset(g, 0, sizeof *g);
    g->on = corner_keys(m, &use, g->hw);
    g->gen2 = m && m->gen == DECK_GEN2;
    corner_init(&g->c, &use);
}

void corner_gate_reset(corner_gate_t *g)
{
    corner_init(&g->c, &g->c.cfg);
    g->last_len = 0;
}

/* The key report's header length (its key states follow it); 0: not a key report. */
static size_t key_hdr(const corner_gate_t *g, const uint8_t *r, size_t n)
{
    if (n < 2 || r[0] != 0x01) return 0;
    if (!g->gen2) return 1;
    return n >= 5 && r[1] == 0x00 ? 4 : 0;
}

static void push(corner_gate_out_t *o, const uint8_t *r, size_t n)
{
    if (o->n >= 3) return;
    memcpy(o->r[o->n], r, n);
    o->len[o->n] = (uint8_t)n;
    o->n++;
}

/* r with the held-back corners' bytes cleared. */
static void push_masked(const corner_gate_t *g, corner_gate_out_t *o, const uint8_t *r, size_t n)
{
    const size_t hdr = key_hdr(g, r, n);
    if (o->n >= 3) return;
    push(o, r, n);
    if (!hdr) return;
    for (int i = 0; i < 2; i++)
        if (corner_held(&g->c, i) && hdr + g->hw[i] < n) o->r[o->n - 1][hdr + g->hw[i]] = 0;
}

static bool corner_down(const corner_gate_t *g, const uint8_t *r, size_t n, int which)
{
    const size_t hdr = key_hdr(g, r, n);
    return hdr && hdr + g->hw[which] < n && r[hdr + g->hw[which]] != 0;
}

void corner_gate_report(corner_gate_t *g, const uint8_t *r, size_t len, int64_t now, corner_gate_out_t *o)
{
    memset(o, 0, sizeof *o);
    const size_t n = len < CORNER_REPORT_MAX ? len : CORNER_REPORT_MAX;
    if (!g->on) {
        push(o, r, n);
        return;
    }
    corner_out_t co;
    corner_tick(&g->c, now, &co); /* a window that ended first: its press before this report */
    o->open |= co.open;
    o->pressed |= co.n > 0;
    if (co.n && g->last_len) push_masked(g, o, g->last, g->last_len);
    if (!key_hdr(g, r, n)) {
        push(o, r, n);
        return;
    }
    bool late_press = false;
    for (int i = 0; i < 2; i++) {
        const bool was = g->last_len && corner_down(g, g->last, g->last_len, i), is = corner_down(g, r, n, i);
        if (was == is) continue;
        corner_key(&g->c, i, is, now, &co);
        o->open |= co.open;
        for (int k = 0; k < co.n; k++) { /* a press now with this report's key-down is not late (unarmed) */
            const bool now_down = co.ev[k].which == i && is;
            late_press |= co.ev[k].down && !now_down;
            o->pressed |= co.ev[k].down;
        }
    }
    /* a press passed on now (its window over, or let go inside it): the report before, with it shown */
    if (late_press && g->last_len) push_masked(g, o, g->last, g->last_len);
    memcpy(g->last, r, n);
    g->last_len = (uint8_t)n;
    push_masked(g, o, r, n);
}

void corner_gate_tick(corner_gate_t *g, int64_t now, corner_gate_out_t *o)
{
    memset(o, 0, sizeof *o);
    if (!g->on) return;
    corner_out_t co;
    corner_tick(&g->c, now, &co);
    o->open = co.open;
    o->pressed = co.n > 0;
    if (co.n && g->last_len) push_masked(g, o, g->last, g->last_len);
}

int64_t corner_gate_due(const corner_gate_t *g) { return g->on ? corner_due(&g->c) : 0; }
