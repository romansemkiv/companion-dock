#pragma once
/* Network Dock mode: Companion 5 adds the box as a Remote Surface (Elgato's TCP protocol)
 * and drives each deck with its own Stream Deck driver. Each deck has a dock k (0..ELGDOCK_MAX-1) with
 * one port, ELGDOCK_PRIMARY_PORT(k), on which the deck itself is the device: the host's
 * identification gets the deck's real VID/PID, so Companion lists one surface per deck and no Network
 * Dock. A dock listens and is announced only while a deck is assigned to it, dock 0 too: when
 * the deck goes its connection is closed, the port closed and the announcement withdrawn, so
 * Companion shows the deck's surface offline and reconnects by itself when the deck is back (to the
 * same port: the dock map is sticky). Each listening dock is announced as mDNS _elg._tcp, dt=215 (a
 * Network Dock: the only kind Companion's discovery lists), named "<model> @ <box name>". One client
 * per port: a new connection replaces the old one. ESP-only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "deckmodel/model.h"
#include "deckproto/corner.h"
#include "esp_err.h"
#include "usbdeck/usbdeck.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ELGDOCK_MAX 6
#define ELGDOCK_PRIMARY_PORT(k) (5343 + 2 * (k)) /* 5344 + 2k (once a dock's child port) is unused */
#define ELGDOCK_IDLE_MS 100 /* on_traffic(NULL) at least this often while a connection is quiet */

typedef struct {
    /* Companion opened (companion true, peer_ip its address) or closed a connection to d on its
     * dock's port. Runs on the dock's own task, with d valid (elgdock_release waits for it), never after
     * elgdock_release(d) returned. While Companion owns d, elgdock takes its input
     * (usbdeck_set_raw_input) and writes all Companion sends to it: draw nothing on it then. A new
     * connection that replaces the one being served (a hand-over) calls on_owner(d, true, its peer)
     * again, with no on_owner(d, false) between: the deck stays Companion's throughout. */
    void (*on_owner)(usbdeck_t *d, bool companion, const char *peer_ip);
    /* May be NULL. Every output report (feature false) and feature report to set (feature true)
     * Companion sends to d, in order, on the dock's own task, before it is passed on: return true to
     * have elgdock pass it on as it is, false when the callback took care of it (passed it on itself
     * through elgdock_write, or kept it back). report NULL (len 0): the connection is quiet, called
     * at least every ELGDOCK_IDLE_MS. Nothing of Companion's reaches d while it runs. d is only to be
     * compared and given to elgdock_write: the callback may still run while elgdock_release(d) is
     * under way (elgdock_write then fails). */
    bool (*on_traffic)(usbdeck_t *d, bool feature, const uint8_t *report, size_t len);
    /* May be NULL. The gesture's gate (elgdock_set_gesture; the chord never reaches Companion; while armed
     * a lone press of one of its two keys reaches it at most 120 ms late, every other key at once): open, the gesture
     * made on d while Companion drives it; else a gated key's press just reached Companion (the box clicks for it then).
     * On the deck's task or its dock's send task (small stacks), with no elgdock lock held: record only. */
    void (*on_corner)(usbdeck_t *d, bool open);
} elgdock_cb_t;

typedef struct {
    int dock;
    uint16_t port;  /* primary */
    bool listening; /* primary */
    char serial[32]; /* assigned deck, "" when none */
    char model[32];
    bool companion; /* Companion has a connection to the deck open */
    char peer[16];  /* its address */
} elgdock_status_t;

/* Starts the dock server task (it opens and announces the docks' ports as decks are assigned; mDNS
 * must be initialised already, see discovery_start). mac: the box's Ethernet MAC, the decks' report
 * 0x85. ndocks (the deck limit, at most ELGDOCK_MAX): the docks the box runs, 0 .. ndocks - 1; only
 * they are ever opened and announced, and a deck finds no other. Strings are copied. Call once. */
esp_err_t elgdock_start(const char *box_name, const uint8_t mac[6], const elgdock_cb_t *cb, int ndocks);

/* A deck attached: gives it a dock, the one it had before (by serial, kept in NVS namespace
 * "elgdock") when that is free, else the lowest free one, and the dock opens and announces its port
 * (within ~100 ms). Returns the dock index, -1 when none is free or elgdock isn't running. The same
 * deck again returns its dock. */
int elgdock_assign(usbdeck_t *d, const deck_model_t *m, const char *serial);

/* d's dock index, -1 when it has none. Thread-safe. */
int elgdock_dock_of(const usbdeck_t *d);

/* From on_detach: frees d's dock, ends the Companion connection to d, and returns once the dock no
 * longer uses d; the dock then closes its port and withdraws its announcement (within ~100 ms, unless
 * the deck is back by then). No-op for a deck without a dock. */
void elgdock_release(usbdeck_t *d);

/* From on_traffic only (d's dock's own task): one report to d, in order, held back like Companion's
 * own while the deck's queue is full (feature: a feature report to set, cut to
 * USBDECK_FEATURE_MAX). false: the connection is ending (replaced or closing) or d left its dock:
 * write nothing more now. */
bool elgdock_write(usbdeck_t *d, bool feature, const uint8_t *report, size_t len);

/* The box's maintenance screen on demand (on one deck) takes the input of the decks named (by
 * serial: the deck it is on, and any deck still handing back after it) from Companion: a replug of one, or
 * its dock handed over, starts held again; every other deck, one plugged in meanwhile included, goes on
 * untouched (the Pedal is never held). While held, a deck's input reports reach the box only (usbdeck's
 * on_input) and Companion gets none, though its connection goes on; holding it first sends Companion an
 * all-released report for what it last saw pressed (keys, encoder pushes). A deck left out of the list:
 * its reports go to Companion again, with a key still held from the session passed on as released until
 * it is let go (ingate.h). n 0: none held. Any task. */
void elgdock_hold_input(const char *const *serials, int n);

/* The box menu gesture every dock's gate runs (corner_cfg_use: the default until given), each deck's gate starting
 * again with it when it changed (anything held back of the gesture before is forgotten). false, and nothing done,
 * while elgdock isn't running (give it again after elgdock_start). Any task; one caller at a time keeps the order. */
bool elgdock_set_gesture(const corner_cfg_t *g);

/* Dock k's send task (docktxK): its stack never used so far, bytes; 0 when it has none yet. Any task. */
unsigned elgdock_tx_stack_free(int k);

/* One entry per dock the box runs (up to max; ndocks of them); returns the count. Thread-safe. */
int elgdock_status(elgdock_status_t *out, int max);

/* Box name or the mDNS setting changed: announces the listening docks under the new name ("<model>
 * @ <box name>"), or withdraws them (on false). Announcing is on after elgdock_start. */
void elgdock_set_announce(const char *box_name, bool on);

#ifdef __cplusplus
}
#endif
