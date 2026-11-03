/* Pure: the per-model plan and the content flow (plan, flow and the
 * status tiles), values split over keys by statusui, every row aligned. */
#include <string.h>

#include "dui_int.h"

bool dui_deck_of(const deck_model_t *m, dui_deck_t *d)
{
    if (!m || m->pedal || !m->key_px || !m->cols || !m->rows) return false;
    memset(d, 0, sizeof *d);
    d->cols = m->cols;
    d->rows = m->rows;
    d->key_px = m->key_px;
    if (m->lcd_w) {
        d->bar = DUI_BAR_STRIP;
        d->bar_w = m->lcd_w;
        d->bar_h = m->lcd_h;
    } else if (m->info_w) {
        d->bar = DUI_BAR_INFO;
        d->bar_w = m->info_w;
        d->bar_h = m->info_h;
    }
    d->dials = m->encoders;
    d->touch_keys = m->touch_keys;
    return true;
}

/* ---- the tiles of the layout ---- */

/* A copy of t in L; DUI_NO_TILE when L is full. */
static int add_tile(dui_layout_t *L, const dui_tile_t *t)
{
    if (L->ntiles >= DUI_LAYOUT_TILES) {
        L->overflow = true;
        return DUI_NO_TILE;
    }
    L->tile[L->ntiles] = *t;
    return L->ntiles++;
}

static int add_button(dui_layout_t *L, int icon, const char *label, uint16_t action, uint16_t flags, int tint)
{
    dui_tile_t t;
    memset(&t, 0, sizeof t);
    t.kind = DUI_TILE_BUTTON;
    t.icon = (uint8_t)icon;
    t.text = label;
    t.action = action;
    t.flags = flags;
    t.tint = (uint8_t)tint;
    return add_tile(L, &t);
}

static int tlen(const dui_tile_t *t) { return t->text ? (t->len ? t->len : (int)strlen(t->text)) : 0; }

static int sui_role(int role)
{
    switch (role) {
    case DUI_ROLE_VALUE: return SUI_ROLE_VALUE;
    case DUI_ROLE_LABEL: return SUI_ROLE_LABEL;
    case DUI_ROLE_ACCENT: return SUI_ROLE_ACCENT;
    default: return SUI_ROLE_STATUS;
    }
}

#define PIECES_MAX 12

/* A value as the keys it takes (statusui's sui_word_cells): with spaces, whole when it reads at
 * the comfortable size on one line, else word by word. Its first piece keeps the caption; the pieces
 * are a run (they stay together on a row when they fit). Returns the tiles added. */
static int add_value(dui_layout_t *L, const dui_tile_t *t, int key_px)
{
    const int len = tlen(t);
    sui_cell_t cell[PIECES_MAX];
    int n = 0;
    if (memchr(t->text, ' ', (size_t)len) &&
        dui__font_fit(t->text, len, key_px - 2 * SUI_MARGIN, key_px * SUI_CAP_PCT / 100, sui_comfort_px(key_px), 0)) {
        cell[0].start = 0;
        cell[0].len = (uint8_t)len;
        n = 1;
    } else {
        for (int i = 0; i < len && n < PIECES_MAX;) { /* each word (no space: the whole value) */
            while (i < len && t->text[i] == ' ') i++;
            int j = i;
            while (j < len && t->text[j] != ' ') j++;
            if (j > i) {
                const int k = sui_word_cells(t->text + i, j - i, key_px, sui_role(t->role), cell + n, PIECES_MAX - n);
                for (int q = n; q < n + k; q++) cell[q].start = (uint8_t)(cell[q].start + i);
                n += k;
            }
            i = j;
        }
    }
    if (n == 0) return add_tile(L, t) == DUI_NO_TILE ? 0 : 1;
    int added = 0;
    for (int k = 0; k < n; k++) {
        dui_tile_t p = *t;
        p.flags &= (uint16_t)~DUI_F_PIECES;
        p.text = t->text + cell[k].start;
        p.len = cell[k].len;
        p.run = (uint8_t)(k == 0 && n > 1 ? n : 0);
        if (k > 0) { /* the caption on the first key only */
            p.kind = DUI_TILE_WORD;
            p.caption = NULL;
        }
        if (add_tile(L, &p) == DUI_NO_TILE) break;
        added++;
    }
    return added;
}

/* ---- the flow ---- */

typedef struct {
    dui_layout_t *L;
    int R, cols, row0; /* content rows, keys a row, the first content row on the deck */
    uint64_t res;      /* reserved content keys: bit r * cols + c */
    int p, r, i;       /* the page, its content row, the free key of that row being filled */
    bool full;         /* out of pages */
} flow_t;

static bool reserved(const flow_t *f, int r, int c) { return (f->res >> (r * f->cols + c)) & 1u; }

static int nfree(const flow_t *f, int r)
{
    int n = 0;
    for (int c = 0; r >= 0 && r < f->R && c < f->cols; c++) n += !reserved(f, r, c);
    return n;
}

static int col_of(const flow_t *f, int r, int i)
{
    for (int c = 0; c < f->cols; c++)
        if (!reserved(f, r, c) && i-- == 0) return c;
    return -1;
}

static void new_page(flow_t *f)
{
    if (f->L->pages >= DUI_PAGES_MAX) { /* the rest is dropped */
        f->full = f->L->overflow = true;
        return;
    }
    f->p = f->L->pages++;
    memset(f->L->key[f->p], DUI_NO_TILE, sizeof f->L->key[f->p]);
    f->r = 0;
    f->i = 0;
}

static void next_row(flow_t *f)
{
    f->r++;
    f->i = 0;
    if (f->r >= f->R) new_page(f);
}

static int left(const flow_t *f) { return f->full ? 0 : nfree(f, f->r) - f->i; }

static int slots_left_on_page(const flow_t *f)
{
    int n = left(f);
    for (int r = f->r + 1; r < f->R; r++) n += nfree(f, r);
    return n;
}

static int page_slots(const flow_t *f)
{
    int n = 0;
    for (int r = 0; r < f->R; r++) n += nfree(f, r);
    return n;
}

static void skip_full_rows(flow_t *f)
{
    for (int guard = 0; !f->full && left(f) <= 0 && guard < DUI_PAGES_MAX * 8; guard++) next_row(f);
}

static void put(flow_t *f, int idx)
{
    skip_full_rows(f);
    if (f->full || left(f) <= 0) return;
    f->L->key[f->p][(f->row0 + f->r) * f->cols + col_of(f, f->r, f->i)] = (uint8_t)idx;
    f->i++;
}

/* The most free keys a content row has. */
static int max_row(const flow_t *f)
{
    int m = 0;
    for (int r = 0; r < f->R; r++)
        if (nfree(f, r) > m) m = nfree(f, r);
    return m;
}

/* 1 when the run of m tiles from idx[k] ends in a port (an address and its port), else 0. */
static int port_of(const flow_t *f, const uint8_t *idx, int k, int m)
{
    return (f->L->tile[idx[k + m - 1]].flags & DUI_F_PORT) ? 1 : 0;
}

/* An address of m tiles (4 octets, a port when port) fits from content row r with a keys left there:
 * the octets on that row, the port too or on the next row of the page. */
static bool address_fits(const flow_t *f, int r, int a, int m, int port)
{
    return a >= m || (a >= m - port && (!port || (r + 1 < f->R && nfree(f, r + 1) >= 1)));
}

/* an address on a deck whose rows hold its 4 octets keeps them on one row, its port on that row
 * when it fits, else on the next row of the same page: from where the flow is, the first row where that
 * holds, on this page or the next. When no row of a page would do (a page of one row too short for the
 * port), it goes on from here. */
static void put_address(flow_t *f, const uint8_t *idx, int m, int port)
{
    bool page_has = false;
    for (int r = 0; r < f->R && !page_has; r++) page_has = address_fits(f, r, nfree(f, r), m, port);
    for (int guard = 0; page_has && !f->full && guard < 2 * f->R + 2; guard++) {
        skip_full_rows(f);
        if (f->full || address_fits(f, f->r, left(f), m, port)) break;
        next_row(f);
    }
    for (int q = 0; q < m; q++) put(f, idx[q]);
}

/* A group's tiles from where the flow is: an address whose 4 octets a row holds as put_address places it
 *; any other run (a value's pieces) that doesn't fit what is left of the row but fits the next row
 * starts it; an address wider than a row goes from a new row in balanced rows, never a lone octet,
 * on the next page when it fits a page but not the rest of this one. No real model reaches the
 * balanced-rows path today (octets over keys only on decks 4 or more keys wide, which keep them on one row); it
 * stays for a narrower deck. */
static void put_tiles(flow_t *f, const uint8_t *idx, int n)
{
    for (int k = 0; k < n && !f->full;) {
        const dui_tile_t *t = &f->L->tile[idx[k]];
        int m = t->run > 1 ? t->run : 1;
        if (k + m > n) m = n - k;
        if (m > 1 && (t->flags & DUI_F_OCTET) && max_row(f) >= m - port_of(f, idx, k, m)) {
            put_address(f, idx + k, m, port_of(f, idx, k, m));
            k += m;
            continue;
        }
        if (m > 1) {
            skip_full_rows(f);
            if (m > left(f)) {
                const int next = nfree(f, f->r + 1 < f->R ? f->r + 1 : 0);
                if (f->i > 0 && m <= next) {
                    next_row(f);
                } else if (t->flags & DUI_F_OCTET) {
                    if (f->i > 0) next_row(f);
                    /* it fits a page but not the rest of this one: the next page, not split over two */
                    if (!f->full && m > slots_left_on_page(f) && m <= page_slots(f) && f->r > 0) new_page(f);
                    for (int rem = m; rem > 0 && !f->full;) {
                        skip_full_rows(f);
                        const int room = left(f);
                        if (room <= 0) break;
                        const int rows = (rem + room - 1) / room, take = (rem + rows - 1) / rows;
                        for (int q = 0; q < take; q++) put(f, idx[k++]);
                        rem -= take;
                        if (rem > 0) next_row(f);
                    }
                    continue;
                }
            }
        }
        for (int q = 0; q < m; q++) put(f, idx[k++]);
    }
}

typedef struct {
    uint8_t first, n, flags;
} lgroup_t;

/* The flow: each group from a new row (a packed one goes on in the row when it fits);
 * an optional tile goes when its group doesn't fit its row; a group that fits a row but not the rest of
 * this one starts the next row; one that fits a page but not the rest of this one, the next page;
 * the anchored groups go to the bottom rows of the last page, else to a page of their own. */
static void flow(flow_t *f, const lgroup_t *g, int ng)
{
    dui_layout_t *L = f->L;
    L->pages = 0;
    f->full = false;
    new_page(f);
    const int widest = max_row(f);
    bool first = true;
    uint8_t idx[DUI_LAYOUT_TILES];
    for (int k = 0; k < ng; k++) {
        if (g[k].flags & DUI_G_ANCHOR || !g[k].n) continue;
        int n = 0;
        for (int i = 0; i < g[k].n; i++) idx[n++] = (uint8_t)(g[k].first + i);
        if (!first && !((g[k].flags & DUI_G_PACK) && left(f) >= n)) next_row(f);
        first = false;
        skip_full_rows(f);
        if (n > left(f)) { /* without its optional tiles */
            int m = 0;
            for (int i = 0; i < n; i++)
                if (!(L->tile[idx[i]].flags & DUI_F_OPTIONAL)) idx[m++] = idx[i];
            n = m;
        }
        if (n > left(f) && f->i > 0 && n <= widest) next_row(f);
        if (n > slots_left_on_page(f) && n <= page_slots(f) && (f->r > 0 || f->i > 0)) new_page(f);
        put_tiles(f, idx, n);
    }
    int na = 0; /* the anchored groups' tiles */
    for (int k = 0; k < ng; k++)
        if (g[k].flags & DUI_G_ANCHOR) na += g[k].n;
    if (!na || f->full) return;
    const int used = f->i > 0 ? f->r + 1 : f->r;
    int start = 0;
    for (int s = f->R - 1; s >= 0; s--) {
        int c = 0;
        for (int r = s; r < f->R; r++) c += nfree(f, r);
        if (c >= na) {
            start = s;
            break;
        }
    }
    if (start < used) new_page(f);
    if (f->full) return;
    f->r = start;
    f->i = 0;
    bool any = false;
    for (int k = 0; k < ng; k++) {
        if (!(g[k].flags & DUI_G_ANCHOR) || !g[k].n) continue;
        if (any && g[k].n > left(f) && f->r + 1 < f->R) {
            f->r++;
            f->i = 0;
        }
        any = true;
        int n = 0;
        for (int i = 0; i < g[k].n; i++) idx[n++] = (uint8_t)(g[k].first + i);
        put_tiles(f, idx, n);
    }
}

/* ---- the status row on the keys (statusTiles) ---- */

static int count_words(const char *const *w)
{
    int n = 0;
    while (n < DUI_STATUS_WORDS && w[n]) n++;
    return n;
}

static int state_tint(int state)
{
    return state == DUI_STATE_BAD ? DUI_TINT_BAD : state == DUI_STATE_WAIT ? DUI_TINT_WAIT
           : state == DUI_STATE_OK ? DUI_TINT_OK : DUI_TINT_BLACK;
}

static int state_role(int state)
{
    return state == DUI_STATE_BAD ? DUI_ROLE_BAD : state == DUI_STATE_WAIT ? DUI_ROLE_WAIT
           : state == DUI_STATE_OK ? DUI_ROLE_OK : DUI_ROLE_VALUE;
}

/* s[0..len) in at most max pieces (1 or 2): whole, or split at the space that balances the two (a toast's words). */
static bool space_of(char ch, bool tabs) { return ch == ' ' || (tabs && ch == '\t'); }

/* s[0..len) trimmed and, with split, cut in two at the space that balances its halves (the longer one shortest),
 * each trimmed again; else, or with no space, one piece. tabs: a tab is a space too. The pieces as offsets and
 * lengths; their count. */
static int balance2(const char *s, int len, bool split, bool tabs, int *st, int *ln)
{
    int a = 0, e = len, best = -1, worst = 1 << 30;
    while (a < e && space_of(s[a], tabs)) a++;
    while (e > a && space_of(s[e - 1], tabs)) e--;
    for (int i = a; split && i < e; i++)
        if (space_of(s[i], tabs)) {
            const int m = i - a > e - i - 1 ? i - a : e - i - 1;
            if (m < worst) worst = m, best = i;
        }
    if (best < 0) {
        st[0] = a, ln[0] = e - a;
        return 1;
    }
    int le = best, rs = best + 1;
    while (le > a && space_of(s[le - 1], tabs)) le--;
    while (rs < e && space_of(s[rs], tabs)) rs++;
    st[0] = a, ln[0] = le - a, st[1] = rs, ln[1] = e - rs;
    return 2;
}

static int split2(const char *s, int len, int max, int *st, int *ln)
{
    int a = 0, e = len;
    while (a < e && s[a] == ' ') a++;
    while (e > a && s[e - 1] == ' ') e--;
    for (int i = a; max > 1 && i + 4 <= e; i++) /* "Unlocked · hold Restart": at the " · ", the dot dropped */
        if (!memcmp(s + i, " \xC2\xB7 ", 4)) {
            st[0] = a, ln[0] = i - a, st[1] = i + 4, ln[1] = e - i - 4;
            return 2;
        }
    return balance2(s, len, max > 1, false, st, ln);
}

/* the status row's signs, right-aligned in a fixed order: the hotspot (always: crossed in grey when off), Wi-Fi
 * (its bars when joined; crossed in grey when off by choice, in red when on and not joined), Ethernet (while its
 * cable is linked). Into ind / prio in that order; prio is how long each is kept when the row has no room for all:
 * the sign carrying the route (Ethernet, else Wi-Fi, crossed when there is no route) the longest, the hotspot the
 * least. Returns how many. */
static int status_signs(dui_layout_t *L, const dui_status_t *st, int tint, int *ind, int *prio)
{
    dui__sign_t sg[3];
    const int n = dui__signs(st, sg);
    for (int i = 0; i < n; i++) {
        dui_tile_t t;
        memset(&t, 0, sizeof t);
        t.kind = DUI_TILE_IND;
        t.tint = (uint8_t)tint;
        t.icon = sg[i].icon;
        t.rssi = sg[i].icon == DUI_ICON_WIFI ? st->rssi : 0;
        t.action = sg[i].action; /* a tap goes to its screen (Wi-Fi and Ethernet: the Network screen) */
        prio[i] = sg[i].prio;
        ind[i] = add_tile(L, &t);
    }
    return n;
}

/* reserve: a column of the row another key takes (the 32-key Network screen's Wi-Fi key), -1 none: the signs stay right
 * of it. */
static void status_row(dui_layout_t *L, const dui_screen_t *s, uint8_t *row, int reserve)
{
    const dui_status_t *st = &s->status;
    const int cols = L->deck.cols;
    const bool menu_key = L->menu_style == DUI_MENU_STATUS, sub = s->sub, close = !sub && st->close;
    /* a toast takes the status row over for its time: its outcome's tint and colour, its words (one or two
     * keys) in place of the status or title words, the glyph (home) its outcome's; Back, Close, Menu and the
     * indicators stay. Never beside the status words, never dropped. */
    const bool toast = st->toast && st->toast[0];
    const int ts = st->toast_state != DUI_STATE_NONE ? st->toast_state : DUI_STATE_OK; /* no outcome given: done */
    const int tint = toast ? state_tint(ts) : sub ? DUI_TINT_NEUTRAL : state_tint(st->state);
    const char *const *words = sub ? st->title : st->word;
    /* a sub-screen's title in one key ("Scan to" / "join"), at the title size like every title */
    const char *one_title[2] = {NULL, NULL};
    if (sub && !toast && count_words(words) > 1) {
        dui__join_words(words, L->title, (int)sizeof L->title);
        one_title[0] = L->title;
        words = one_title;
    }
    int nw = toast ? 2 : count_words(words);
    const bool has_short = !sub && st->short_word[0];
    if (!toast && menu_key && nw + 1 > cols && has_short) {
        words = st->short_word;
        nw = count_words(words);
    }
    int ind[3], prio[3] = {0}, nind = 0; /* the signs, or the Menu key */
    dui_tile_t t;
    if (menu_key) ind[nind++] = add_button(L, DUI_ICON_MENU, "Menu", DUI_ACT_MENU, 0, tint);
    else nind = status_signs(L, st, tint, ind, prio);
    int lead = DUI_NO_TILE; /* Back, Close or the glyph */
    if (sub || close) {
        lead = add_button(L, sub ? DUI_ICON_BACK : DUI_ICON_CLOSE, NULL, sub ? DUI_ACT_BACK : DUI_ACT_CLOSE, DUI_F_NAV, tint);
        L->back_col = 0;
        L->back_row = 0;
    } else if (toast || st->state != DUI_STATE_NONE) {
        memset(&t, 0, sizeof t);
        t.kind = DUI_TILE_GLYPH;
        t.state = (uint8_t)(toast ? ts : st->state);
        t.phase = st->phase;
        t.tint = (uint8_t)tint;
        lead = add_tile(L, &t);
    }
    int nlead = lead != DUI_NO_TILE, nshow = nind; /* the signs shown: the nshow longest kept (prio) */
    if (!menu_key && reserve >= 0)
        while (nshow > cols - 1 - reserve && nshow > 0) nshow--;
    if (toast && nlead + nw + nshow > cols) nw = 1; /* a toast in one key (two lines) before an indicator goes */
    /* narrow decks: the indicators go first (the hotspot's first, the route's last), then the glyph */
    while (nlead + nw + nshow > cols && nshow > 0 && !menu_key) nshow--;
    if (nlead + nw + nshow > cols && !sub && !close) nlead = 0;
    if (!toast && nlead + nw + nshow > cols && has_short && words != st->short_word) {
        words = st->short_word;
        nw = count_words(words);
    }
    while (nlead + nw + nshow > cols && nw > 0) nw--;
    int c = 0;
    if (nlead) row[c++] = (uint8_t)lead;
    int pst[2] = {0}, pln[2] = {0};
    if (toast) nw = split2(st->toast, (int)strlen(st->toast), nw < 1 ? 1 : nw, pst, pln);
    for (int i = 0; i < nw && c < cols; i++) {
        memset(&t, 0, sizeof t);
        t.kind = DUI_TILE_WORD;
        t.text = toast ? st->toast + pst[i] : words[i];
        t.len = (uint8_t)(toast ? pln[i] : 0);
        t.role = (uint8_t)(toast ? state_role(ts) : sub ? DUI_ROLE_VALUE : state_role(st->state));
        t.tint = (uint8_t)tint;
        row[c++] = (uint8_t)add_tile(L, &t);
    }
    while (c < cols - nshow) {
        memset(&t, 0, sizeof t);
        t.kind = DUI_TILE_FILL;
        t.tint = (uint8_t)tint;
        row[c++] = (uint8_t)add_tile(L, &t);
    }
    /* the nshow signs kept longest, in their fixed order */
    bool shown[3] = {true, true, true};
    if (!menu_key) {
        dui__sign_t sg[3];
        for (int i = 0; i < nind; i++) sg[i].prio = (uint8_t)prio[i];
        dui__signs_keep(sg, nind, nshow, shown);
    }
    for (int i = 0; i < nind && c < cols; i++) {
        if (!shown[i]) continue;
        if (menu_key) {
            L->menu_col = (int8_t)c;
            L->menu_row = 0;
        }
        row[c++] = (uint8_t)ind[i];
    }
}

/* ---- The Network screen's fixed key map ---- */

/* The models with a map, by their sub-screen's grid: the 32-key decks (8 x 4 under a status row), the + XL (9 x 4,
 * Back top left), the 15-key decks (5 x 3 under a status row, a ">" key), the + and the Neo (4 x 2, Back top left,
 * dials or touch keys page), the Mini and the Module 6 (3 x 2, Back with the title top left, a ">" key). */
enum { KM_NONE, KM_W8, KM_W9, KM_M5, KM_S4, KM_T3 };

static int km_class(const dui_layout_t *L)
{
    const dui_deck_t *d = &L->deck;
    if (d->cols == 8 && d->rows == 4 && L->status_rows == 1 && L->pager == DUI_PAGER_KEY) return KM_W8;
    if (d->cols == 9 && d->rows == 4 && L->status_rows == 0) return KM_W9;
    if (d->cols == 5 && d->rows == 3 && L->status_rows == 1 && L->pager == DUI_PAGER_KEY) return KM_M5;
    if (d->cols == 4 && d->rows == 2 && L->status_rows == 0 && L->pager == DUI_PAGER_HW) return KM_S4;
    if (d->cols == 3 && d->rows == 2 && L->status_rows == 0 && L->pager == DUI_PAGER_KEY) return KM_T3;
    return KM_NONE;
}

/* Where map row n's role goes (its k-th key): page, row, column. The wide decks: one row per network, the icon, the
 * name (two keys), the state, the action, Edit / SIGNAL, the address (two keys). The 15-key decks: a page per
 * network, the same order over two rows. The + and the Neo: a page per network, the icon, the name and the state,
 * then the action, Edit or the Wi-Fi key, the address (the signal stays in the icon's bars). The Mini: a page per
 * network (icon, name, state, action), then a page of its details (Edit or the Wi-Fi key, the signal, the
 * address) when it has any. False: not shown on this model; *status: the status row's key (the 32-key decks'
 * Wi-Fi key). */
static bool km_pos(int cls, int n, int role, int k, const int *pmain, const int *pdet, int *p, int *r, int *c, bool *status)
{
    *status = false;
    switch (cls) {
    case KM_W8:
    case KM_W9: {
        static const int8_t col[] = {0, 0, 1, 3, 4, 5, 6};
        if (role == DUI_MC_TOGGLE) {
            *p = 0, *r = 0, *c = cls == KM_W8 ? 5 : 8;
            *status = cls == KM_W8;
            return true;
        }
        if (role < DUI_MC_ICON || role > DUI_MC_IP) return false;
        *p = 0, *r = n, *c = col[role] + k;
        return true;
    }
    case KM_M5: {
        static const int8_t row[] = {0, 1, 1, 1, 1, 2, 2, 2}, col[] = {0, 0, 1, 3, 4, 0, 1, 3};
        if (role < DUI_MC_ICON || role > DUI_MC_TOGGLE) return false;
        *p = pmain[n], *r = row[role], *c = col[role] + k;
        return true;
    }
    case KM_S4: {
        static const int8_t row[] = {0, 0, 0, 0, 1, 1, 1, 1}, col[] = {0, 1, 2, 3, 0, 1, 2, 1};
        if (role < DUI_MC_ICON || role > DUI_MC_TOGGLE || (role == DUI_MC_AUX && n != DUI_MAP_ETH)) return false;
        *p = pmain[n], *r = row[role], *c = col[role] + k;
        return true;
    }
    case KM_T3:
        switch (role) {
        case DUI_MC_ICON: *p = pmain[n], *r = 0, *c = 1; return true;
        case DUI_MC_NAME: *p = pmain[n], *r = 0, *c = 2; return true;
        case DUI_MC_STATE: *p = pmain[n], *r = 1, *c = 0; return true;
        case DUI_MC_ACTION: *p = pmain[n], *r = 1, *c = 1; return true;
        case DUI_MC_AUX: *p = pdet[n], *r = 0, *c = n == DUI_MAP_ETH ? 1 : 2; return true;
        case DUI_MC_TOGGLE: *p = pdet[n], *r = 0, *c = 1; return true;
        case DUI_MC_IP: *p = pdet[n], *r = 1, *c = k; return true;
        default: return false;
        }
    default: return false;
    }
}

static void km_put(dui_layout_t *L, int p, int r, int c, int idx)
{
    if (idx == DUI_NO_TILE || p < 0 || p >= L->pages || r < 0 || r >= L->deck.rows || c < 0 || c >= L->deck.cols) return;
    L->key[p][r * L->deck.cols + c] = (uint8_t)idx;
}

/* A Wi-Fi name on span keys: one key when span is 1 or it is one word; else split at the space that balances its two
 * keys (whole words; two words one a key). Returns the pieces, as offsets and lengths. */
static int km_name(const dui_tile_t *t, int span, int *st, int *ln)
{
    return balance2(t->text, tlen(t), span > 1, true, st, ln);
}

/* the pair from the octet tiles s->tile[i..i+3] (*t: the first, as it will be placed): *t up to the second
 * octet with its dot ("192.168."), *b a copy of it holding the last two ("8.225"), with no caption. */
static void ip_pair(const dui_screen_t *s, int i, dui_tile_t *t, dui_tile_t *b)
{
    const dui_tile_t *o2 = &s->tile[i + 1], *last = &s->tile[i + 3];
    *b = *t;
    t->len = (uint8_t)(o2->text + tlen(o2) + 1 - t->text);
    b->text = s->tile[i + 2].text;
    b->len = (uint8_t)(last->text + tlen(last) - b->text);
    b->caption = NULL;
}

/* The Network screen by the model's map (dui_build_network sets s->keymap): false when the model has none, or a Wi-Fi
 * name can't read at 16 px on its keys (the flow places it then, splitting the name over keys). The 32-key
 * decks' Wi-Fi key goes into the status row: *status_col, *status_idx. */
static bool plan_keymap(const dui_screen_t *s, dui_layout_t *L, int *status_col, int *status_idx)
{
    const int cls = km_class(L);
    if (cls == KM_NONE) return false;
    const int cols = L->deck.cols, key_px = L->deck.key_px;
    const bool small = cols * L->deck.rows <= DUI_SMALL_KEYS;
    const int span = cls == KM_S4 || cls == KM_T3 ? 1 : 2;
    int pmain[DUI_MAP_TOP + 1] = {0}, pdet[DUI_MAP_TOP + 1] = {0}, pages = 0, plocal, lrow, lcol, lmax = cols;
    for (int n = DUI_MAP_ETH; n <= DUI_MAP_BACKUP; n++) {
        if (cls == KM_W8 || cls == KM_W9) continue;
        if (cls != KM_T3) {
            pmain[n] = pdet[n] = n - 1;
            continue;
        }
        pmain[n] = pages++;
        pdet[n] = pages++; /* the Mini: every network's detail page, always, so no page (".local" last) ever moves */
    }
    switch (cls) {
    case KM_W8: pages = 2, plocal = 1, lrow = 1, lcol = 0; break;
    case KM_W9: pages = 1, plocal = 0, lrow = 0, lcol = 1, lmax = cols - 1; break; /* the Wi-Fi key at the end */
    case KM_M5: pages = 4, plocal = 3, lrow = 1, lcol = 0; break;
    case KM_S4: pages = 4, plocal = 3, lrow = 0, lcol = 1; break;
    default: plocal = pages++, lrow = 0, lcol = 1; break;
    }
    if (pages > DUI_PAGES_MAX) return false;
    for (int i = 0; i < s->ntiles; i++) { /* a Wi-Fi name that can't read at 16 px on its keys: the flow */
        const dui_tile_t *t = &s->tile[i];
        if ((t->map & 15) != DUI_MC_NAME || t->caption || !(t->flags & DUI_F_PIECES)) continue;
        int st[2], ln[2];
        const int k = km_name(t, span, st, ln);
        for (int q = 0; q < k; q++)
            if (!dui__value_fits(t->text + st[q], ln[q], key_px)) return false;
    }
    L->pages = (uint8_t)pages;
    *status_col = -1;
    *status_idx = DUI_NO_TILE;
    for (int i = 0; i < s->ntiles; i++) {
        const dui_tile_t *src = &s->tile[i];
        if (!src->map) continue;
        const int n = src->map >> 4, role = src->map & 15;
        dui_tile_t t = *src;
        t.tint = (uint8_t)(t.kind == DUI_TILE_BUTTON ? DUI_TINT_BLACK : DUI_TINT_MAIN);
        t.run = 0;
        if (role == DUI_MC_LOCAL) { /* the box's name (one key on a small deck, else the pieces) and ".local" */
            const int first = L->ntiles;
            if ((t.flags & DUI_F_PIECES) && tlen(&t) > 0 && !(small && dui__value_fits(t.text, tlen(&t), key_px)))
                add_value(L, &t, key_px);
            else {
                t.flags &= (uint16_t)~DUI_F_PIECES;
                add_tile(L, &t);
            }
            for (int j = first; j < L->ntiles && lcol < lmax; j++) {
                L->tile[j].run = 0;
                km_put(L, plocal, lrow, lcol++, j);
            }
            continue;
        }
        int p, r, c;
        bool status;
        if (role == DUI_MC_IP) { /* the pair on every model here: its two keys are kept for it ("192.168." | "8.225") */
            if (!(t.flags & DUI_F_OCTET) || i + 3 >= s->ntiles) continue;
            dui_tile_t b2;
            t.flags = (uint16_t)((t.flags & ~DUI_F_OCTET) | DUI_F_IP);
            ip_pair(s, i, &t, &b2);
            i += 3;
            if (km_pos(cls, n, role, 0, pmain, pdet, &p, &r, &c, &status)) km_put(L, p, r, c, add_tile(L, &t));
            if (km_pos(cls, n, role, 1, pmain, pdet, &p, &r, &c, &status)) km_put(L, p, r, c, add_tile(L, &b2));
            continue;
        }
        if (role == DUI_MC_NAME && t.caption == NULL && (t.flags & DUI_F_PIECES)) { /* a Wi-Fi name on its keys */
            int st[2], ln[2];
            const int k = km_name(&t, span, st, ln);
            for (int q = 0; q < k; q++) {
                dui_tile_t w = t;
                w.flags &= (uint16_t)~DUI_F_PIECES;
                w.text = t.text + st[q];
                w.len = (uint8_t)ln[q];
                if (km_pos(cls, n, role, q, pmain, pdet, &p, &r, &c, &status)) km_put(L, p, r, c, add_tile(L, &w));
            }
            continue;
        }
        t.flags &= (uint16_t)~DUI_F_PIECES;
        if (!km_pos(cls, n, role, 0, pmain, pdet, &p, &r, &c, &status)) continue;
        if (status) {
            *status_col = c;
            *status_idx = add_tile(L, &t);
        } else {
            km_put(L, p, r, c, add_tile(L, &t));
        }
    }
    if (cls == KM_T3) /* a detail page with nothing on it: the network's state, not a blank page ("Not set") */
        for (int n = DUI_MAP_ETH; n <= DUI_MAP_BACKUP; n++) {
            const uint8_t *k = L->key[pdet[n]];
            if (k[1] != DUI_NO_TILE || k[2] != DUI_NO_TILE || k[3] != DUI_NO_TILE || k[4] != DUI_NO_TILE) continue;
            for (int i = 0; i < s->ntiles; i++)
                if (s->tile[i].map == DUI_MAP(n, DUI_MC_STATE)) {
                    dui_tile_t t = s->tile[i];
                    t.tint = DUI_TINT_MAIN;
                    km_put(L, pdet[n], 0, 1, add_tile(L, &t));
                    break;
                }
        }
    if (pages > 1 && L->pager == DUI_PAGER_KEY) { /* the ">" key, in one place on every page */
        L->pager_col = (int8_t)(cls == KM_W8 ? 4 : cls == KM_M5 ? 4 : 2);
        L->pager_row = (int8_t)(cls == KM_W8 ? 0 : cls == KM_M5 ? 2 : 1);
    }
    return true;
}

/* ---- the plan ---- */

/* The menu's style on a screen with a menu, status_rows key rows under the status. */
static int menu_style(const dui_deck_t *d, int status_rows)
{
    const int avail = d->rows - status_rows;
    if (status_rows == 1 && d->cols <= 3) return DUI_MENU_STATUS; /* tiny: the Menu key in the status row */
    return (avail >= 3 || (avail == 2 && d->cols >= 5)) ? DUI_MENU_ROW : DUI_MENU_KEY;
}

int dui_home_menu(const dui_deck_t *d) { return menu_style(d, d->bar == DUI_BAR_NONE ? 1 : 0); }

/* A lead label (dui_lead): a word carrying the caption its next tile takes when it is dropped. */
static bool is_lead(const dui_tile_t *t) { return t->kind == DUI_TILE_WORD && t->hint && t->hint[0]; }

/* The page tile i is placed on; -1: none. */
static int page_of(const dui_layout_t *L, int i)
{
    const int keys = L->deck.cols * L->deck.rows;
    for (int p = 0; p < L->pages; p++)
        for (int k = 0; k < keys; k++)
            if (L->key[p][k] == i) return p;
    return -1;
}

/* a lead label placed on another page than the address after it (the run it leads). A lead before anything
 * but an address stays, wherever it lands: its caption ("Companion IP") would name the wrong thing. */
static bool lead_apart(const dui_layout_t *L)
{
    for (int i = 0; i + 1 < L->ntiles; i++)
        if (is_lead(&L->tile[i]) && (L->tile[i + 1].flags & (DUI_F_OCTET | DUI_F_IP))) {
            const int p = page_of(L, i);
            if (p >= 0 && page_of(L, i + 1) != p) return true;
        }
    return false;
}

static void plan_once(const dui_deck_t *d, const dui_screen_t *s, dui_layout_t *L, bool drop_leads);

void dui_plan(const dui_deck_t *d, const dui_screen_t *s, dui_layout_t *L)
{
    plan_once(d, s, L, false);
    /* a lead label that would sit apart from what it leads goes; that run's first key takes its caption */
    if (lead_apart(L)) plan_once(d, s, L, true);
}

static void plan_once(const dui_deck_t *d, const dui_screen_t *s, dui_layout_t *L, bool drop_leads)
{
    memset(L, 0, sizeof *L);
    L->deck = *d;
    L->menu_col = L->menu_row = L->back_col = L->back_row = L->pager_col = L->pager_row = -1;
    const int cols = d->cols, rows = d->rows, keys = cols * rows;
    L->pages = 1;
    L->overflow = s->overflow;
    memset(L->key, DUI_NO_TILE, sizeof L->key);
    if (cols < 1 || rows < 1 || keys > DUI_KEYS_MAX) return;

    const bool on_keys = d->bar == DUI_BAR_NONE;
    L->status_at = d->bar == DUI_BAR_STRIP ? DUI_STATUS_STRIP : d->bar == DUI_BAR_INFO ? DUI_STATUS_INFO : DUI_STATUS_KEYS;
    L->full = s->full || (s->sub && keys <= 6); /* tiny decks: sub-screens take the whole grid */
    L->status_rows = on_keys && !L->full ? 1 : 0;
    const int avail = rows - L->status_rows;
    const bool tiny = L->status_rows == 1 && cols <= 3;
    L->menu_style = !s->menu ? DUI_MENU_NONE : menu_style(d, L->status_rows);
    const int R = avail - (L->menu_style == DUI_MENU_ROW);
    L->content_rows = (uint8_t)(R > 0 ? R : 0);
    L->pager = L->menu_style == DUI_MENU_ROW ? DUI_PAGER_MENUROW
               : (d->touch_keys || d->dials) ? DUI_PAGER_HW : tiny ? DUI_PAGER_TAP : DUI_PAGER_KEY;

    /* the reserved content keys: Menu bottom-right; Back (or Close) top-left with no status row */
    flow_t f = {L, R, cols, L->status_rows, 0, 0, 0, 0, false};
    int menu_idx = DUI_NO_TILE, back_idx = DUI_NO_TILE;
    if (R > 0 && L->menu_style == DUI_MENU_KEY) {
        menu_idx = add_button(L, DUI_ICON_MENU, "Menu", DUI_ACT_MENU, 0, DUI_TINT_BLACK);
        f.res |= 1ull << ((R - 1) * cols + cols - 1);
        L->menu_col = (int8_t)(cols - 1);
        L->menu_row = (int8_t)(L->status_rows + R - 1);
    }
    if (R > 0 && L->status_rows == 0 && (s->sub || s->status.close)) {
        const char *label = NULL;
        if (s->sub && L->full && keys <= 6) { /* the Back key carries the title */
            dui__join_words(s->status.title, L->title, (int)sizeof L->title);
            label = L->title;
        }
        /* no status row on the keys (the Mini's sub-screens, a full screen): the toast in the Back (or Close)
         * key, in its outcome's colour, for its time; the key still goes back */
        const bool toast = on_keys && s->status.toast && s->status.toast[0];
        if (toast) label = s->status.toast;
        back_idx = add_button(L, s->sub ? DUI_ICON_BACK : DUI_ICON_CLOSE, label, s->sub ? DUI_ACT_BACK : DUI_ACT_CLOSE,
                              DUI_F_NAV, DUI_TINT_BLACK);
        if (toast && back_idx != DUI_NO_TILE)
            L->tile[back_idx].state = s->status.toast_state != DUI_STATE_NONE ? s->status.toast_state : DUI_STATE_OK;
        f.res |= 1ull;
        L->back_col = 0;
        L->back_row = 0;
    }

    /* the content: values split over keys, each tile on its zone's tint (a button on black). on a small
     * deck a value stays on one key (2 lines at its natural break), as long as it reads at DUI_TEXT_MIN_PX.
     * An IPv4 address on a small deck takes one key when it fits there on one line at
     * DUI_TEXT_MIN_PX, else two keys of two octets each, "192.168." | "8.225", one line each, together (a run:
     * one row, one page; its port stays on its own key after them) */
    const bool small = keys <= DUI_SMALL_KEYS;
    int km_col = -1, km_idx = DUI_NO_TILE; /* the Network screen by the model's key map */
    const bool keymapped = s->keymap && R > 0 && plan_keymap(s, L, &km_col, &km_idx);
    lgroup_t g[DUI_GROUPS_MAX];
    int ng = 0;
    for (int k = 0; !keymapped && k < s->ngroups && k < DUI_GROUPS_MAX; k++) {
        const dui_group_t *sg = &s->group[k];
        g[ng].first = L->ntiles;
        g[ng].flags = sg->flags;
        const int tint = sg->zone == DUI_ZONE_MAIN ? DUI_TINT_MAIN : sg->zone == DUI_ZONE_SVC ? DUI_TINT_SVC : DUI_TINT_BLACK;
        const int end = sg->first + sg->n < s->ntiles ? sg->first + sg->n : s->ntiles;
        const char *lead_cap = NULL; /* a dropped lead label's caption, for the next tile */
        for (int i = sg->first; i < end; i++) {
            dui_tile_t t = s->tile[i];
            if (drop_leads && is_lead(&t) && i + 1 < end && (s->tile[i + 1].flags & DUI_F_OCTET)) { /* an address next */
                lead_cap = t.hint;
                continue;
            }
            if (lead_cap) {
                if (t.kind == DUI_TILE_WORD) t.kind = DUI_TILE_CAP;
                t.caption = lead_cap;
                lead_cap = NULL;
            }
            t.tint = (uint8_t)(t.kind == DUI_TILE_BUTTON ? DUI_TINT_BLACK : tint);
            if (small && (t.flags & DUI_F_OCTET) && t.run >= 4 && i + 3 < end) { /* one key, or a pair */
                const dui_tile_t *last = &s->tile[i + 3];
                const int full = (int)(last->text + tlen(last) - t.text), port = t.run == 5;
                t.flags = (uint16_t)((t.flags & ~DUI_F_OCTET) | DUI_F_IP);
                if (dui__font_fit(t.text, full, d->key_px - 2 * SUI_MARGIN, d->key_px * 40 / 96, DUI_TEXT_MIN_PX, 0)) {
                    t.len = (uint8_t)full;
                    t.run = port ? 2 : 0; /* with its port: a run of two */
                    add_tile(L, &t);
                } else {
                    dui_tile_t b;
                    ip_pair(s, i, &t, &b); /* "192.168.", its dot kept; "8.225" */
                    t.run = (uint8_t)(port ? 3 : 2);
                    b.run = 0;
                    add_tile(L, &t);
                    add_tile(L, &b);
                }
                i += 3;
            } else if ((t.flags & DUI_F_PIECES) && tlen(&t) > 0) {
                /* a value on the small decks, a Wi-Fi name's word on every deck: one key, 2 lines inside it */
                if ((small || (t.flags & DUI_F_WHOLE)) && dui__value_fits(t.text, tlen(&t), d->key_px)) {
                    t.flags &= (uint16_t)~DUI_F_PIECES;
                    add_tile(L, &t);
                } else if ((t.flags & DUI_F_WHOLE) && memchr(t.text, ' ', (size_t)tlen(&t))) {
                    /* a whole Wi-Fi name (the Network screen's, when its key map can't hold it): word by word,
                     * each one key when it reads at 16 px, over keys only as the last resort; one run */
                    const int first = L->ntiles, len = tlen(&t);
                    for (int a = 0; a < len;) {
                        while (a < len && t.text[a] == ' ') a++;
                        int e = a;
                        while (e < len && t.text[e] != ' ') e++;
                        if (e > a) {
                            dui_tile_t w = t;
                            w.text = t.text + a;
                            w.len = (uint8_t)(e - a);
                            if (L->ntiles > first) w.caption = NULL;
                            if (dui__value_fits(w.text, w.len, d->key_px)) {
                                w.flags &= (uint16_t)~DUI_F_PIECES;
                                add_tile(L, &w);
                            } else {
                                add_value(L, &w, d->key_px);
                            }
                        }
                        a = e;
                    }
                    for (int j = first; j < L->ntiles; j++) L->tile[j].run = 0;
                    if (L->ntiles - first > 1) L->tile[first].run = (uint8_t)(L->ntiles - first);
                } else {
                    add_value(L, &t, d->key_px); /* a big deck, or too long for one key at 16 px */
                }
            } else {
                add_tile(L, &t);
            }
        }
        g[ng].n = (uint8_t)(L->ntiles - g[ng].first);
        ng++;
    }
    if (R > 0 && !keymapped) {
        flow(&f, g, ng);
        if (L->pages > 1 && L->pager == DUI_PAGER_KEY) { /* a ">" key: the last free key of the content */
            for (int c = cols - 1; c >= 0; c--)
                if (!reserved(&f, R - 1, c)) {
                    f.res |= 1ull << ((R - 1) * cols + c);
                    L->pager_col = (int8_t)c;
                    L->pager_row = (int8_t)(L->status_rows + R - 1);
                    break;
                }
            flow(&f, g, ng);
        }
    }

    /* the rows on every page: the status, the menu row; the reserved keys */
    uint8_t srow[DUI_KEYS_MAX], mrow[DUI_KEYS_MAX];
    memset(srow, DUI_NO_TILE, sizeof srow);
    memset(mrow, DUI_NO_TILE, sizeof mrow);
    if (L->status_rows) status_row(L, s, srow, km_idx != DUI_NO_TILE ? km_col : -1);
    if (km_idx != DUI_NO_TILE && km_col >= 0 && km_col < cols) srow[km_col] = (uint8_t)km_idx; /* the Wi-Fi key */
    if (L->menu_style == DUI_MENU_ROW) {
        for (int i = 0; i < s->nmenu && i < cols; i++) {
            dui_tile_t t = s->menu_tile[i];
            t.tint = DUI_TINT_BLACK;
            mrow[i] = (uint8_t)add_tile(L, &t);
        }
        if (L->pages > 1) {
            dui_tile_t t;
            memset(&t, 0, sizeof t);
            t.kind = DUI_TILE_PAGER;
            t.action = DUI_ACT_PAGER;
            mrow[cols - 1] = (uint8_t)add_tile(L, &t);
            L->pager_col = (int8_t)(cols - 1);
            L->pager_row = (int8_t)(rows - 1);
        }
    }
    int pager_idx = DUI_NO_TILE;
    if (L->pager_col >= 0 && L->pager == DUI_PAGER_KEY) {
        dui_tile_t t;
        memset(&t, 0, sizeof t);
        t.kind = DUI_TILE_PAGER;
        t.action = DUI_ACT_PAGER;
        pager_idx = add_tile(L, &t);
    }
    for (int p = 0; p < L->pages; p++) {
        uint8_t *k = L->key[p];
        if (L->status_rows) memcpy(k, srow, (size_t)cols);
        if (L->menu_style == DUI_MENU_ROW) memcpy(k + (rows - 1) * cols, mrow, (size_t)cols);
        if (menu_idx != DUI_NO_TILE) k[L->menu_row * cols + L->menu_col] = (uint8_t)menu_idx;
        if (back_idx != DUI_NO_TILE) k[0] = (uint8_t)back_idx;
        if (pager_idx != DUI_NO_TILE) k[L->pager_row * cols + L->pager_col] = (uint8_t)pager_idx;
    }

    /* every row aligned */
    for (int p = 0; p < L->pages; p++)
        for (int r = 0; r < rows; r++) {
            dui_tile_t *row[9];
            int n = 0;
            for (int c = 0; c < cols && n < 9; c++) {
                const uint8_t i = L->key[p][r * cols + c];
                row[n++] = i == DUI_NO_TILE ? NULL : &L->tile[i];
            }
            if (r < L->status_rows) {
                /* one size per model. The status row holds the same tiles on every page (and only
                 * there), and a tile's status fit depends on nothing else: once */
                if (p == 0) dui_align_status(row, n, d->key_px);
            } else {
                dui_align_row(row, n, d->key_px);
            }
        }
}

bool dui_layout_key(const dui_layout_t *L, int page, int col, int row, dui_tile_t *out)
{
    memset(out, 0, sizeof *out);
    if (col < 0 || row < 0 || col >= L->deck.cols || row >= L->deck.rows || L->pages < 1) return false;
    if (page < 0) page = 0;
    page %= L->pages;
    const uint8_t i = L->key[page][row * L->deck.cols + col];
    if (i == DUI_NO_TILE || i >= L->ntiles) return false;
    *out = L->tile[i];
    if (out->kind == DUI_TILE_PAGER) {
        out->num = L->pages;
        out->cur = (uint8_t)page;
    }
    return true;
}
