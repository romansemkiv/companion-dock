/* Pure: the Network Dock mode image cache and gate of one deck (dockcache.h). */
#include "dockcache.h"

#include <string.h>

#define FEAT_MAX 32
#define REP_FILL 0x06  /* gen2 feature 03 06 key r g b: fill a key with a colour */
#define REP_CLEAR 0x05 /* gen2 feature 03 05 r g b: fill the whole LCD (core: clearPanel, 0 0 0) */
#define REP_RESET 0x02 /* gen2 feature 03 02: the logo on the whole LCD */

static size_t rlen(const dc_t *c) { return c->m->report_len; }
/* The hold covers the strip and the info bar (the maintenance screen on demand). */
static bool strip_held(const dc_t *c) { return c->hold && c->hold_all; }
static unsigned nkeys(const dc_t *c) { return (unsigned)c->m->cols * c->m->rows; }

static bool same_target(const deck_img_target_t *a, const deck_img_target_t *b)
{
    if (a->kind != b->kind) return false;
    if (a->kind == DECK_TGT_KEY) return a->key == b->key;
    if (a->kind == DECK_TGT_LCD) return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
    return true;
}

/* a lies within b (both regions) */
static bool inside(const deck_img_target_t *a, const deck_img_target_t *b)
{
    return a->x >= b->x && a->y >= b->y && (uint32_t)a->x + a->w <= (uint32_t)b->x + b->w &&
           (uint32_t)a->y + a->h <= (uint32_t)b->y + b->h;
}

/* ---- bounds ---- */

size_t dc_target_cap(const deck_model_t *m, const deck_img_target_t *t)
{
    size_t px, hdr;
    switch (t->kind) {
    case DECK_TGT_KEY:
        px = (size_t)m->key_px * m->key_px;
        hdr = m->gen == DECK_GEN2 ? 8 : 16;
        break;
    case DECK_TGT_INFO:
        px = (size_t)m->info_w * m->info_h;
        hdr = 8;
        break;
    case DECK_TGT_LCD:
        px = (size_t)t->w * t->h;
        if (px > (size_t)m->lcd_w * m->lcd_h) return 0;
        hdr = 16;
        break;
    default: return 0;
    }
    if (px == 0 || m->report_len <= hdr) return 0;
    const size_t per = m->report_len - hdr;
    return (px * 3 + DC_SLACK_BYTES + per - 1) / per;
}

size_t dc_bound_bytes(const deck_model_t *m)
{
    if (!deck_model_has_display(m)) return 0;
    const deck_img_target_t key = {.kind = DECK_TGT_KEY};
    const deck_img_target_t strip = {.kind = DECK_TGT_LCD, .w = m->lcd_w, .h = m->lcd_h};
    const deck_img_target_t info = {.kind = DECK_TGT_INFO};
    const size_t kc = dc_target_cap(m, &key);
    const size_t sc = m->lcd_w ? dc_target_cap(m, &strip) : 0;
    const size_t ic = m->info_w ? dc_target_cap(m, &info) : 0;
    size_t big = kc;
    if (sc > big) big = sc;
    if (ic > big) big = ic;
    const size_t nk = (size_t)m->cols * m->rows;
    return (2 * nk * kc + 2 * (DC_LCD_MAX * sc + ic) + DC_SEQ_MAX * big) * m->report_len;
}

/* ---- entries ---- */

static void entry_free(dc_t *c, dc_entry_t *e)
{
    c->mem.release(e->reps);
    e->reps = NULL;
    e->n = 0;
}

static void entry_none(dc_t *c, dc_entry_t *e, uint32_t stamp)
{
    entry_free(c, e);
    e->kind = DC_NONE;
    e->dirty = false;
    e->stamp = stamp;
}

static void entry_fill(dc_t *c, dc_entry_t *e, uint8_t r, uint8_t g, uint8_t b, uint32_t stamp, bool dirty)
{
    entry_free(c, e);
    e->kind = DC_FILL;
    e->rgb[0] = r;
    e->rgb[1] = g;
    e->rgb[2] = b;
    e->stamp = stamp;
    e->dirty = dirty;
}

/* The entry of a target, made when there is none (an LCD region takes a free slot, else the oldest). */
static dc_entry_t *entry_of(dc_t *c, const deck_img_target_t *t)
{
    if (t->kind == DECK_TGT_KEY) return &c->key[t->key];
    if (t->kind == DECK_TGT_INFO) return &c->info;
    dc_entry_t *free_e = NULL, *oldest = &c->lcd[0];
    for (int i = 0; i < DC_LCD_MAX; i++) {
        dc_entry_t *e = &c->lcd[i];
        if (e->kind != DC_NONE && same_target(&e->t, t)) return e;
        if (e->kind == DC_NONE && !free_e) free_e = e;
        if (e->stamp < oldest->stamp) oldest = e;
    }
    dc_entry_t *e = free_e ? free_e : oldest;
    entry_none(c, e, 0);
    e->t = *t;
    return e;
}

/* ---- sequences ---- */

static void seq_end(dc_t *c, dc_seq_t *s)
{
    c->mem.release(s->reps);
    memset(s, 0, sizeof *s);
}

static dc_seq_t *seq_of(dc_t *c, const deck_img_target_t *t)
{
    for (int i = 0; i < DC_SEQ_MAX; i++)
        if (c->seq[i].active && same_target(&c->seq[i].t, t)) return &c->seq[i];
    return NULL;
}

/* A new image starts: room for it at its cap. Without that room it can't be held either. */
static dc_seq_t *seq_start(dc_t *c, const deck_img_target_t *t, bool hold, uint32_t now)
{
    dc_seq_t *s = NULL;
    for (int i = 0; i < DC_SEQ_MAX && !s; i++)
        if (!c->seq[i].active) s = &c->seq[i];
    if (!s) { /* the oldest gives way (the host library sends one image at a time) */
        s = &c->seq[0];
        for (int i = 1; i < DC_SEQ_MAX; i++)
            if ((int32_t)(c->seq[i].at_ms - s->at_ms) < 0) s = &c->seq[i];
        seq_end(c, s);
    }
    s->active = true;
    s->t = *t;
    s->next = deckproto_image_first_chunk(c->m);
    s->at_ms = now;
    const size_t cap = dc_target_cap(c->m, t);
    s->cap = (uint16_t)(cap > UINT16_MAX ? UINT16_MAX : cap);
    s->reps = s->cap ? c->mem.alloc((size_t)s->cap * rlen(c)) : NULL;
    if (!s->reps) {
        s->nocache = true;
        c->stats.uncached++;
    }
    s->held = hold && !s->nocache;
    return s;
}

/* The sequence is complete: its reports become the target's image. */
static void commit(dc_t *c, dc_seq_t *s)
{
    dc_entry_t *e = entry_of(c, &s->t);
    entry_free(c, e);
    uint8_t *reps = s->reps;
    if (c->mem.resize && s->n < s->cap) {
        uint8_t *p = c->mem.resize(reps, (size_t)s->n * rlen(c));
        if (p) reps = p;
    }
    s->reps = NULL;
    e->kind = DC_IMAGE;
    e->reps = reps;
    e->n = s->n;
    e->t = s->t;
    e->stamp = ++c->stamp;
    e->dirty = s->held;
    if (s->t.kind != DECK_TGT_LCD) return;
    for (int i = 0; i < DC_LCD_MAX; i++) { /* regions it draws over whole are gone */
        dc_entry_t *o = &c->lcd[i];
        if (o != e && o->kind != DC_NONE && inside(&o->t, &e->t)) entry_none(c, o, 0);
    }
}

/* The deck shows an image of t that isn't kept: what is kept of t is out of date. A strip region
 * takes no slot: the regions it covers whole are forgotten. */
static void forget(dc_t *c, const deck_img_target_t *t)
{
    if (t->kind != DECK_TGT_LCD) {
        entry_none(c, entry_of(c, t), ++c->stamp);
        return;
    }
    for (int i = 0; i < DC_LCD_MAX; i++)
        if (c->lcd[i].kind != DC_NONE && inside(&c->lcd[i].t, t)) entry_none(c, &c->lcd[i], 0);
}

/* ---- writing ---- */

static bool write_reps(const dc_t *c, const dc_io_t *io, const uint8_t *p, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
        if (!io->write(io->ctx, false, p + (size_t)i * rlen(c), rlen(c))) return false;
    return true;
}

/* An image held so far is passed on after all (too big to keep): what was held first. */
static bool flush(dc_t *c, const dc_io_t *io, dc_seq_t *s)
{
    if (!write_reps(c, io, s->reps, s->n)) return false;
    s->held = false;
    return true;
}

static bool write_fill(dc_t *c, const dc_io_t *io, uint8_t key, const uint8_t rgb[3])
{
    uint8_t f[FEAT_MAX];
    const size_t n = deckproto_feat_fill_key(c->m, key, rgb[0], rgb[1], rgb[2], f);
    return n == 0 || io->write(io->ctx, true, f, n);
}

static bool write_entry(dc_t *c, const dc_io_t *io, const dc_entry_t *e)
{
    static const uint8_t black[3] = {0, 0, 0};
    switch (e->kind) {
    case DC_IMAGE: return write_reps(c, io, e->reps, e->n);
    case DC_FILL: return write_fill(c, io, e->t.key, e->rgb);
    default: /* a key under a message frame with nothing kept: black (gen1 has no fill: it keeps the frame) */
        return e->t.kind != DECK_TGT_KEY || write_fill(c, io, e->t.key, black);
    }
}

/* Every entry newer than the panel op just written: the op covered it, so it is written again. */
static void dirty_after_panel(dc_t *c)
{
    for (int i = 0; i < DC_KEYS_MAX + DC_LCD_MAX + 1; i++) {
        dc_entry_t *e = i < DC_KEYS_MAX ? &c->key[i] : i < DC_KEYS_MAX + DC_LCD_MAX ? &c->lcd[i - DC_KEYS_MAX] : &c->info;
        if (e->kind != DC_NONE && e->stamp > c->panel_stamp) e->dirty = true;
    }
}

/* Writes what waits, oldest first: a held panel op, dirty entries and (keys true) the keys under a
 * message frame. While holding (keys false) only the strip and the info bar, and those not while the
 * hold covers them too (hold_all): a panel op would wipe the frames, it waits for the hold to end. */
static bool replay(dc_t *c, const dc_io_t *io, bool keys)
{
    for (;;) {
        dc_entry_t *best = NULL;
        uint32_t at = UINT32_MAX;
        const bool clear = keys && c->panel_dirty;
        if (clear) at = c->panel_stamp;
        for (unsigned k = 0; keys && k < nkeys(c); k++) {
            dc_entry_t *e = &c->key[k];
            if ((e->dirty || c->box_drew[k]) && e->stamp < at) {
                best = e;
                at = e->stamp;
            }
        }
        for (int i = 0; i <= DC_LCD_MAX && !strip_held(c); i++) {
            dc_entry_t *e = i < DC_LCD_MAX ? &c->lcd[i] : &c->info;
            if (e->dirty && e->stamp < at) {
                best = e;
                at = e->stamp;
            }
        }
        if (clear && !best) { /* the op is the oldest (it wins a tie: entries it set carry its stamp) */
            if (!io->write(io->ctx, true, c->panel_rep, c->panel_len)) return false;
            c->panel_dirty = false;
            dirty_after_panel(c);
            c->stats.replayed++;
            continue;
        }
        if (!best) return true;
        if (!write_entry(c, io, best)) return false;
        best->dirty = false;
        if (best->t.kind == DECK_TGT_KEY) c->box_drew[best->t.key] = false;
        c->stats.replayed++;
    }
}

/* An image Companion is passing on is open (the box writes nothing then). Sequences that went
 * stale are dropped here: never finished, the deck kept what it showed. */
static bool open_passing(dc_t *c, uint32_t now)
{
    bool open = false;
    for (int i = 0; i < DC_SEQ_MAX; i++) {
        dc_seq_t *s = &c->seq[i];
        if (!s->active) continue;
        if (now - s->at_ms >= DC_STALE_MS)
            seq_end(c, s);
        else if (!s->held)
            open = true;
    }
    return open;
}

bool dc_pump(dc_t *c, const dc_io_t *io)
{
    if (open_passing(c, io->now_ms)) return true;
    if (c->hold_all && c->box_bright_due) { /* the box's brightness, between Companion's images */
        if (!io->write(io->ctx, true, c->box_bright_rep, c->box_bright_len)) return false;
        c->box_bright_due = false;
        c->box_bright_on = true;
    }
    if (c->comp_bright_due && !c->hold_all) { /* on demand over: Companion's last brightness back (never while
                                               * the box's screen covers the deck again) */
        if (!io->write(io->ctx, true, c->comp_bright_rep, c->comp_bright_len)) return false;
        c->comp_bright_due = false;
    }
    if (!c->hold) return replay(c, io, true);
    for (unsigned k = 0; k < nkeys(c); k++) {
        dc_box_t *b = &c->box[k];
        if (!b->reps || b->written) continue;
        if (!write_reps(c, io, b->reps, b->n)) return false;
        b->written = true;
        c->box_drew[k] = true;
        c->stats.box_frames++;
    }
    for (int i = 0; c->hold_all && i <= DC_LCD_MAX; i++) { /* the strip's frames, the info bar's */
        dc_box_t *b = i < DC_LCD_MAX ? &c->box_lcd[i] : &c->box_info;
        if (!b->reps || b->written) continue;
        if (!write_reps(c, io, b->reps, b->n)) return false;
        b->written = true;
        if (i < DC_LCD_MAX)
            c->box_drew_lcd = true;
        else
            c->box_drew_info = true;
        c->stats.box_frames++;
    }
    return replay(c, io, false);
}

/* ---- Companion's traffic ---- */

bool dc_output(dc_t *c, const dc_io_t *io, const uint8_t *r, size_t len)
{
    deck_img_target_t t;
    uint16_t ch;
    bool last;
    const uint8_t *pl;
    size_t pn;
    if (!deckproto_parse_image_report(c->m, r, len, &t, &ch, &last, &pl, &pn))
        return io->write(io->ctx, false, r, len) && dc_pump(c, io); /* not an image: as it is */

    const bool key = t.kind == DECK_TGT_KEY;
    dc_seq_t *s = seq_of(c, &t);
    if (ch == deckproto_image_first_chunk(c->m)) {
        if (s) seq_end(c, s); /* one never finished: dropped */
        s = seq_start(c, &t, c->hold && (key || c->hold_all), io->now_ms);
    } else if (s && ch != s->next && !s->broken) { /* a gap: it can't be shown or kept */
        s->broken = s->nocache = true;
        c->mem.release(s->reps);
        s->reps = NULL;
    }

    bool held = false; /* a report of no image known: passed on (it can't be kept, nor dropped) */
    if (s) {
        s->at_ms = io->now_ms;
        s->next = (uint16_t)(ch + 1);
        if (!s->nocache && s->n >= s->cap) { /* too big to keep: never lose it, pass it on */
            c->stats.uncached++;
            if (s->held && !flush(c, io, s)) return false;
            s->nocache = true;
            c->mem.release(s->reps);
            s->reps = NULL;
        }
        if (!s->nocache) {
            uint8_t *dst = s->reps + (size_t)s->n * rlen(c);
            const size_t n = len < rlen(c) ? len : rlen(c);
            memcpy(dst, r, n);
            if (n < rlen(c)) memset(dst + n, 0, rlen(c) - n);
            s->n++;
        }
        held = s->held;
    }
    if (held)
        c->stats.held_reports++;
    else if (!io->write(io->ctx, false, r, len))
        return false;

    if (s && last) {
        if (!s->nocache) {
            if (s->held) c->stats.held_images++;
            commit(c, s);
        } else if (!s->held && !s->broken) { /* the deck shows an image that isn't kept */
            forget(c, &t);
        }
        if (key && !s->held && !s->broken) c->box_drew[t.key] = false; /* Companion's image is on top */
        seq_end(c, s);
    }
    return dc_pump(c, io);
}

bool dc_feature(dc_t *c, const dc_io_t *io, const uint8_t *r, size_t len)
{
    if (deckproto_feat_brightness_of(c->m, r, len) >= 0) { /* kept; held while on demand covers the deck */
        c->comp_bright_len = len < sizeof c->comp_bright_rep ? len : sizeof c->comp_bright_rep;
        memcpy(c->comp_bright_rep, r, c->comp_bright_len);
        c->comp_bright_known = true;
        c->comp_bright_due = false;
        if (c->hold_all) {
            c->stats.held_reports++;
            return dc_pump(c, io);
        }
        return io->write(io->ctx, true, r, len) && dc_pump(c, io);
    }
    if (c->m->gen == DECK_GEN2 && deck_model_has_display(c->m) && len >= 2 && r[0] == 0x03) {
        if (r[1] == REP_FILL && len >= 6 && r[2] < nkeys(c)) { /* an LCD key filled: its image now */
            entry_fill(c, &c->key[r[2]], r[3], r[4], r[5], ++c->stamp, c->hold);
            if (c->hold) {
                c->stats.held_reports++;
                return dc_pump(c, io);
            }
            return io->write(io->ctx, true, r, len) && dc_pump(c, io);
        }
        if (r[1] == REP_CLEAR || r[1] == REP_RESET) { /* the whole panel: a colour, or the logo */
            const uint32_t st = ++c->stamp;
            const bool fill = r[1] == REP_CLEAR;
            const uint8_t cr = fill && len > 2 ? r[2] : 0, cg = fill && len > 3 ? r[3] : 0, cb = fill && len > 4 ? r[4] : 0;
            for (unsigned k = 0; k < nkeys(c); k++) {
                if (fill)
                    entry_fill(c, &c->key[k], cr, cg, cb, st, false);
                else
                    entry_none(c, &c->key[k], st); /* the logo: only the op itself brings it back */
                c->box_drew[k] = false;            /* replaying the op puts it over the frames */
            }
            for (int i = 0; i < DC_LCD_MAX; i++) entry_none(c, &c->lcd[i], 0); /* inside the panel too */
            entry_none(c, &c->info, st);
            c->panel_valid = true;
            c->panel_len = len < sizeof c->panel_rep ? len : sizeof c->panel_rep;
            memcpy(c->panel_rep, r, c->panel_len);
            c->panel_stamp = st;
            c->panel_dirty = c->hold;
            if (c->hold) {
                c->stats.held_reports++;
                return dc_pump(c, io);
            }
            return io->write(io->ctx, true, r, len) && dc_pump(c, io);
        }
    }
    return io->write(io->ctx, true, r, len) && dc_pump(c, io); /* brightness and the rest: at once */
}

/* ---- the box's side ---- */

void dc_init(dc_t *c, const deck_model_t *m, const dc_mem_t *mem)
{
    memset(c, 0, sizeof *c);
    c->m = m;
    c->mem = *mem;
    for (int k = 0; k < DC_KEYS_MAX; k++) {
        c->key[k].t.kind = DECK_TGT_KEY;
        c->key[k].t.key = (uint8_t)k;
    }
    c->info.t.kind = DECK_TGT_INFO;
}

/* the box's frames go, their reports released: the keys' and / or the strip's and the info bar's. */
static void free_box_frames(dc_t *c, bool keys, bool strip)
{
    for (int k = 0; keys && k < DC_KEYS_MAX; k++) {
        c->mem.release(c->box[k].reps);
        memset(&c->box[k], 0, sizeof c->box[k]);
    }
    for (int i = 0; strip && i <= DC_LCD_MAX; i++) {
        dc_box_t *b = i < DC_LCD_MAX ? &c->box_lcd[i] : &c->box_info;
        c->mem.release(b->reps);
        memset(b, 0, sizeof *b);
    }
}

void dc_reset(dc_t *c)
{
    for (int k = 0; k < DC_KEYS_MAX; k++) {
        entry_none(c, &c->key[k], 0);
        c->mem.release(c->box[k].reps);
        memset(&c->box[k], 0, sizeof c->box[k]);
        c->box_drew[k] = false;
    }
    for (int i = 0; i < DC_LCD_MAX; i++) entry_none(c, &c->lcd[i], 0);
    entry_none(c, &c->info, 0);
    free_box_frames(c, false, true);
    c->box_drew_lcd = c->box_drew_info = false;
    for (int i = 0; i < DC_SEQ_MAX; i++) seq_end(c, &c->seq[i]);
    c->panel_valid = c->panel_dirty = false;
    /* the brightness state belongs to a connection: a new one starts with none known */
    c->comp_bright_known = c->comp_bright_due = false;
    c->box_bright_on = c->box_bright_due = false;
    memset(&c->stats, 0, sizeof c->stats);
}

/* The box's strip and info frames go: what Companion drew there comes back from the cache. */
static void restore_strip(dc_t *c)
{
    free_box_frames(c, false, true);
    for (int i = 0; c->box_drew_lcd && i < DC_LCD_MAX; i++)
        if (c->lcd[i].kind != DC_NONE) c->lcd[i].dirty = true;
    if (c->box_drew_info && c->info.kind != DC_NONE) c->info.dirty = true;
    c->box_drew_lcd = c->box_drew_info = false;
}

void dc_set_hold(dc_t *c, bool on)
{
    if (on == c->hold) return;
    c->hold = on;
    if (on) return;
    free_box_frames(c, true, false); /* the keys under them come back from the cache */
    restore_strip(c);
}

void dc_set_hold_all(dc_t *c, bool all)
{
    if (all == c->hold_all) return;
    c->hold_all = all;
    if (all) c->comp_bright_due = false; /* covered again: the box's brightness now, Companion's at the end */
    if (!all) {
        restore_strip(c);
        /* the box changed the brightness: Companion's last one goes back (none known: it stays) */
        if (c->box_bright_on && c->comp_bright_known) c->comp_bright_due = true;
        c->box_bright_on = c->box_bright_due = false;
    }
}

static bool box_frame(dc_t *c, dc_box_t *b, deck_img_writer_t *w, size_t cap);

bool dc_box_key(dc_t *c, uint8_t hw, const uint8_t *img, size_t len)
{
    if (!c->hold || !deck_model_has_display(c->m) || hw >= nkeys(c) || !img || !len) return false;
    const deck_img_target_t t = {.kind = DECK_TGT_KEY, .key = hw};
    deck_img_writer_t w; /* box_frame is this, line for line (deckproto_img_key only sets the writer up) */
    deckproto_img_key(&w, c->m, hw, img, len);
    return box_frame(c, &c->box[hw], &w, dc_target_cap(c->m, &t));
}

/* Builds a box frame's reports from w into b (replacing what b held). */
static bool box_frame(dc_t *c, dc_box_t *b, deck_img_writer_t *w, size_t cap)
{
    uint8_t *buf = cap ? c->mem.alloc(cap * rlen(c)) : NULL;
    if (!buf) return false;
    uint16_t n = 0;
    while (n < cap && deckproto_img_next(w, buf + (size_t)n * rlen(c))) n++;
    if (n == 0 || w->off < w->len) { /* nothing, or more than the cap */
        c->mem.release(buf);
        return false;
    }
    if (c->mem.resize && n < cap) {
        uint8_t *p = c->mem.resize(buf, (size_t)n * rlen(c));
        if (p) buf = p;
    }
    c->mem.release(b->reps);
    *b = (dc_box_t){.reps = buf, .n = n, .written = false};
    return true;
}

bool dc_box_lcd(dc_t *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *img, size_t len)
{
    if (!strip_held(c) || !deck_model_has_display(c->m) || !c->m->lcd_w || !img || !len) return false;
    if ((uint32_t)x + w > c->m->lcd_w || (uint32_t)y + h > c->m->lcd_h) return false;
    const deck_img_target_t t = {.kind = DECK_TGT_LCD, .x = x, .y = y, .w = w, .h = h};
    int at = -1;
    for (int i = 0; i < DC_LCD_MAX && at < 0; i++) /* its region's frame, else a free one */
        if (c->box_lcd[i].reps && same_target(&c->box_lcd_t[i], &t)) at = i;
    for (int i = 0; i < DC_LCD_MAX && at < 0; i++)
        if (!c->box_lcd[i].reps) at = i;
    if (at < 0) return false;
    deck_img_writer_t wr;
    deckproto_img_lcd(&wr, c->m, x, y, w, h, img, len);
    if (!box_frame(c, &c->box_lcd[at], &wr, dc_target_cap(c->m, &t))) return false;
    c->box_lcd_t[at] = t;
    return true;
}

bool dc_box_info(dc_t *c, const uint8_t *img, size_t len)
{
    if (!strip_held(c) || !deck_model_has_display(c->m) || !c->m->info_w || !img || !len) return false;
    const deck_img_target_t t = {.kind = DECK_TGT_INFO};
    deck_img_writer_t wr;
    deckproto_img_info(&wr, c->m, img, len);
    return box_frame(c, &c->box_info, &wr, dc_target_cap(c->m, &t));
}

void dc_replay_all(dc_t *c)
{
    for (int i = 0; i < DC_KEYS_MAX + DC_LCD_MAX + 1; i++) {
        dc_entry_t *e = i < DC_KEYS_MAX ? &c->key[i] : i < DC_KEYS_MAX + DC_LCD_MAX ? &c->lcd[i - DC_KEYS_MAX] : &c->info;
        if (e->kind != DC_NONE) e->dirty = true;
    }
    if (c->panel_valid) c->panel_dirty = true; /* first, at its turn: what it covered is gone */
    for (int k = 0; k < DC_KEYS_MAX; k++) c->box[k].written = false;
    for (int i = 0; i < DC_LCD_MAX; i++) c->box_lcd[i].written = false;
    c->box_info.written = false;
}

bool dc_settled(const dc_t *c)
{
    for (int i = 0; i <= DC_LCD_MAX && !strip_held(c); i++)
        if ((i < DC_LCD_MAX ? &c->lcd[i] : &c->info)->dirty) return false;
    if (c->hold) {
        for (unsigned k = 0; k < nkeys(c); k++)
            if (c->box[k].reps && !c->box[k].written) return false;
        if (c->hold_all && c->box_bright_due) return false;
        for (int i = 0; c->hold_all && i <= DC_LCD_MAX; i++) {
            const dc_box_t *b = i < DC_LCD_MAX ? &c->box_lcd[i] : &c->box_info;
            if (b->reps && !b->written) return false;
        }
        return true;
    }
    if (c->panel_dirty || c->comp_bright_due) return false;
    for (unsigned k = 0; k < nkeys(c); k++)
        if (c->key[k].dirty || c->box_drew[k]) return false;
    return true;
}

size_t dc_bytes(const dc_t *c)
{
    size_t n = 0;
    for (int k = 0; k < DC_KEYS_MAX; k++) n += (size_t)c->key[k].n + c->box[k].n;
    for (int i = 0; i < DC_LCD_MAX; i++) n += (size_t)c->lcd[i].n + c->box_lcd[i].n;
    n += (size_t)c->info.n + c->box_info.n;
    for (int i = 0; i < DC_SEQ_MAX; i++)
        if (c->seq[i].reps) n += c->seq[i].cap;
    return n * rlen(c);
}

void dc_end_sequences(dc_t *c)
{
    for (int i = 0; i < DC_SEQ_MAX; i++) seq_end(c, &c->seq[i]);
}

bool dc_box_brightness(dc_t *c, uint8_t pct)
{
    if (!c->hold_all) return false;
    c->box_bright_len = deckproto_feat_brightness(c->m, pct, c->box_bright_rep);
    c->box_bright_due = c->box_bright_len > 0;
    return c->box_bright_due;
}

size_t dc_key_image_cap(const dc_t *c, uint8_t hw)
{
    if (!c->m || hw >= DC_KEYS_MAX || hw >= nkeys(c)) return 0;
    const dc_entry_t *e = &c->key[hw];
    return e->kind == DC_IMAGE && e->reps ? (size_t)e->n * rlen(c) : 0;
}

dc_kind_t dc_key_image(const dc_t *c, uint8_t hw, uint8_t *out, size_t cap, size_t *len, uint8_t rgb[3])
{
    *len = 0;
    if (!c->m || hw >= DC_KEYS_MAX || hw >= nkeys(c)) return DC_NONE;
    const dc_entry_t *e = &c->key[hw];
    if (e->kind == DC_FILL) {
        memcpy(rgb, e->rgb, 3);
        return DC_FILL;
    }
    if (e->kind != DC_IMAGE || !e->reps) return DC_NONE;
    size_t at = 0;
    for (unsigned i = 0; i < e->n; i++) {
        deck_img_target_t t;
        uint16_t idx;
        bool last;
        const uint8_t *pl;
        size_t pn;
        if (!deckproto_parse_image_report(c->m, e->reps + (size_t)i * rlen(c), rlen(c), &t, &idx, &last, &pl, &pn) ||
            pn > cap - at)
            return DC_NONE;
        memcpy(out + at, pl, pn);
        at += pn;
    }
    *len = at;
    return DC_IMAGE;
}
