/* The WebSocket client table (ws_table.h). */
#include "ws_table.h"

#include <string.h>

wst_slot_t *wst_find(wst_t *t, int fd)
{
    for (int i = 0; i < WST_MAX; i++)
        if (t->s[i].fd == fd) return &t->s[i];
    return NULL;
}

wst_slot_t *wst_add(wst_t *t, int fd)
{
    if (!fd) return NULL;
    wst_slot_t *w = wst_find(t, fd);
    if (w) return w;
    w = wst_find(t, 0);
    if (!w) return NULL;
    memset(w, 0, sizeof *w);
    w->fd = fd;
    atomic_fetch_add(&t->n, 1);
    return w;
}

void wst_sign_out(wst_t *t, wst_slot_t *w)
{
    if (!w || !w->signed_in) return;
    w->signed_in = false;
    memset(w->hash, 0, sizeof w->hash);
    atomic_fetch_sub(&t->n_signed, 1);
}

void wst_remove(wst_t *t, int fd)
{
    wst_slot_t *w = fd ? wst_find(t, fd) : NULL;
    if (!w) return;
    wst_sign_out(t, w);
    memset(w, 0, sizeof *w);
    atomic_fetch_sub(&t->n, 1);
}

bool wst_sign_in(wst_t *t, wst_slot_t *w, const uint8_t hash[32])
{
    if (!w || !hash) return false;
    if (w->signed_in && !memcmp(w->hash, hash, sizeof w->hash)) return false;
    if (!w->signed_in) atomic_fetch_add(&t->n_signed, 1);
    w->signed_in = true;
    memcpy(w->hash, hash, sizeof w->hash);
    return true;
}

bool wst_authed(wst_t *t, wst_slot_t *w, bool (*live)(const uint8_t hash[32]))
{
    if (!w || !w->signed_in) return false;
    if (live && live(w->hash)) return true;
    wst_sign_out(t, w);
    return false;
}

bool wst_listening(wst_t *t, bool auth_on)
{
    return atomic_load(auth_on ? &t->n_signed : &t->n) > 0;
}
