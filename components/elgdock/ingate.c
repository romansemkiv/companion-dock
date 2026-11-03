/* Pure: the dock input gate (ingate.h). */
#include "ingate.h"

#include <string.h>

enum { K_OTHER, K_KEYS, K_ENC }; /* what a report says */

/* The report's kind and its header length (the states follow it). */
static int kind_of(const ingate_t *g, const uint8_t *r, size_t len, size_t *hdr)
{
    if (len < 2 || r[0] != 0x01) return K_OTHER;
    if (!g->gen2) {
        *hdr = 1;
        return K_KEYS;
    }
    if (len >= 5 && r[1] == 0x00) {
        *hdr = 4;
        return K_KEYS;
    }
    if (len >= 6 && r[1] == 0x03 && r[4] == 0x00) {
        *hdr = 5;
        return K_ENC;
    }
    return K_OTHER;
}

static size_t hdr_of(const ingate_t *g, int kind) { return kind == K_ENC ? 5 : g->gen2 ? 4 : 1; }

bool ingate_held(const char *serial, const char *const *hold, int n)
{
    for (int i = 0; serial && serial[0] && hold && i < n; i++)
        if (hold[i] && !strcmp(serial, hold[i])) return true;
    return false;
}

void ingate_init(ingate_t *g, bool gen2, bool screen, bool hold_all)
{
    memset(g, 0, sizeof *g);
    g->gen2 = gen2;
    g->screen = screen;
    g->hold = screen && hold_all;
}

/* The all-released copy of a kept report into out; false when nothing in it was pressed. */
static bool released(const uint8_t *r, uint8_t len, size_t hdr, uint8_t *out)
{
    bool any = false;
    for (size_t i = hdr; i < len; i++) any |= r[i] != 0;
    if (!any) return false;
    memcpy(out, r, hdr);
    memset(out + hdr, 0, (size_t)len - hdr);
    return true;
}

/* What is pressed in the kept report becomes the mask. */
static void arm(uint8_t *mask, const uint8_t *r, uint8_t len, size_t hdr)
{
    memset(mask, 0, INGATE_MAX);
    for (size_t i = hdr; i < len; i++) mask[i] = r[i] != 0;
}

int ingate_hold(ingate_t *g, bool hold, uint8_t out[2][INGATE_MAX], uint8_t out_len[2])
{
    if (!g->screen || hold == g->hold) return 0;
    g->hold = hold;
    int n = 0;
    if (hold) { /* Companion lets go of what it saw pressed */
        memset(g->mask_keys, 0, sizeof g->mask_keys);
        memset(g->mask_enc, 0, sizeof g->mask_enc);
        if (g->keys_len && released(g->keys, g->keys_len, hdr_of(g, K_KEYS), out[n])) out_len[n++] = g->keys_len;
        if (g->enc_len && released(g->enc, g->enc_len, hdr_of(g, K_ENC), out[n])) out_len[n++] = g->enc_len;
    } else { /* held through the session: released for Companion until let go */
        arm(g->mask_keys, g->keys, g->keys_len, hdr_of(g, K_KEYS));
        arm(g->mask_enc, g->enc, g->enc_len, hdr_of(g, K_ENC));
    }
    return n;
}

bool ingate_report(ingate_t *g, const uint8_t *r, size_t len, uint8_t *out)
{
    const size_t n = len < INGATE_MAX ? len : INGATE_MAX;
    size_t hdr = 0;
    const int kind = kind_of(g, r, n, &hdr);
    if (kind != K_OTHER) { /* the deck's state now */
        memcpy(kind == K_KEYS ? g->keys : g->enc, r, n);
        if (kind == K_KEYS)
            g->keys_len = (uint8_t)n;
        else
            g->enc_len = (uint8_t)n;
    }
    if (g->hold) return false;
    memcpy(out, r, n);
    uint8_t *mask = kind == K_KEYS ? g->mask_keys : kind == K_ENC ? g->mask_enc : NULL;
    for (size_t i = hdr; mask && i < n; i++) {
        if (!mask[i]) continue;
        if (r[i] == 0)
            mask[i] = 0; /* let go: the next press is Companion's */
        else
            out[i] = 0;
    }
    return true;
}
