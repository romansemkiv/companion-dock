/* Pure: which deck sits in which slot, and where a Companion KEY-STATE goes. */
#include <stdio.h>
#include <string.h>
#include "surfaces/surfaces.h"

#include "satproto/b64.h"

static bool slot_used(const surf_slot_t *s) { return s->device_id[0] != '\0'; }

int surfreg_find(const surf_registry_t *r, const char *device_id)
{
    if (!r || !device_id || !device_id[0]) return -1;
    for (int i = 0; i < SURF_MAX_DECKS; i++)
        if (slot_used(&r->slot[i]) && strcmp(r->slot[i].device_id, device_id) == 0) return i;
    return -1;
}

int surfreg_attach(surf_registry_t *r, const char *device_id, void *deck, const deck_model_t *m)
{
    if (!r || !device_id || !device_id[0] || !deck || !m) return -1;
    if (strlen(device_id) >= sizeof r->slot[0].device_id) return -1;
    int i = surfreg_find(r, device_id);
    bool fresh = i < 0;
    const int cap = r->cap > 0 && r->cap < SURF_MAX_DECKS ? r->cap : SURF_MAX_DECKS;
    for (int k = 0; k < cap && i < 0; k++)
        if (!slot_used(&r->slot[k])) i = k;
    if (i < 0) return -1;

    surf_slot_t *s = &r->slot[i];
    int n = deck_model_controls(m, s->ctls, SURF_MAX_CTLS);
    if (n < 0) {
        if (fresh) memset(s, 0, sizeof *s);
        return -1;
    }
    if (fresh) {
        snprintf(s->device_id, sizeof s->device_id, "%s", device_id);
        s->added = false;
    }
    s->deck = deck;
    s->model = m;
    s->nctl = n;
    return i;
}

int surfreg_detach(surf_registry_t *r, void *deck)
{
    if (!r || !deck) return -1;
    for (int i = 0; i < SURF_MAX_DECKS; i++) {
        if (slot_used(&r->slot[i]) && r->slot[i].deck == deck) {
            memset(&r->slot[i], 0, sizeof r->slot[i]);
            return i;
        }
    }
    return -1;
}

void surfreg_mark_all_unadded(surf_registry_t *r)
{
    if (!r) return;
    for (int i = 0; i < SURF_MAX_DECKS; i++) r->slot[i].added = false;
}

int surf_pending_adds(const surf_registry_t *r, int *slots, int max)
{
    int n = 0;
    if (!r || !slots) return 0;
    for (int i = 0; i < SURF_MAX_DECKS && n < max; i++)
        if (slot_used(&r->slot[i]) && !r->slot[i].added) slots[n++] = i;
    return n;
}

int surf_route_keystate(const surf_registry_t *r, const sat_key_state_t *ks, int *slot,
                        const deck_ctl_t **ctl, size_t *expected_bytes)
{
    if (!r || !ks || !ks->control_id || !slot || !ctl || !expected_bytes) return -1;
    int i = surfreg_find(r, ks->device_id);
    if (i < 0) return -1;
    const surf_slot_t *s = &r->slot[i];
    const deck_ctl_t *c = deck_ctl_find(s->ctls, s->nctl, ks->control_id);
    if (!c) return -1;
    switch (c->kind) {
    case CTL_KEY_LCD:
    case CTL_LCD_CELL:
    case CTL_INFO:
        *expected_bytes = (size_t)c->w * c->h * 3;
        break;
    case CTL_KEY_RGB:
        *expected_bytes = 0;
        break;
    default: /* pedal inputs, encoders: nothing to draw */
        return -1;
    }
    *slot = i;
    *ctl = c;
    return 0;
}

bool surf_bitmap_ok(size_t expected, size_t got) { return expected != 0 && got == expected; }

bool surf_bitmap_text_ok(const char *v, size_t len, size_t expected)
{
    if (!v || !expected) return false;
    const char *b64;
    size_t n;
    switch (sat_bitmap_split(v, len, &b64, &n)) {
    case SAT_BITMAP_RGB:
        return n == (expected + 2) / 3 * 4;
    case SAT_BITMAP_PNG: /* even an incompressible RGBA PNG stays under this; a bigger one is no button image */
        return n > 0 && n <= (expected + expected / 2 + 4096 + 2) / 3 * 4;
    default:
        return false;
    }
}

surf_loss_t surf_loss_phase(int64_t lost_at_us, bool dimmed, int64_t now_us)
{
    if (!lost_at_us) return SURF_LOSS_NONE;
    const int64_t gone = now_us - lost_at_us;
    if (gone >= SURF_LOSS_OVER_US) return SURF_LOSS_OVER;
    if (dimmed) return SURF_LOSS_DIMMED;
    return gone >= SURF_LOSS_DIM_US ? SURF_LOSS_DIM_DUE : SURF_LOSS_GRACE;
}
