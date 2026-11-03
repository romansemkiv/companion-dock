/* The WebSocket client table (pure, host-tested): which sockets are open, which are signed in (by
 * the SHA-256 of their session token: never the token itself), and which are being closed. web.c
 * owns one table and uses it from the HTTP server's task only; the counters are atomic because the
 * notify task reads them. */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WST_MAX 4

typedef struct {
    int fd;           /* 0 = free */
    bool signed_in;   /* a live session's token came with the upgrade or a frame... */
    uint8_t hash[32]; /* ...and this is its SHA-256 */
    bool closing;     /* the session is being closed: no more pushes */
} wst_slot_t;

typedef struct {
    wst_slot_t s[WST_MAX];
    atomic_int n;        /* sockets in the table */
    atomic_int n_signed; /* of them signed in */
} wst_t; /* zero-initialise */

wst_slot_t *wst_find(wst_t *t, int fd);
/* The socket's slot, a new one if it has none; NULL when the table is full or fd is 0. */
wst_slot_t *wst_add(wst_t *t, int fd);
void wst_remove(wst_t *t, int fd);
/* Signs the slot in with a live session's hash; true when it was not signed in with that session. */
bool wst_sign_in(wst_t *t, wst_slot_t *w, const uint8_t hash[32]);
/* Drops the slot's sign-in (logout, expiry, password change). */
void wst_sign_out(wst_t *t, wst_slot_t *w);
/* The slot is signed in and live(hash) says its session still is; a session that ended drops the
 * sign-in, so the slot stops counting as a listener. */
bool wst_authed(wst_t *t, wst_slot_t *w, bool (*live)(const uint8_t hash[32]));
/* Someone can receive pushes: any socket while the API is open, else a signed-in one. */
bool wst_listening(wst_t *t, bool auth_on);

#ifdef __cplusplus
}
#endif
