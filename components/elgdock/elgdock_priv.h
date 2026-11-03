#pragma once
/* Shared by elgdock.c (the dock server: listening sockets, mDNS, the dock map) and elgdock_deck.c
 * (deck connections: the deck presented on its dock's port, Companion's HID traffic for it). ESP-only.
 *
 * A dock has one port, open only while a deck is assigned. Each connection serves the
 * deck that was assigned when it began, and ends when that deck goes (dockproto_dock_plan).
 *
 * Locks, in this order:
 *  - a dock's usb_mtx: held by dockrxK while it uses the dock's deck, and around on_owner;
 *    elgdock_release takes it to know the deck is no longer used;
 *  - its tx_mtx: serialises sends on the deck connection (dockrxK replies, docktxK input and
 *    keepalives); held across a blocking send, so only that dock's own tasks ever take it;
 *  - its cs_mtx: the deck connection is opened, closed and shut down under it; held briefly, never
 *    across a send, so the server task and a detaching deck's task never wait on a slow peer;
 *  - elg.mtx: assignments, the dock map, connection hand-over, status. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "cora.h"
#include "deckproto/corner.h"
#include "dockproto.h"
#include "ingate.h"
#include "elgdock/elgdock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ELG_TAG "elgdock"
#define ELG_IDLE_MS 6000 /* nothing from the host this long (it ACKs every keepalive): gone */
#define ELG_SEND_MAX (CORA_HDR_LEN + DOCK_CHILD_LEN)
#define ELG_IN_MAX 64

typedef struct {
    int k;

    /* elg.mtx */
    usbdeck_t *deck; /* NULL -> d under elg.mtx; d -> NULL under usb_mtx + elg.mtx */
    uint16_t vid, pid;
    char deck_serial[32], model[32], deck_fw[16];
    bool listening; /* copy of lp >= 0 for elgdock_status */
    int cs_new;     /* accepted deck connection waiting for dockrxK */
    char peer_new[16];
    bool companion;
    char peer[16];

    /* server task only (elgdock.c) */
    int lp; /* the dock's listening port, -1 while it has no deck */
    bool announced, ann_failing;
    char ann_name[DOCK_MDNS_NAME_MAX + 1], ann_sn[32]; /* as announced */
    int64_t listen_retry_ms; /* a failed listen is tried again from then */
    bool listen_failing;     /* logged once per failure streak */

    /* deck connection (elgdock_deck.c) */
    SemaphoreHandle_t usb_mtx, tx_mtx, cs_mtx;
    int cs;             /* tx_mtx + cs_mtx to change; either to read: the connection served, -1 */
    uint32_t c_gen;     /* bumped per deck connection, with cs */
    atomic_bool c_stop; /* end the deck connection (replaced, or the deck went) */
    atomic_uint in_gen; /* c_gen, for the input reports the deck task hands over */
    TaskHandle_t rx, tx;
    QueueHandle_t inq;
    atomic_uint in_dropped, warned;
    /* the input gate (elgdock_hold_input): in_mtx, held by the deck task over handing a report on */
    SemaphoreHandle_t in_mtx;
    ingate_t gate;
    corner_gate_t corner; /* the gesture's gate (in_mtx; elg.gesture), after the input gate */
} dock_t;

typedef struct {
    SemaphoreHandle_t mtx;
    elgdock_cb_t cb;
    uint8_t mac[6];
    TaskHandle_t srv;
    /* mtx */
    char box_name[33];
    bool announce;
    int ndocks; /* the docks in use, 0 .. ndocks - 1 (the deck limit), set at start */
    char hold[ELGDOCK_MAX][32]; /* elgdock_hold_input: the decks held from Companion (nhold of them); one
                                 * assigned (again) meanwhile starts held */
    int nhold;
    char map[ELGDOCK_MAX][32]; /* sticky deck serial per dock, as in NVS */
    bool map_dirty;
    corner_cfg_t gesture; /* the box menu gesture every dock's gate runs (elgdock_set_gesture) */
    dock_t dock[ELGDOCK_MAX];
} elg_t;

extern elg_t elg;

void elg_lock(void);
void elg_unlock(void);
int64_t elg_now_ms(void);
/* One Cora message, whole (blocking send, SO_SNDTIMEO bounded). */
bool elg_send_msg(int fd, uint16_t flags, uint8_t op, uint32_t id, const uint8_t *pl, size_t len);
/* ---- elgdock_deck.c ---- */

/* Server task: a new deck connection (TCP options set) replaces the one being served. */
void elg_deck_accept(dock_t *k, int fd, const char *peer);
/* Any task: ends the deck connection being served, and drops one waiting to be served. */
void elg_deck_stop(dock_t *k);
