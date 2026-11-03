/* Pure: fingerprints of what a key or the status bar shows, and the animation phase (deckui.h): the
 * caller draws a key again only when its fingerprint changes (a key is re-encoded and sent
 * only when its content changed; animated tiles at 5 fps or less). FNV-1a, 32 bits. */
#include <string.h>

#include "deckui/deckui.h"
#include "dui_int.h"

#define FNV_BASIS DUI__FNV_BASIS
#define FNV_PRIME 16777619u

static uint32_t mix(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * FNV_PRIME;
    return h;
}

uint32_t dui__fnv(uint32_t h, const void *p, size_t n) { return mix(h, p, n); }

static uint32_t mix_u(uint32_t h, uint32_t v) { return mix(h, &v, sizeof v); }

/* A string by its content (n chars; n < 0: up to its NUL), then a separator, so "ab"+"c" != "a"+"bc". */
static uint32_t mix_s(uint32_t h, const char *s, int n)
{
    if (!s) return mix_u(h, 0xFFFFFFFFu);
    const size_t len = n < 0 ? strlen(s) : (size_t)n;
    h = mix(h, s, len);
    return mix_u(h, (uint32_t)len);
}

uint8_t dui_phase(int64_t now_ms, int period_ms)
{
    if (period_ms <= 0 || now_ms < 0) return 0;
    const int64_t t = now_ms / DUI_ANIM_MS * DUI_ANIM_MS;
    return (uint8_t)((t % period_ms) * 256 / period_ms);
}

uint8_t dui_bar_phase(int64_t now_ms)
{
    if (now_ms < 0) return 0;
    const int64_t t = now_ms / DUI_BAR_ANIM_MS * DUI_BAR_ANIM_MS;
    return (uint8_t)((t % DUI_BAR_PULSE_MS) * 256 / DUI_BAR_PULSE_MS);
}

bool dui_tile_animated(const dui_tile_t *t)
{
    return t->kind == DUI_TILE_SPIN || (t->kind == DUI_TILE_GLYPH && t->state == DUI_STATE_WAIT);
}

uint32_t dui_tile_hash(const dui_tile_t *t)
{
    uint32_t h = FNV_BASIS;
    const uint8_t b[] = {t->kind, t->role, t->tint, t->icon, t->state, t->num, t->cur, t->phase,
                         (uint8_t)t->rssi, t->hold, t->run};
    h = mix(h, b, sizeof b);
    h = mix_u(h, t->flags);
    h = mix_u(h, t->action);
    h = mix_u(h, t->hold_ms);
    h = mix_s(h, t->text, t->text && t->len ? t->len : -1);
    h = mix_s(h, t->caption, -1);
    h = mix_s(h, t->hint, -1);
    const uint8_t f[] = {t->fit.px, t->fit.nl, t->fit.cut, t->fit.hyph, t->fit.base, t->fit.rowcap, t->fit.cap_px};
    h = mix(h, f, sizeof f);
    return h ? h : 1;
}

uint32_t dui_bar_hash(const dui_status_t *st, bool sub, int page, int pages)
{
    uint32_t h = FNV_BASIS;
    const uint8_t b[] = {st->state, st->net, (uint8_t)st->rssi, st->hotspot, st->close, st->phase, sub, st->toast_state,
                         st->wifi, st->eth_link}; /* the signs */
    h = mix(h, b, sizeof b);
    for (int i = 0; i < DUI_STATUS_WORDS; i++) {
        h = mix_s(h, st->word[i], -1);
        h = mix_s(h, st->short_word[i], -1);
        h = mix_s(h, st->title[i], -1);
    }
    h = mix_s(h, st->text, -1);
    h = mix_s(h, st->toast, -1);
    h = mix_u(h, (uint32_t)page);
    h = mix_u(h, (uint32_t)pages);
    return h ? h : 1;
}
