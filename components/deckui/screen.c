/* Pure: the screen builders (deckui.h): tiles in groups, their strings copied into the screen's pool. */
#include "deckui/deckui.h"

#include <string.h>

void dui_screen_init(dui_screen_t *s) { memset(s, 0, sizeof *s); }

const char *dui_str(dui_screen_t *s, const char *text)
{
    if (!text) return "";
    const size_t n = strlen(text);
    if (s->pool_used + n + 1 > sizeof s->pool) {
        s->overflow = true;
        return "";
    }
    char *dst = s->pool + s->pool_used;
    memcpy(dst, text, n + 1);
    s->pool_used = (uint16_t)(s->pool_used + n + 1);
    return dst;
}

/* Splits words (space-separated) into out[DUI_STATUS_WORDS], each copied into the pool. */
static void split_words(dui_screen_t *s, const char *words, const char **out)
{
    for (int i = 0; i < DUI_STATUS_WORDS; i++) out[i] = NULL;
    if (!words) return;
    int k = 0;
    for (const char *p = words; *p && k < DUI_STATUS_WORDS;) {
        while (*p == ' ') p++;
        const size_t n = strcspn(p, " ");
        if (!n) break;
        if (s->pool_used + n + 1 > sizeof s->pool) {
            s->overflow = true;
            return;
        }
        char *dst = s->pool + s->pool_used;
        memcpy(dst, p, n);
        dst[n] = '\0';
        s->pool_used = (uint16_t)(s->pool_used + n + 1);
        out[k++] = dst;
        p += n;
    }
}

void dui_status_words(dui_screen_t *s, int state, const char *words, const char *short_words, const char *text)
{
    s->status.state = (uint8_t)state;
    split_words(s, words, s->status.word);
    split_words(s, short_words, s->status.short_word);
    s->status.text = text ? dui_str(s, text) : NULL;
}

void dui_title(dui_screen_t *s, const char *words)
{
    s->sub = true;
    split_words(s, words, s->status.title);
}

void dui_group(dui_screen_t *s, int zone, int flags)
{
    if (s->ngroups >= DUI_GROUPS_MAX) { /* refused: its tiles go to the sink, not into the last group */
        s->overflow = s->sinking = true;
        return;
    }
    s->sinking = false;
    dui_group_t *g = &s->group[s->ngroups++];
    g->first = s->ntiles;
    g->n = 0;
    g->zone = (uint8_t)zone;
    g->flags = (uint8_t)flags;
}

/* A dropped tile: the sink, written to and never laid out. */
static dui_tile_t *sink(dui_screen_t *s)
{
    s->overflow = true;
    memset(&s->sink, 0, sizeof s->sink);
    return &s->sink;
}

dui_tile_t *dui_add(dui_screen_t *s, int kind)
{
    if (!s->ngroups && !s->sinking) dui_group(s, DUI_ZONE_MAIN, 0);
    if (s->sinking || !s->ngroups) return sink(s);
    dui_group_t *g = &s->group[s->ngroups - 1];
    if (s->ntiles >= DUI_SCREEN_TILES || g->first + g->n != s->ntiles) return sink(s);
    dui_tile_t *t = &s->tile[s->ntiles++];
    g->n++;
    memset(t, 0, sizeof *t);
    t->kind = (uint8_t)kind;
    return t;
}

dui_tile_t *dui_word(dui_screen_t *s, const char *text, int role)
{
    dui_tile_t *t = dui_add(s, DUI_TILE_WORD);
    t->text = dui_str(s, text);
    t->role = (uint8_t)role;
    return t;
}

dui_tile_t *dui_lead(dui_screen_t *s, const char *label, const char *alone_caption)
{
    dui_tile_t *t = dui_word(s, label, DUI_ROLE_LABEL);
    t->hint = dui_str(s, alone_caption); /* a word's hint: unused otherwise; dui_plan reads it (lead_apart) */
    return t;
}

dui_tile_t *dui_cap(dui_screen_t *s, const char *caption, const char *text, int role)
{
    dui_tile_t *t = dui_add(s, DUI_TILE_CAP);
    t->caption = dui_str(s, caption);
    t->text = dui_str(s, text);
    t->role = (uint8_t)role;
    return t;
}

dui_tile_t *dui_value(dui_screen_t *s, const char *caption, const char *text, int role)
{
    dui_tile_t *t = caption ? dui_cap(s, caption, text, role) : dui_word(s, text, role);
    t->flags |= DUI_F_PIECES;
    return t;
}

int dui_split_words(const char *s, int n, int *start, int *len, int max)
{
    int k = 0, i = 0;
    while (s && i < n && k < max) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= n) break;
        int j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t') j++; /* whitespace only separates words */
        start[k] = i;
        len[k] = j - i;
        k++;
        i = j;
    }
    if (k == max && i < n) { /* more words than places: the last takes the rest */
        int e = n;
        while (e > start[k - 1] && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
        len[k - 1] = e - start[k - 1];
    }
    return k;
}

#define WORDS_MAX 8

/* The k pieces of text (offsets start, lengths len) as tiles, the first with the caption, together on a row when
 * they fit (a run); text, never empty here, copied into the pool once. A full pool (dui_str gives "" and sets the
 * screen's overflow) adds nothing: no piece points past that "". */
static void pieces(dui_screen_t *s, const char *caption, const char *text, const int *start, const int *len, int k,
                   int role, uint16_t flags)
{
    const char *copy = dui_str(s, text);
    if (!copy[0]) return;
    for (int i = 0; i < k; i++) {
        dui_tile_t *t = i == 0 && caption ? dui_add(s, DUI_TILE_CAP) : dui_add(s, DUI_TILE_WORD);
        if (i == 0 && caption) t->caption = dui_str(s, caption);
        t->text = copy + start[i];
        t->len = (uint8_t)len[i];
        t->role = (uint8_t)role;
        t->flags |= flags;
        if (i == 0 && k > 1) t->run = (uint8_t)k;
    }
}

void dui_words(dui_screen_t *s, const char *caption, const char *text, int role)
{
    int start[WORDS_MAX], len[WORDS_MAX];
    const int n = text ? (int)strlen(text) : 0, k = dui_split_words(text, n, start, len, WORDS_MAX);
    if (k < 1) { /* nothing (or blanks): a value as ever */
        dui_value(s, caption, text, role);
        return;
    }
    /* each word one key, 2 lines inside it; over keys only as a last resort */
    pieces(s, caption, text, start, len, k, role, DUI_F_PIECES | DUI_F_WHOLE);
}

/* An IPv4 address: four numbers of 1-3 digits, each at most 255, joined by dots. Fills the octets'
 * offsets and lengths. */
static bool ipv4(const char *w, int start[4], int n[4])
{
    int i = 0;
    for (int k = 0; k < 4; k++) {
        int j = i, v = 0;
        while (j - i < 3 && w[j] >= '0' && w[j] <= '9') v = v * 10 + (w[j++] - '0');
        if (j == i || v > 255) return false;
        start[k] = i;
        n[k] = j - i;
        if (k == 3) return w[j] == '\0';
        if (w[j] != '.') return false;
        i = j + 1;
    }
    return false;
}

void dui_ipv4(dui_screen_t *s, const char *caption, const char *ip)
{
    int start[4], n[4];
    if (!ip || !ipv4(ip, start, n)) {
        dui_value(s, caption, ip, DUI_ROLE_ACCENT);
        return;
    }
    pieces(s, caption, ip, start, n, 4, DUI_ROLE_ACCENT, DUI_F_OCTET);
}

dui_tile_t *dui_port(dui_screen_t *s, const char *caption, const char *port)
{
    const int at = s->ntiles; /* where it goes, when there is room */
    dui_tile_t *t = caption ? dui_cap(s, caption, port, DUI_ROLE_ACCENT) : dui_word(s, port, DUI_ROLE_ACCENT);
    t->flags |= DUI_F_PORT;
    /* the address right before it (in the same group) takes it into its run */
    if (s->ntiles == at + 1 && at >= 4 && at - 4 >= s->group[s->ngroups - 1].first && s->tile[at - 4].run == 4 &&
        (s->tile[at - 1].flags & DUI_F_OCTET))
        s->tile[at - 4].run = 5;
    return t;
}

dui_tile_t *dui_step(dui_screen_t *s, int n, const char *verb, const char *sub)
{
    dui_tile_t *t = dui_add(s, DUI_TILE_STEP);
    t->num = (uint8_t)n;
    t->text = dui_str(s, verb);
    t->caption = sub ? dui_str(s, sub) : NULL;
    t->role = DUI_ROLE_VALUE;
    t->flags |= DUI_F_OPTIONAL;
    return t;
}

dui_tile_t *dui_button(dui_screen_t *s, int icon, const char *label, uint16_t action)
{
    dui_tile_t *t = dui_add(s, DUI_TILE_BUTTON);
    t->icon = (uint8_t)icon;
    t->text = dui_str(s, label);
    t->action = action;
    return t;
}

dui_tile_t *dui_choice(dui_screen_t *s, const char *label, const char *sub, uint16_t action)
{
    dui_tile_t *t = dui_button(s, DUI_ICON_NONE, label, action);
    t->caption = dui_str(s, sub);
    return t;
}

dui_tile_t *dui_icon_tile(dui_screen_t *s, int icon, const char *label, bool dim)
{
    dui_tile_t *t = dui_add(s, DUI_TILE_ICON);
    t->icon = (uint8_t)icon;
    t->text = dui_str(s, label);
    if (dim) t->flags |= DUI_F_DIM;
    return t;
}

dui_tile_t *dui_qr(dui_screen_t *s, const char *payload)
{
    dui_tile_t *t = dui_add(s, DUI_TILE_QR);
    t->text = dui_str(s, payload);
    return t;
}

dui_tile_t *dui_spin(dui_screen_t *s) { return dui_add(s, DUI_TILE_SPIN); }

dui_tile_t *dui_menu_button(dui_screen_t *s, int icon, const char *label, uint16_t action, int dot)
{
    if (s->nmenu >= DUI_MENU_TILES) return sink(s);
    dui_tile_t *t = &s->menu_tile[s->nmenu++];
    memset(t, 0, sizeof *t);
    t->kind = DUI_TILE_BUTTON;
    t->icon = (uint8_t)icon;
    t->text = dui_str(s, label);
    t->action = action;
    t->state = (uint8_t)dot;
    return t;
}
