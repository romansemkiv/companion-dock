/* Pure: the row alignment, on statusui's fitting (sui_fit). */
#include <math.h>
#include <string.h>

#include "dui_int.h"

#define U(v) ((float)(v) * (float)key_px / 96.0f) /* design units (a 96 px key) to px */

static int tlen(const dui_tile_t *t) { return t->text ? (t->len ? t->len : (int)strlen(t->text)) : 0; }

static bool is_text(const dui_tile_t *t)
{
    return t && (t->kind == DUI_TILE_WORD || t->kind == DUI_TILE_CAP || t->kind == DUI_TILE_STEP) && tlen(t) > 0;
}

/* a natural place to break s before s[i]. */
static bool natural(const char *s, int i)
{
    const char a = s[i - 1], b = s[i];
    if (a == '-' || a == '.' || a == '/' || b == ' ') return true;
    if (a >= 'a' && a <= 'z' && b >= 'A' && b <= 'Z') return true;
    const bool al = (a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z'), bl = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
    const bool ad = a >= '0' && a <= '9', bd = b >= '0' && b <= '9';
    return (al && bd) || (ad && bl);
}

/* s has a natural break somewhere (not at a run of spaces' end). */
static bool has_natural(const char *s, int len)
{
    for (int i = 1; i < len; i++)
        if (natural(s, i) && s[i - 1] != ' ') return true;
    return false;
}

static bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_vowel(char c)
{
    const char l = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return l == 'a' || l == 'e' || l == 'i' || l == 'o' || l == 'u' || l == 'y';
}

/* A syllable-like place to break the word s[0..len) before s[i] (with an added '-'): between two consonants
 * with a vowel after them ("Recon" | "necting"), or before a consonant between two vowels ("Ope" | "rating");
 * 3 letters at least on each line. */
static bool syllable(const char *s, int len, int i, bool word)
{
    if (i < 3 || len - i < 3) return false;
    if (!word) return false; /* one plain word only (all_letters(), once per dui__two_lines()) */
    return !is_vowel(s[i]) && is_vowel(s[i + 1]); /* the second line starts with a consonant and a vowel */
}

static bool all_letters(const char *s, int len)
{
    for (int k = 0; k < len; k++)
        if (!is_letter(s[k])) return false;
    return true;
}

int dui__two_lines(const char *s, int len, int inner, int max_px, int min_px, bool hyphen_ok, int *cut, int *next,
                   bool *hyph)
{
    int best_px = 0, best_cut = 0, best_next = 0, best_uneven = 1 << 30;
    bool best_h = false;
    char tmp[SUI_FIT_MAX + 2];
    const bool word = hyphen_ok && all_letters(s, len); /* pass 1 only, which runs only when hyphen_ok */
    /* the natural breaks; then (a word, where a hyphen may be added) a syllable-like break with a '-'; then
     * any place (with a '-' where one may be added) */
    for (int pass = 0; pass < 3 && !best_px; pass++) {
        if (pass == 1 && !hyphen_ok) continue;
        for (int i = 1; i < len; i++) {
            if (s[i - 1] == ' ' || (pass == 0 && !natural(s, i)) || (pass == 1 && !syllable(s, len, i, word))) continue;
            int a = i, b = i;
            while (a > 0 && s[a - 1] == ' ') a--;
            while (b < len && s[b] == ' ') b++;
            if (a == 0 || b >= len) continue;
            const bool h = pass > 0 && hyphen_ok && is_letter(s[a - 1]) && is_letter(s[b]);
            if (a + 1 > SUI_FIT_MAX) continue;
            memcpy(tmp, s, (size_t)a);
            if (h) tmp[a] = '-';
            /* no size below the best so far can win (a tie still can), so neither fit looks below it; the
             * second line not at all when the first can't */
            const int lo = best_px > min_px ? best_px : min_px;
            const int p1 = dui__font_fit(tmp, a + h, inner, max_px, lo, 0),
                      p2 = p1 ? dui__font_fit(s + b, len - b, inner, max_px, lo, 0) : 0;
            const int px = p1 && p2 ? (p1 < p2 ? p1 : p2) : 0;
            if (!px) continue;
            const int n1 = a + h, uneven = n1 > len - b ? n1 - (len - b) : (len - b) - n1;
            if (px > best_px || (px == best_px && uneven < best_uneven)) {
                best_px = px;
                best_cut = a;
                best_next = b;
                best_uneven = uneven;
                best_h = h;
            }
        }
    }
    *cut = best_cut;
    *next = best_next;
    if (hyph) *hyph = best_h;
    return best_px;
}

/* A value on one key at DUI_TEXT_MIN_PX or more, on 1 line or 2 as two_lines() splits a value (nothing added). */
bool dui__value_fits(const char *s, int len, int key_px)
{
    const int inner = key_px - 2 * SUI_MARGIN, max_px = (int)(U(40) + 0.001f);
    if (dui__font_fit(s, len, inner, max_px, DUI_TEXT_MIN_PX, 0)) return true;
    int a, b;
    return dui__two_lines(s, len, inner, (int)(U(30) + 0.001f), DUI_TEXT_MIN_PX, false, &a, &b, NULL) > 0;
}

/* A word in two lines centred in the value area, at the largest size the longer line allows from
 * DUI_TEXT_MIN_PX up to 30 px on a 96 px key; below that only when nothing else fits. At a natural break; else:
 * a status word or a label at a syllable-like point with a '-' added ("Recon-" / "necting", never "Reconn" /
 * "ecting"); a value (an SSID's word, a password, an address, a version) inside it with nothing ever added
 * (statusui's value rule), split as evenly as reads well. dui__value_fits plans a value the same way. */
static void two_lines(const dui_tile_t *t, int key_px, dui_fit_t *fit)
{
    const int len = tlen(t), inner = key_px - 2 * SUI_MARGIN, max2 = (int)(U(30) + 0.001f);
    /* a status word or a label; never a value: nothing is ever added to a value (an SSID's word included) */
    const bool word = t->kind == DUI_TILE_WORD && t->role != DUI_ROLE_VALUE && t->role != DUI_ROLE_ACCENT &&
                      !(t->flags & (DUI_F_WHOLE | DUI_F_PIECES));
    int a, b;
    bool h = false;
    int px = dui__two_lines(t->text, len, inner, max2, DUI_TEXT_MIN_PX, word, &a, &b, &h);
    if (!px) px = dui__two_lines(t->text, len, inner, max2, 10, word, &a, &b, &h); /* too long: the best it can */
    if (!px) {
        dui__split2(t->text, len, &a, &b);
        px = 10;
        h = false;
    }
    fit->nl = 2;
    fit->cut = (uint8_t)a;
    fit->hyph = h;
    fit->px = (uint8_t)px;
}

/* a word or value that fits one line only small, while its natural break gives two lines a third
 * larger or more (and at least the comfortable size), reads better on two ("Simbox-" / "Home"). */
static bool prefers_two(const dui_tile_t *t, int key_px, int one_px)
{
    if (t->kind == DUI_TILE_STEP || one_px >= sui_comfort_px(key_px)) return false;
    const int inner = key_px - 2 * SUI_MARGIN, len = tlen(t);
    if (!has_natural(t->text, len)) return false; /* never "Recon-" / "necting" for a word that fits one line */
    int a, b;
    const int two = dui__two_lines(t->text, len, inner, (int)(U(30) + 0.001f), DUI_TEXT_MIN_PX, false, &a, &b, NULL);
    return two >= sui_comfort_px(key_px) && two * 3 >= one_px * 4 && a > 0 && a < len && natural(t->text, a);
}

/* The caption upper-cased (at most 31 chars). */
int dui__status_px(int key_px)
{
    const int px = dui__font_fit(DUI_STATUS_WHOLE, (int)strlen(DUI_STATUS_WHOLE), key_px - 2 * SUI_MARGIN,
                                 (int)(U(30) + 0.001f), DUI_TEXT_MIN_PX, 0);
    return px ? px : DUI_TEXT_MIN_PX;
}

void dui_align_status(dui_tile_t *const *row, int n, int key_px)
{
    const int inner = key_px - 2 * SUI_MARGIN, px = dui__status_px(key_px);
    const int base = (int)lroundf(U(48) + dui__cap(px) / 2.0f);
    for (int i = 0; i < n; i++) {
        dui_tile_t *t = row[i];
        if (!is_text(t)) continue;
        dui_fit_t fit = {0};
        fit.base = (uint8_t)base;
        fit.px = (uint8_t)px;
        fit.nl = 1;
        const int len = tlen(t);
        if (dui__text_w(px, t->text, len, 0, NULL) > inner) { /* two lines at the row's size, never smaller ... */
            const bool word = t->kind == DUI_TILE_WORD && t->role != DUI_ROLE_VALUE && t->role != DUI_ROLE_ACCENT &&
                              !(t->flags & (DUI_F_WHOLE | DUI_F_PIECES));
            int a, b;
            bool h = false;
            int p = dui__two_lines(t->text, len, inner, px, DUI_TEXT_MIN_PX, word, &a, &b, &h);
            if (!p) p = dui__two_lines(t->text, len, inner, px, 10, word, &a, &b, &h); /* ... unless too long even so */
            if (!p) {
                dui__split2(t->text, len, &a, &b);
                p = 10;
                h = false;
            }
            fit.nl = 2;
            fit.cut = (uint8_t)a;
            fit.hyph = h;
            fit.px = (uint8_t)p;
        }
        t->fit = fit;
    }
}

void dui_align_row(dui_tile_t *const *row, int n, int key_px)
{
    bool rowcap = false;
    for (int i = 0; i < n; i++)
        if (is_text(row[i]) && (row[i]->kind == DUI_TILE_CAP || row[i]->kind == DUI_TILE_STEP)) rowcap = true;
    const int inner = key_px - 2 * SUI_MARGIN, spacing = (int)lroundf(U(0.5f) * 16);
    const int cap_near = dui__font_near(U(15)) > DUI_CAP_MIN_PX ? dui__font_near(U(15)) : DUI_CAP_MIN_PX,
              comfort = sui_comfort_px(key_px);
    int role_px[DUI_ROLES + 1] = {0}, cap_px = 0; /* index DUI_ROLES: the steps' verbs */
    for (int i = 0; i < n; i++) {
        const dui_tile_t *t = row[i];
        if (!is_text(t)) continue;
        const int r = t->kind == DUI_TILE_STEP ? DUI_ROLES : t->role < DUI_ROLES ? t->role : DUI_ROLE_VALUE;
        int lim = (int)(U(r == DUI_ROLES ? 24 : rowcap ? 36 : 40) + 0.001f);
        /* a one-word label ("Mode") no larger than the comfortable size: it never outweighs the
         * values and the states it labels */
        if (t->kind == DUI_TILE_WORD && r == DUI_ROLE_LABEL && !memchr(t->text, ' ', (size_t)tlen(t)) && lim > comfort)
            lim = comfort;
        int f = dui__font_fit(t->text, tlen(t), inner, lim, DUI_TEXT_MIN_PX, 0);
        if (!f && (t->flags & DUI_F_IP)) f = dui__font_fit(t->text, tlen(t), inner, lim, 10, 0); /* one line */
        if (f && !(t->flags & DUI_F_IP) && prefers_two(t, key_px, f)) f = 0; /* two larger lines read better */
        if (f && f < comfort && t->kind == DUI_TILE_WORD && r >= DUI_ROLE_BAD && r <= DUI_ROLE_OK) {
            int a, b; /* a state word that fits one line only small: in its key on two ("Recon-" / "necting") */
            const int two = dui__two_lines(t->text, tlen(t), inner, (int)(U(30) + 0.001f), comfort, true, &a, &b, NULL);
            if (two && two * 3 >= f * 4) f = 0;
        }
        if (f && (!role_px[r] || f < role_px[r])) role_px[r] = f;
        if (t->kind == DUI_TILE_CAP && t->caption && t->caption[0]) {
            char up[32];
            const int cn = dui__upper(t->caption, up, 31);
            int cf = dui__font_fit(up, cn, inner, cap_near, DUI_CAP_MIN_PX, spacing);
            if (!cf) cf = dui__font_fit(up, cn, inner, cap_near, DUI_CAP_MIN_PX, 0); /* without the spacing */
            if (!cf) cf = DUI_CAP_MIN_PX;
            if (!cap_px || cf < cap_px) cap_px = cf;
        }
    }
    int maxpx = 0;
    for (int r = 0; r < DUI_ROLES; r++)
        if (role_px[r] > maxpx) maxpx = role_px[r];
    const int capref = dui__cap(maxpx ? maxpx : dui__font_near(U(30)));
    const int base = (int)lroundf(U(rowcap ? 57 : 48) + capref / 2.0f);
    for (int i = 0; i < n; i++) {
        dui_tile_t *t = row[i];
        if (!is_text(t)) continue;
        const int r = t->kind == DUI_TILE_STEP ? DUI_ROLES : t->role < DUI_ROLES ? t->role : DUI_ROLE_VALUE;
        dui_fit_t fit = {0};
        fit.base = (uint8_t)base;
        fit.rowcap = rowcap;
        fit.cap_px = (uint8_t)cap_px;
        const int px = role_px[r];
        if (px && dui__text_w(px, t->text, tlen(t), 0, NULL) <= inner) {
            fit.nl = 1;
            fit.px = (uint8_t)px;
        } else if (t->flags & DUI_F_IP) { /* an address's key is one line, at the size that fits it */
            fit.nl = 1;
            const int own = dui__font_fit(t->text, tlen(t), inner, px ? px : DUI_TEXT_MIN_PX, 10, 0);
            fit.px = (uint8_t)(own ? own : 10);
        } else {
            two_lines(t, key_px, &fit);
        }
        t->fit = fit;
    }
}
