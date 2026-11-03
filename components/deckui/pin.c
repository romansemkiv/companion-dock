/* Pure: the deck's PIN pad and its unlock (pin.h). */
#include "deckui/pin.h"

#include <stdio.h>
#include <string.h>

#include "deckui/nav.h"
#include "deckui/screens.h"
#include "dui_int.h"
#include "raster.h"

static const dui__pad_ids_t IDS = {.digit0 = DUI_PIN_A_DIGIT, .del = DUI_PIN_A_DEL, .cancel = DUI_PIN_A_CANCEL,
                                   .ok = DUI_PIN_A_OK, .down = DUI_PIN_A_DOWN, .up = DUI_PIN_A_UP, .add = DUI_PIN_A_ADD,
                                   .ok_text = "OK", .add_text = "Next digit"}; /* never read as OK */

static void clear(dui_pin_out_t *o) { memset(o, 0, sizeof *o); }

static void wipe(dui_pin_t *p)
{
    memset(p->d, 0, sizeof p->d);
    p->n = 0;
}

static void no_held(dui_pin_t *p)
{
    memset(&p->held, 0, sizeof p->held);
    p->held.col = p->held.row = -1;
}

int dui_pin_style(const dui_deck_t *d)
{
    if (d->cols >= 8 && d->rows >= 4) return d->bar == DUI_BAR_NONE ? DUI_PIN_PHONE : DUI_PIN_PHONE_BAR;
    if (d->cols >= 5 && d->rows >= 3) return DUI_PIN_ROWS;
    if (d->cols * d->rows >= 8) return DUI_PIN_PAGES;
    return DUI_PIN_ROLLER;
}

static int secs_left(const dui_pin_t *p, int64_t now)
{
    return p->until > now ? (int)((p->until - now + 999) / 1000) : 0;
}

static void lock_out(dui_pin_t *p, int wait_s, int64_t now)
{
    p->phase = DUI_PIN_LOCKED;
    p->until = now + (int64_t)(wait_s > 0 ? wait_s : 1) * 1000;
    p->shown_s = secs_left(p, now);
}

void dui_pin_open(dui_pin_t *p, const dui_deck_t *d, uint16_t from_action, bool have_pin, int wait_s, int64_t now,
                  dui_pin_out_t *o)
{
    clear(o);
    memset(p, 0, sizeof *p);
    no_held(p);
    p->open = true;
    p->style = (uint8_t)dui_pin_style(d);
    p->from_action = from_action;
    p->last_press = now;
    p->phase = DUI_PIN_ENTRY;
    if (!have_pin) p->phase = DUI_PIN_NOPIN;
    else if (wait_s > 0) lock_out(p, wait_s, now);
    o->redraw = true;
}

void dui_pin_close(dui_pin_t *p)
{
    const uint16_t from = p->from_action; /* the caller may still want to know which key it was */
    memset(p, 0, sizeof *p);
    no_held(p);
    p->from_action = from;
}

static void closed(dui_pin_t *p, dui_pin_out_t *o)
{
    o->last_press = p->last_press;
    dui_pin_close(p);
    o->closed = o->redraw = true;
}

/* ---- the words ---- */

/* what the locked key that opened the pad does, for its title: "PIN to restart", "PIN: Wi-Fi off"
 * (colon: a state, not a verb). NULL: an action with no name here ("Enter PIN"). */
static const char *act_words(uint16_t a, bool *colon)
{
    *colon = false;
    switch (a) {
    case DUI_A_RESTART: return "restart";
    case DUI_A_SYNC: return "sync";
    case DUI_A_BOOT_OTHER: return "boot other";
    case DUI_A_MODE_SAT: return "use Satellite";
    case DUI_A_MODE_DOCK: return "use Dock";
    case DUI_A_USE_MAIN: return "use Primary"; /* as the web page calls it */
    case DUI_A_USE_BACKUP: return "use Backup";
    case DUI_A_ETH_DHCP: return "use DHCP";
    case DUI_A_ETH_STATIC: return "make static";
    case DUI_A_ETH_EDIT: return "edit address";
    case DUI_A_ETH_SAVE: return "save address";
    case DUI_A_COMP_EDIT: return "set Companion IP";
    case DUI_A_COMP_SAVE: return "save Companion IP";
    case DUI_A_COMP_AUTO: return "auto find";
    case DUI_A_AP_OFF: *colon = true; return "hotspot off";
    case DUI_A_AP_ON: *colon = true; return "hotspot on"; /* Always on */
    case DUI_A_WIFI_OFF: *colon = true; return "Wi-Fi off";
    case DUI_A_WIFI_ON: *colon = true; return "Wi-Fi on";
    default: return NULL;
    }
}

/* The same, as short as a 3 x 2 deck's key needs ("Satellite", "edit IP"). */
static const char *act_short(uint16_t a)
{
    switch (a) {
    case DUI_A_MODE_SAT: return "Satellite";
    case DUI_A_MODE_DOCK: return "Dock";
    case DUI_A_USE_MAIN: return "Primary";
    case DUI_A_USE_BACKUP: return "Backup";
    case DUI_A_ETH_DHCP: return "DHCP";
    case DUI_A_ETH_STATIC: return "static";
    case DUI_A_ETH_EDIT: return "edit IP";
    case DUI_A_ETH_SAVE: return "save IP";
    case DUI_A_COMP_EDIT: return "set IP";
    case DUI_A_COMP_SAVE: return "save IP";
    case DUI_A_AP_OFF: return "hotspot";
    case DUI_A_AP_ON: return "hotspot";
    default: {
        bool colon;
        return act_words(a, &colon);
    }
    }
}

/* s as a pad key's caption fits a key of key_px (upper case, the caption's tracking, 12 px at least). */
static bool cap_fits(const char *s, int key_px) { return dui__cap_px(s, key_px) > 0; } /* as draw.c sizes it */

int dui_pin_title(const dui_pin_t *p, char *out, int n)
{
    if (n <= 0) return 0;
    bool colon;
    const char *w = act_words(p->from_action, &colon);
    return w ? snprintf(out, (size_t)n, colon ? "PIN: %s" : "PIN to %s", w) : snprintf(out, (size_t)n, "Enter PIN");
}

int dui_pin_message(const dui_pin_t *p, int64_t now, char *out, int n)
{
    if (n <= 0) return 0;
    switch (p->phase) {
    case DUI_PIN_CHECKING: return snprintf(out, (size_t)n, "Checking");
    case DUI_PIN_WRONG: return snprintf(out, (size_t)n, "Wrong PIN");
    case DUI_PIN_LOCKED: return snprintf(out, (size_t)n, "Locked \xC2\xB7 wait %d s", secs_left(p, now));
    case DUI_PIN_NOPIN: return snprintf(out, (size_t)n, "Set a PIN on the web page");
    case DUI_PIN_BUSY: return snprintf(out, (size_t)n, "Busy, try again");
    default: return dui_pin_title(p, out, n);
    }
}

/* The words on the keys (up to 3, one a key: one_key gives them all on one key, but "no PIN", which always
 * takes 3), into pool (NUL-separated); returns how many. */
static int key_words(const dui_pin_t *p, int64_t now, bool one_key, char *pool, size_t cap, const char *w[3])
{
    char buf[48];
    const char *parts[3] = {NULL, NULL, NULL};
    int n = 0;
    switch (p->phase) {
    case DUI_PIN_CHECKING: parts[n++] = "Checking"; break;
    case DUI_PIN_WRONG:
        if (one_key) parts[n++] = "Wrong PIN";
        else parts[n++] = "Wrong", parts[n++] = "PIN";
        break;
    case DUI_PIN_BUSY:
        if (one_key) parts[n++] = "Busy, try again";
        else parts[n++] = "Busy,", parts[n++] = "try again";
        break;
    case DUI_PIN_LOCKED: /* the specified words; one key: on its 2 lines */
        snprintf(buf, sizeof buf, "%swait %d s", one_key ? "Locked \xC2\xB7 " : "", secs_left(p, now));
        if (!one_key) parts[n++] = "Locked";
        parts[n++] = buf;
        break;
    case DUI_PIN_NOPIN: parts[n++] = "Set a PIN", parts[n++] = "on the", parts[n++] = "web page"; break;
    default: { /* the title: one key, or "PIN to" | "restart" */
        bool colon;
        const char *aw = act_words(p->from_action, &colon);
        if (!aw) {
            if (one_key) parts[n++] = "Enter PIN";
            else parts[n++] = "Enter", parts[n++] = "PIN";
        } else if (one_key) {
            dui_pin_title(p, buf, sizeof buf);
            parts[n++] = buf;
        } else {
            parts[n++] = colon ? "PIN:" : "PIN to";
            parts[n++] = aw;
        }
        break;
    }
    }
    size_t u = 0;
    for (int i = 0; i < n; i++) {
        const size_t k = strlen(parts[i]);
        if (u + k + 1 > cap) return i;
        memcpy(pool + u, parts[i], k + 1);
        w[i] = pool + u;
        u += k + 1;
    }
    return n;
}

/* ---- the layout ---- */

static int status_tint(const dui_pin_t *p)
{
    switch (p->phase) {
    case DUI_PIN_CHECKING:
    case DUI_PIN_BUSY: return DUI_TINT_WAIT;
    case DUI_PIN_WRONG:
    case DUI_PIN_LOCKED: return DUI_TINT_BAD;
    default: return DUI_TINT_NEUTRAL;
    }
}

static int status_role(const dui_pin_t *p)
{
    switch (p->phase) {
    case DUI_PIN_CHECKING:
    case DUI_PIN_BUSY: return DUI_ROLE_WAIT;
    case DUI_PIN_WRONG:
    case DUI_PIN_LOCKED: return DUI_ROLE_BAD;
    case DUI_PIN_NOPIN: return DUI_ROLE_VALUE;
    default: return DUI_ROLE_LABEL;
    }
}

/* The keys that do something in this phase: the digits, ⌫, OK, the roller (not while checking, locked
 * out or with no PIN set). */
static bool entry_ok(const dui_pin_t *p)
{
    return p->phase == DUI_PIN_ENTRY || p->phase == DUI_PIN_WRONG || p->phase == DUI_PIN_BUSY;
}

#define put dui__lay_put

static dui_tile_t *key(const dui_pin_t *p, dui_layout_t *L, int page, int col, int row, uint16_t action)
{
    dui_tile_t *t = dui__pad_key(L, page, col, row, action, &IDS);
    const bool works = action == DUI_PIN_A_CANCEL || action == DUI_PIN_A_PAGE || entry_ok(p);
    if (!works) t->flags |= DUI_F_DIM;
    return t;
}

static void status_fill(const dui_pin_t *p, dui_layout_t *L, int col, int row)
{
    put(L, 0, col, row, DUI_TILE_FILL)->tint = (uint8_t)status_tint(p);
}

static void word(const dui_pin_t *p, dui_layout_t *L, int page, int col, int row, const char *text, int tint)
{
    dui_tile_t *t = put(L, page, col, row, DUI_TILE_WORD);
    t->tint = (uint8_t)tint;
    t->role = (uint8_t)status_role(p);
    t->text = text;
}

static void dots(const dui_pin_t *p, dui_layout_t *L, int col, int row, int first, const char *caption)
{
    dui_tile_t *t = put(L, 0, col, row, DUI_TILE_DOTS);
    t->tint = (uint8_t)status_tint(p);
    t->num = p->n;
    t->cur = (uint8_t)first;
    t->caption = caption;
}

/* 1-9 as a phone pad at (x0, y0), ⌫ / 0 / OK in the column beside it. */
static void phone_pad(const dui_pin_t *p, dui_layout_t *L, int x0, int y0)
{
    for (int k = 1; k <= 9; k++) key(p, L, 0, x0 + (k - 1) % 3, y0 + (k - 1) / 3, (uint16_t)(DUI_PIN_A_DIGIT + k));
    key(p, L, 0, x0 + 3, y0, DUI_PIN_A_DEL);
    key(p, L, 0, x0 + 3, y0 + 1, DUI_PIN_A_DIGIT + 0);
    key(p, L, 0, x0 + 3, y0 + 2, DUI_PIN_A_OK);
}

void dui_pin_layout(const dui_deck_t *d, const dui_pin_t *p, int64_t now, dui_layout_t *L)
{
    dui__lay_init(L, d);
    const char *w[3] = {NULL, NULL, NULL};
    const int st = status_tint(p);
    switch (p->style) {
    case DUI_PIN_PHONE: {
        const int nw = key_words(p, now, false, L->title, sizeof L->title, w);
        const bool nopin = p->phase == DUI_PIN_NOPIN; /* 3 words, no dots */
        key(p, L, 0, 0, 0, DUI_PIN_A_CANCEL)->tint = (uint8_t)st;
        for (int i = 0; i < (nopin ? 3 : 2); i++) {
            if (i < nw) word(p, L, 0, 1 + i, 0, w[i], st);
            else status_fill(p, L, 1 + i, 0);
        }
        if (nopin) {
            status_fill(p, L, 4, 0);
        } else {
            dots(p, L, 3, 0, 0, NULL);
            dots(p, L, 4, 0, 4, NULL);
        }
        for (int c = 5; c < d->cols; c++) status_fill(p, L, c, 0);
        phone_pad(p, L, (d->cols - 3) / 2, d->rows - 3);
        dui__lay_align_row0(L);
        break;
    }
    case DUI_PIN_PHONE_BAR: /* the words and the dots on the strip */
        key(p, L, 0, 0, 0, DUI_PIN_A_CANCEL);
        phone_pad(p, L, (d->cols - 3) / 2, d->rows - 3);
        break;
    case DUI_PIN_ROWS: {
        const bool nopin = p->phase == DUI_PIN_NOPIN; /* 3 words, over ⌫ too */
        key(p, L, 0, 0, 0, DUI_PIN_A_CANCEL)->tint = (uint8_t)st;
        if (!nopin) key(p, L, 0, 1, 0, DUI_PIN_A_DEL)->tint = (uint8_t)st;
        if (p->phase == DUI_PIN_ENTRY) { /* the title over the dots: "PIN to" | "restart" */
            const int nw = key_words(p, now, false, L->title, sizeof L->title, w);
            const char *second = nw > 1 ? w[1] : NULL;
            if (second && !cap_fits(second, d->key_px)) second = act_short(p->from_action); /* "Satellite" */
            dots(p, L, 2, 0, 0, nw > 0 ? w[0] : NULL);
            dots(p, L, 3, 0, 4, second);
        } else {
            const int nw = key_words(p, now, false, L->title, sizeof L->title, w), at0 = nopin ? 1 : 2;
            for (int i = 0; i < 4 - at0; i++) {
                if (i < nw) word(p, L, 0, at0 + i, 0, w[i], st);
                else status_fill(p, L, at0 + i, 0);
            }
        }
        key(p, L, 0, 4, 0, DUI_PIN_A_OK)->tint = (uint8_t)st;
        dui__lay_align_row0(L);
        for (int k = 1; k <= 5; k++) key(p, L, 0, k - 1, 1, (uint16_t)(DUI_PIN_A_DIGIT + k));
        for (int k = 6; k <= 10; k++) key(p, L, 0, k - 6, 2, (uint16_t)(DUI_PIN_A_DIGIT + k % 10));
        break;
    }
    case DUI_PIN_PAGES:
        L->pages = 2;
        for (int pg = 0; pg < 2; pg++) {
            for (int k = 0; k < 5; k++) { /* 1-5, then 6-0 */
                const int digit = (pg * 5 + k + 1) % 10;
                key(p, L, pg, k < 4 ? k : 0, k < 4 ? 0 : 1, (uint16_t)(DUI_PIN_A_DIGIT + digit));
            }
            key(p, L, pg, 1, 1, DUI_PIN_A_DEL)->flags |= DUI_F_TAG; /* no ✕ key: "HOLD" and a ✕ on ⌫ */
            key(p, L, pg, 2, 1, DUI_PIN_A_OK);
            dui_tile_t *t = put(L, pg, 3, 1, DUI_TILE_PAGER);
            t->tint = DUI_TINT_MAIN;
            t->action = DUI_PIN_A_PAGE;
        }
        L->pager_col = 3;
        L->pager_row = 1;
        break;
    default: { /* ROLLER */
        key(p, L, 0, 0, 0, DUI_PIN_A_DOWN);
        if (p->phase == DUI_PIN_ENTRY) {
            dui_tile_t *t = key(p, L, 0, 1, 0, 0);
            t->text = dui__digit[p->roller % 10];
            t->flags |= DUI_F_DOTS;
            t->num = p->n;
            /* the title over the digit: the whole one, or the action alone, or its short form, the
             * first that fits the narrow key */
            char whole[sizeof L->title];
            dui_pin_title(p, whole, sizeof whole);
            bool colon;
            const char *cand[3] = {whole, act_words(p->from_action, &colon), act_short(p->from_action)};
            for (int i = 0; i < 3 && !t->caption; i++)
                if (cand[i] && cap_fits(cand[i], d->key_px)) {
                    snprintf(L->title, sizeof L->title, "%s", cand[i]);
                    t->caption = L->title;
                }
        } else if (p->phase == DUI_PIN_NOPIN) { /* the words over the row, nothing else */
            const int nw = key_words(p, now, true, L->title, sizeof L->title, w);
            memset(L->key, DUI_NO_TILE, sizeof L->key);
            L->ntiles = 0;
            for (int i = 0; i < 3 && i < d->cols; i++) {
                if (i < nw) word(p, L, 0, i, 0, w[i], st);
                else status_fill(p, L, i, 0);
            }
            dui__lay_align_row0(L);
            return;
        } else {
            key_words(p, now, true, L->title, sizeof L->title, w);
            word(p, L, 0, 1, 0, w[0], st);
        }
        key(p, L, 0, 2, 0, DUI_PIN_A_UP);
        key(p, L, 0, 0, 1, DUI_PIN_A_DEL)->flags |= DUI_F_TAG; /* no ✕ key: "HOLD" and a ✕ on ⌫ */
        key(p, L, 0, 1, 1, DUI_PIN_A_ADD);
        key(p, L, 0, 2, 1, DUI_PIN_A_OK);
        break;
    }
    }
}

void dui_pin_decorate(const dui_pin_t *p, int col, int row, dui_tile_t *t)
{
    if (p->held.col == col && p->held.row == row && !p->held.fired &&
        (t->kind == DUI_TILE_KEY || t->kind == DUI_TILE_PAGER))
        t->flags |= DUI_F_DOWN;
}

/* ---- input ---- */

static void add(dui_pin_t *p, int digit)
{
    if (p->phase == DUI_PIN_WRONG || p->phase == DUI_PIN_BUSY) p->phase = DUI_PIN_ENTRY; /* typing ends the flash */
    if (p->n >= DUI_PIN_MAX) return;
    p->d[p->n++] = (char)('0' + digit);
    p->d[p->n] = '\0';
}

static void turn(dui_pin_t *p, int steps, dui_pin_out_t *o)
{
    if (p->style != DUI_PIN_PAGES || !(steps & 1)) return;
    p->pg ^= 1;
    o->redraw = true;
}

/* A key's action, on its release. */
static void run(dui_pin_t *p, uint16_t a, dui_pin_out_t *o)
{
    if (p->phase == DUI_PIN_NOPIN || a == DUI_PIN_A_CANCEL) { /* no PIN to enter: any key closes it */
        closed(p, o);
        return;
    }
    if (a == DUI_PIN_A_PAGE) {
        turn(p, 1, o);
        return;
    }
    if (!entry_ok(p)) { /* checking, or locked out */
        if (p->phase == DUI_PIN_LOCKED) o->sound = DUI_SOUND_LOCKED;
        return;
    }
    if (a >= DUI_PIN_A_DIGIT && a <= DUI_PIN_A_DIGIT + 9) {
        add(p, a - DUI_PIN_A_DIGIT);
    } else if (a == DUI_PIN_A_DEL) {
        if (p->n) p->d[--p->n] = '\0';
    } else if (a == DUI_PIN_A_DOWN) {
        p->roller = (uint8_t)((p->roller + 9) % 10);
    } else if (a == DUI_PIN_A_UP) {
        p->roller = (uint8_t)((p->roller + 1) % 10);
    } else if (a == DUI_PIN_A_ADD) {
        add(p, p->roller);
    } else if (a == DUI_PIN_A_OK) {
        if (p->n < DUI_PIN_MIN) {
            o->sound = DUI_SOUND_LOCKED; /* too short: nothing to check */
            return;
        }
        p->phase = DUI_PIN_CHECKING;
        p->until = p->last_press + DUI_PIN_CHECK_MS; /* no answer by then: busy */
        p->roller = 0; /* the last digit chosen doesn't stay on show */
        o->check = true;
    }
    o->redraw = true;
}

void dui_pin_key(dui_pin_t *p, const dui_layout_t *L, int col, int row, bool down, int64_t now, dui_pin_out_t *o)
{
    clear(o);
    if (!p->open || col < 0 || row < 0 || col >= L->deck.cols || row >= L->deck.rows) return;
    if (down) {
        p->last_press = now;
        if (p->held.col >= 0) return; /* one key at a time */
        dui_tile_t t;
        const bool has = dui_layout_key(L, p->pg, col, row, &t);
        no_held(p);
        p->held.col = (int8_t)col;
        p->held.row = (int8_t)row;
        p->held.at = now;
        p->held.action = has && (t.kind == DUI_TILE_KEY || t.kind == DUI_TILE_PAGER) ? t.action : 0;
        if (p->held.action && !(t.flags & DUI_F_DIM)) {
            o->sound = DUI_SOUND_CLICK;
            o->redraw = true; /* shown pressed */
        } else if (p->phase == DUI_PIN_NOPIN) {
            o->sound = DUI_SOUND_CLICK;
        }
        return;
    }
    if (p->held.col != col || p->held.row != row) return; /* not the key that went down */
    const uint16_t a = p->held.action;
    const bool fired = p->held.fired;
    no_held(p);
    p->last_press = now;
    o->redraw = true;
    if (fired) return;
    if (a || p->phase == DUI_PIN_NOPIN) run(p, a, o);
}

void dui_pin_dial_turn(dui_pin_t *p, int steps, int64_t now, dui_pin_out_t *o)
{
    clear(o);
    if (!p->open) return;
    p->last_press = now;
    turn(p, steps < 0 ? -steps : steps, o);
}

void dui_pin_dial_press(dui_pin_t *p, int64_t now, dui_pin_out_t *o)
{
    clear(o);
    if (!p->open) return;
    p->last_press = now; /* a press: the screen's idle clock runs from it, never rewound */
    closed(p, o);
}

void dui_pin_touch(dui_pin_t *p, int dir, int64_t now, dui_pin_out_t *o)
{
    (void)dir;
    dui_pin_dial_turn(p, 1, now, o);
}

void dui_pin_tick(dui_pin_t *p, int64_t now, dui_pin_out_t *o)
{
    clear(o);
    if (!p->open) return;
    if (p->held.col >= 0 && p->held.action == DUI_PIN_A_DEL && !p->held.fired &&
        (p->style == DUI_PIN_PAGES || p->style == DUI_PIN_ROLLER) && now - p->held.at >= DUI_PIN_CANCEL_HOLD_MS) {
        closed(p, o); /* ⌫ held: Cancel, where there is no ✕ key (its release finds the pad closed) */
        return;
    }
    if ((p->phase == DUI_PIN_WRONG || p->phase == DUI_PIN_BUSY) && now >= p->until) {
        p->phase = DUI_PIN_ENTRY;
        o->redraw = true;
    }
    if (p->phase == DUI_PIN_CHECKING && now >= p->until) { /* no answer: busy (the digits went with the check) */
        p->phase = DUI_PIN_BUSY;
        p->until = now + DUI_PIN_WRONG_MS;
        p->last_press = now;
        o->redraw = true;
    }
    if (p->phase == DUI_PIN_LOCKED) {
        if (now >= p->until) {
            p->phase = DUI_PIN_ENTRY;
            o->redraw = true;
        } else if (secs_left(p, now) != p->shown_s) {
            p->shown_s = secs_left(p, now);
            o->redraw = true;
        }
    }
    if (p->phase != DUI_PIN_CHECKING && p->held.col < 0 && now - p->last_press >= DUI_PIN_IDLE_MS) closed(p, o);
}

void dui_pin_result(dui_pin_t *p, int result, int wait_s, int64_t now, dui_pin_out_t *o)
{
    clear(o);
    if (!p->open || p->phase != DUI_PIN_CHECKING) return; /* cancelled meanwhile: a stale answer */
    wipe(p);
    p->last_press = now;
    o->redraw = true;
    switch (result) {
    case DUI_PIN_R_OK:
        closed(p, o);
        o->unlocked = true;
        o->sound = DUI_SOUND_CONFIRM;
        return;
    case DUI_PIN_R_WRONG:
        p->phase = DUI_PIN_WRONG;
        p->until = now + DUI_PIN_WRONG_MS;
        o->sound = DUI_SOUND_LOCKED;
        return;
    case DUI_PIN_R_LOCKED:
        lock_out(p, wait_s, now);
        o->sound = DUI_SOUND_LOCKED;
        return;
    case DUI_PIN_R_BUSY:
        p->phase = DUI_PIN_BUSY;
        p->until = now + DUI_PIN_WRONG_MS;
        return;
    default:
        p->phase = DUI_PIN_NOPIN;
        o->sound = DUI_SOUND_LOCKED;
        return;
    }
}

void dui_pin_take(dui_pin_t *p, char out[DUI_PIN_MAX + 1])
{
    memcpy(out, p->d, DUI_PIN_MAX + 1);
    out[DUI_PIN_MAX] = '\0';
    wipe(p);
}

/* ---- the strip / info bar ---- */

uint32_t dui_pin_bar_hash(const dui_pin_t *p, int64_t now)
{
    char m[48];
    const int n = dui_pin_message(p, now, m, sizeof m);
    const uint8_t b[] = {p->open, p->style, p->phase, p->n, p->pg};
    uint32_t h = dui__fnv(DUI__FNV_BASIS, b, sizeof b);
    h = dui__fnv(h, m, (size_t)(n < 0 ? 0 : n < (int)sizeof m ? n : (int)sizeof m));
    return h ? h : 1;
}

void dui_render_pin_bar(const dui_pin_t *p, int64_t now, int w, int h, uint8_t *rgb)
{
    const dui_canvas_t c = {rgb, w, h};
    const int tint = status_tint(p);
    rs_fill(&c, 0, 0, w, h, tint == DUI_TINT_NEUTRAL ? DUI_RGB_TINT_NEUTRAL : dui__tint_rgb(tint));
    const bool small = h < 80;
    const int pad = small ? 8 : 22;
    char m[48];
    const int n = dui_pin_message(p, now, m, sizeof m);
    /* the dots at the right while digits go in: 8 places, filled as they are typed */
    const bool show_dots = p->phase == DUI_PIN_ENTRY || p->phase == DUI_PIN_WRONG;
    const float r = small ? 5.0f : 11.0f, pitch = small ? 15.0f : 34.0f;
    const float dots_w = 7 * pitch + 2 * r;
    const float dots_x0 = (float)w - (float)pad - dots_w;
    if (show_dots) {
        const float cx = dots_x0 + dots_w / 2;
        const int need = p->n > 4 ? p->n : 4;
        for (int i = 0; i < 8; i++) {
            const float x = cx + ((float)i - 3.5f) * pitch;
            if (i < p->n) rs_draw_disc(&c, x, h / 2.0f, r, SUI_RGB_ACCENT, 1);
            else if (i < need) rs_draw_ring(&c, x, h / 2.0f, r - (small ? 0.8f : 1.5f), small ? 1.6f : 3.0f, DUI_RGB_DOT_OFF);
        }
    }
    const int room = (int)(show_dots ? dots_x0 - (float)pad - (small ? 8 : 24) : (float)(w - 2 * pad));
    const int fs = dui__bar_px(m, n, room, small); /* the bar's one size; else 16 px or more */
    const int role = status_role(p);
    if (fs) dui__text(&c, fs, (float)pad, h / 2.0f + 0.4f * fs + 2, m, n, dui__role_rgb(role), 0, DUI__LEFT);
}

/* ---- the unlock ---- */

void dui_unlock_start(dui_unlock_t *u, int64_t now)
{
    u->on = true;
    u->until = now + DUI_UNLOCK_MS;
}

bool dui_unlock_step(dui_unlock_t *u, int64_t now, bool closed_, bool idle)
{
    if (!u->on) return false;
    if (!closed_ && !idle && now < u->until) return false;
    u->on = false;
    return true;
}

void dui_pin_busy(dui_pin_t *p, const char *pin, int64_t now, dui_pin_out_t *o)
{
    clear(o);
    if (!p->open || p->phase != DUI_PIN_CHECKING) return;
    wipe(p);
    for (int i = 0; pin && pin[i] && i < DUI_PIN_MAX; i++) p->d[p->n++] = pin[i]; /* back, for another OK */
    p->phase = DUI_PIN_BUSY;
    p->until = now + DUI_PIN_WRONG_MS;
    p->last_press = now;
    o->redraw = true;
}

void dui_pin_cancel(dui_pin_t *p) { no_held(p); }
