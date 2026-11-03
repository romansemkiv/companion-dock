/* Pure: newest-wins pending item per control, drained round-robin per slot. */
#include "surfaces/surfaces.h"

static bool in_range(int slot, int ctl)
{
    return slot >= 0 && slot < SURF_MAX_DECKS && ctl >= 0 && ctl < SURF_MAX_CTLS;
}

void *coalesce_put(coalesce_t *c, int slot, int ctl, void *item)
{
    if (!c || !in_range(slot, ctl)) return item;
    void *old = c->pending[slot][ctl];
    c->pending[slot][ctl] = item;
    return old;
}

void *coalesce_take(coalesce_t *c, int slot, int *ctl)
{
    if (!c || !in_range(slot, 0)) return NULL;
    int start = c->next[slot] % SURF_MAX_CTLS;
    for (int k = 0; k < SURF_MAX_CTLS; k++) {
        int i = (start + k) % SURF_MAX_CTLS;
        void *item = c->pending[slot][i];
        if (item) {
            c->pending[slot][i] = NULL;
            c->next[slot] = (uint8_t)((i + 1) % SURF_MAX_CTLS);
            if (ctl) *ctl = i;
            return item;
        }
    }
    return NULL;
}

void coalesce_drop_slot(coalesce_t *c, int slot, void (*free_fn)(void *))
{
    if (!c || !in_range(slot, 0)) return;
    for (int i = 0; i < SURF_MAX_CTLS; i++) {
        void *item = c->pending[slot][i];
        c->pending[slot][i] = NULL;
        if (item && free_fn) free_fn(item);
    }
    c->next[slot] = 0;
}
