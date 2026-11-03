/* Pure: the maintenance UI's navigation, one per deck (nav.h): presses,
 * releases, holds, dials and timers. */
#include "deckui/nav.h"

#include <stdio.h>
#include <string.h>

static void clear(dui_nav_out_t *o) { memset(o, 0, sizeof *o); }

static uint64_t bit(const dui_nav_t *n, int col, int row) { return 1ull << (row * n->cols + col); }

static void clear_hints(dui_nav_t *n) { memset(n->hint_until, 0, sizeof n->hint_until); }

static void no_held(dui_nav_t *n)
{
    memset(&n->held, 0, sizeof n->held);
    n->held.col = n->held.row = -1;
}

/* Something the person did with this deck. */
static void pressed(dui_nav_t *n, int64_t now)
{
    n->last_press = now;
    n->touched = true;
    n->idle = false;
}

/* The screen changed under a key held down: it does nothing until its release. A change the person
 * didn't make (theirs false) also holds off new key-downs for DUI_NAV_SETTLE_MS. */
static void changed(dui_nav_t *n, int64_t now, bool theirs)
{
    n->held.ignore = true;
    clear_hints(n);
    n->shown_at = now;
    if (!theirs) n->settle_until = now + DUI_NAV_SETTLE_MS;
}

/* Another screen: its first page. */
static void show(dui_nav_t *n, int page, int64_t now, bool theirs, dui_nav_out_t *o)
{
    n->page = (uint8_t)page;
    n->pg = 0;
    changed(n, now, theirs);
    o->rebuild = o->all = true;
}

/* Another page of this screen. */
static void turn(dui_nav_t *n, int pg, int64_t now, bool theirs, dui_nav_out_t *o)
{
    if (pg == n->pg) return;
    n->pg = (uint8_t)pg;
    changed(n, now, theirs);
    o->all = true;
}

static int wrap(int pg, int pages) { return ((pg % pages) + pages) % pages; }

/* Back: sub-screens to home, or to Menu on decks whose menu is one key; QR to Hotspot; Choose to Companion;
 * Menu to home. */
static int parent(const dui_nav_t *n)
{
    switch (n->page) {
    case DUI_PAGE_HOME:
    case DUI_PAGE_MENU: return DUI_PAGE_HOME;
    case DUI_PAGE_QR: return DUI_PAGE_HOTSPOT;
    case DUI_PAGE_CHOOSE: return DUI_PAGE_COMPANION;
    default: return n->menu_key ? DUI_PAGE_MENU : DUI_PAGE_HOME;
    }
}

static void back(dui_nav_t *n, int64_t now, dui_nav_out_t *o)
{
    if (n->page != DUI_PAGE_HOME) show(n, parent(n), now, true, o);
}

/* A key's action (a tap released, a hold done): the engine's and the GO ones here, a box action out,
 * home after those that restart, Companion after a pick on the Choose page. */
static void run(dui_nav_t *n, uint16_t action, int64_t now, dui_nav_out_t *o)
{
    switch (action) {
    case DUI_A_NONE: return;
    case DUI_ACT_BACK: back(n, now, o); return;
    case DUI_ACT_MENU:
    case DUI_A_GO_MENU: show(n, DUI_PAGE_MENU, now, true, o); return;
    case DUI_ACT_PAGER:
        if (n->pages > 1) turn(n, wrap(n->pg + 1, n->pages), now, true, o);
        return;
    case DUI_ACT_CLOSE: o->close = true; return;
    case DUI_ACT_NET: show(n, DUI_PAGE_NETWORK, now, true, o); return;     /* the indicators */
    case DUI_ACT_HOTSPOT: show(n, DUI_PAGE_HOTSPOT, now, true, o); return;
    case DUI_A_GO_NETWORK: show(n, DUI_PAGE_NETWORK, now, true, o); return;
    case DUI_A_GO_COMPANION: show(n, DUI_PAGE_COMPANION, now, true, o); return;
    case DUI_A_GO_HOTSPOT: show(n, DUI_PAGE_HOTSPOT, now, true, o); return;
    case DUI_A_GO_QR: show(n, DUI_PAGE_QR, now, true, o); return;
    case DUI_A_GO_BOX: show(n, DUI_PAGE_BOX, now, true, o); return;
    case DUI_A_GO_CHOOSE: show(n, DUI_PAGE_CHOOSE, now, true, o); return; /* a list, nothing saved */
    default: break;
    }
    if (action >= DUI_A_COUNT) return;
    o->action = action;
    if (action == DUI_A_MODE_SAT || action == DUI_A_MODE_DOCK || action == DUI_A_RESTART ||
        action == DUI_A_BOOT_OTHER)
        show(n, DUI_PAGE_HOME, now, true, o);
    else if (action >= DUI_A_PICK0 && action <= DUI_A_PICK5 && n->page == DUI_PAGE_CHOOSE)
        show(n, DUI_PAGE_COMPANION, now, true, o); /* picked: the target it connects to now */
}

void dui_nav_init(dui_nav_t *n, const dui_deck_t *d, int64_t now)
{
    memset(n, 0, sizeof *n);
    n->cols = d->cols;
    n->rows = d->rows;
    n->menu_key = dui_home_menu(d) != DUI_MENU_ROW;
    n->page = DUI_PAGE_HOME;
    n->pages = 1;
    n->last_press = n->shown_at = now;
    no_held(n);
}

void dui_nav_layout(dui_nav_t *n, const dui_layout_t *L)
{
    n->pages = L->pages ? L->pages : 1;
    if (n->pg >= n->pages) n->pg = 0;
    if (n->held.col < 0 || n->held.ignore) return;
    dui_tile_t t; /* the held key's tile changed (the box changed under it): it no longer counts */
    dui_layout_key(L, n->pg, n->held.col, n->held.row, &t);
    if (t.kind != n->held.kind || t.action != n->held.action || t.hold_ms != n->held.hold_ms ||
        ((t.flags ^ n->held.flags) & DUI_F_LOCKED))
        n->held.ignore = true;
}

void dui_nav_key(dui_nav_t *n, const dui_layout_t *L, int col, int row, bool down, int64_t now, dui_nav_out_t *out)
{
    clear(out);
    if (col < 0 || row < 0 || col >= n->cols || row >= n->rows || row * n->cols + col >= DUI_KEYS_MAX) return;
    const int k = row * n->cols + col;
    if (!down) {
        if (n->held.col != col || n->held.row != row) return; /* not the key that went down: not a press */
        pressed(n, now);
        const bool ignore = n->held.ignore, tap_page = n->held.tap_page, fired = n->held.fired, locked = n->held.locked;
        const uint16_t action = n->held.action, hold_ms = n->held.hold_ms;
        no_held(n);
        if (ignore) return;
        if (locked) { /* the PIN pad, over this screen; the key itself never fires */
            out->keys = bit(n, col, row);
            out->pin = true;
            out->pin_action = action;
            return;
        }
        if (tap_page) {
            turn(n, wrap(n->pg + 1, n->pages), now, true, out);
            return;
        }
        out->keys = bit(n, col, row); /* shown released */
        if (hold_ms) { /* a hold let go early: what it is for */
            if (!fired) n->hint_until[k] = now + DUI_NAV_HINT_MS;
            return;
        }
        run(n, action, now, out);
        return;
    }
    pressed(n, now);
    if (n->held.col >= 0) return; /* another key is down: one at a time */
    no_held(n);
    n->held.col = (int8_t)col;
    n->held.row = (int8_t)row;
    n->held.at = now;
    if (now < n->settle_until) { /* the screen just changed under the finger */
        n->held.ignore = true;
        return;
    }
    dui_tile_t t;
    const bool has = dui_layout_key(L, n->pg, col, row, &t);
    n->held.kind = t.kind;
    n->held.action = t.action;
    n->held.hold_ms = t.hold_ms;
    n->held.flags = t.flags;
    if (has && (t.kind == DUI_TILE_BUTTON || t.kind == DUI_TILE_PAGER || (t.kind == DUI_TILE_IND && t.action))) {
        if (t.flags & DUI_F_LOCKED) { /* locked; its release opens the PIN pad */
            out->sound = DUI_SOUND_CLICK;
            out->keys = bit(n, col, row);
            n->held.locked = true;
            return;
        }
        out->sound = DUI_SOUND_CLICK;
        out->keys = bit(n, col, row); /* shown pressed */
        n->hint_until[k] = 0;
        return;
    }
    out->sound = DUI_SOUND_CLICK; /* every press clicks */
    /* the Mini's home: any key that isn't a button turns the page, the status row and blanks too */
    if (L->pager == DUI_PAGER_TAP && n->pages > 1) n->held.tap_page = true;
    else n->held.ignore = true; /* an info tile: nothing on release */
}

void dui_nav_dial_turn(dui_nav_t *n, int steps, int64_t now, dui_nav_out_t *out)
{
    clear(out);
    pressed(n, now);
    if (n->pages <= 1) return;
    const int pg = wrap(n->pg + steps, n->pages);
    if (pg == n->pg) return;
    out->sound = DUI_SOUND_CLICK;
    turn(n, pg, now, true, out);
}

void dui_nav_dial_press(dui_nav_t *n, int64_t now, dui_nav_out_t *out)
{
    clear(out);
    pressed(n, now);
    out->sound = DUI_SOUND_CLICK;
    back(n, now, out);
}

void dui_nav_touch(dui_nav_t *n, int dir, int64_t now, dui_nav_out_t *out)
{
    dui_nav_dial_turn(n, dir < 0 ? -1 : 1, now, out);
}

void dui_nav_tick(dui_nav_t *n, int64_t now, dui_nav_out_t *out)
{
    clear(out);
    for (int k = 0; k < n->cols * n->rows && k < DUI_KEYS_MAX; k++) /* hints run out */
        if (n->hint_until[k] && now >= n->hint_until[k]) {
            n->hint_until[k] = 0;
            out->keys |= 1ull << k;
        }
    if (n->held.col >= 0 && !n->held.ignore && !n->held.fired && !n->held.locked && n->held.hold_ms &&
        n->held.kind == DUI_TILE_BUTTON) {
        out->keys |= bit(n, n->held.col, n->held.row); /* its fill grows */
        if (now - n->held.at >= n->held.hold_ms) {
            n->held.fired = true;
            out->sound = DUI_SOUND_CONFIRM;
            run(n, n->held.action, now, out);
        }
    }
    if (n->held.col < 0 && !n->idle && now - n->last_press >= DUI_NAV_IDLE_MS) { /* nobody there */
        const bool was_touched = n->touched;
        n->idle = true;
        n->touched = false;
        if (n->ondemand) out->close = true;
        if (n->page != DUI_PAGE_HOME) show(n, DUI_PAGE_HOME, now, false, out);
        else if (was_touched) n->shown_at = now; /* paging by itself again, a period from now */
    }
    if (n->page == DUI_PAGE_HOME && !n->touched && n->pages > 1 && n->held.col < 0 &&
        now - n->shown_at >= sui_page_us(n->pages) / 1000) /* untouched: the home pages by itself */
        turn(n, wrap(n->pg + 1, n->pages), now, false, out);
}

void dui_nav_box_changed(dui_nav_t *n, dui_nav_out_t *out)
{
    (void)n;
    clear(out);
    out->rebuild = out->all = true;
}

void dui_nav_cancel(dui_nav_t *n)
{
    no_held(n);
    clear_hints(n);
}

void dui_nav_ondemand(dui_nav_t *n, bool on, int64_t now, dui_nav_out_t *out)
{
    clear(out);
    n->ondemand = on;
    dui_nav_cancel(n); /* a key held across it: its release goes to Companion, or came before it */
    if (!on) { /* the Close key goes */
        out->rebuild = out->all = true;
        return;
    }
    n->touched = n->idle = false;
    n->last_press = now;
    show(n, DUI_PAGE_HOME, now, false, out);
}

void dui_nav_activity(dui_nav_t *n, int64_t now) { pressed(n, now); }

int dui_nav_page(const dui_nav_t *n) { return n->page; }
int dui_nav_pg(const dui_nav_t *n) { return n->pg; }

void dui_nav_decorate(const dui_nav_t *n, int col, int row, int64_t now, dui_tile_t *t)
{
    if (col < 0 || row < 0 || col >= n->cols || row >= n->rows) return;
    const int k = row * n->cols + col;
    if (n->held.col == col && n->held.row == row && !n->held.ignore &&
        (t->kind == DUI_TILE_BUTTON || t->kind == DUI_TILE_PAGER || (t->kind == DUI_TILE_IND && t->action))) {
        t->flags |= DUI_F_DOWN;
        if (n->held.hold_ms && !n->held.fired && !n->held.locked) {
            const int64_t e = now - n->held.at;
            t->hold = (uint8_t)(e >= n->held.hold_ms ? 255 : e <= 0 ? 0 : e * 255 / n->held.hold_ms);
        }
    }
    if (k < DUI_KEYS_MAX && n->hint_until[k] > now) t->flags |= DUI_F_HINT;
    if (n->pulse_action && t->kind == DUI_TILE_BUTTON && t->action == n->pulse_action && now >= n->pulse_from &&
        now < n->pulse_until) /* the key just unlocked glows cyan, then not, so it shows where it is (phase: the glow) */
        t->phase = (uint8_t)((now - n->pulse_from) / DUI_PULSE_HALF_MS % 2 == 0);
}

void dui_nav_pulse(dui_nav_t *n, uint16_t action, int64_t now)
{
    n->pulse_action = action;
    n->pulse_from = now;
    n->pulse_until = now + DUI_UNLOCK_PULSE_MS;
}

int dui_unlock_note(const dui_layout_t *L, uint16_t action, char *out, int n)
{
    if (n <= 0) return 0;
    for (int i = 0; L && action && i < L->ntiles; i++) {
        const dui_tile_t *t = &L->tile[i];
        if (t->kind != DUI_TILE_BUTTON || t->action != action || !t->text) continue;
        const int len = t->len ? t->len : (int)strlen(t->text);
        return snprintf(out, (size_t)n, "Unlocked \xC2\xB7 %s %.*s", t->hold_ms ? "hold" : "press", len, t->text);
    }
    return snprintf(out, (size_t)n, "Unlocked");
}

void dui_nav_bar_tap(dui_nav_t *n, const dui_status_t *st, int w, int h, int x, int64_t now, dui_nav_out_t *out)
{
    clear(out);
    const int a = dui_bar_hit(st, w, h, x);
    if (!a) return;
    pressed(n, now);
    out->sound = DUI_SOUND_CLICK;
    run(n, (uint16_t)a, now, out);
}
