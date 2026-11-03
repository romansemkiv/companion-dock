#pragma once
/* Pure: the gate between one deck's input reports and Companion in Network Dock mode, for the box's
 * maintenance screen on demand (while it is open, no press reaches Companion). Host-tested
 * (test/host/test_ingate.c); elgdock_deck.c runs one per dock under its in_mtx.
 *
 * - The hold is per deck, by serial (the deck the screen on demand is on, and any still handing back
 *   after it): elgdock_hold_input names them, and such a deck assigned to a dock (again) while held starts
 *   held (ingate_init with ingate_held), so a replug or a dock handed over can never slip through; every
 *   other deck passes.
 *   Only decks with a screen are held (the Pedal keeps going to Companion).
 * - Turning it on: Companion gets an all-released copy of the last key report and of the last
 *   encoder-push report, when anything in them was pressed, so nothing stays held down in Companion.
 * - While on: every report stays with the box (usbdeck still hands it to on_input).
 * - Turning it off: whatever is held down then is masked: passed on as released until the deck reports
 *   it released, so a key held through the session never reaches Companion as a press.
 * The layouts are deckproto_parse_input's: gen1 01 + keys; gen2 01 00 len16 + keys, 01 03 05 00 00 +
 * encoder pushes; any other report (an encoder turn, a touch on the strip) has no state to release. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INGATE_MAX 64 /* bytes of a report kept: all input data of every model (usbdeck hands on 64) */

typedef struct {
    bool gen2;   /* the reports carry a header of 4 (keys) or 5 (encoder pushes) bytes; gen1: 1 */
    bool screen; /* the deck has a screen: only those are held */
    bool hold;   /* reports stay with the box */
    uint8_t keys[INGATE_MAX], enc[INGATE_MAX]; /* the last key report, the last encoder-push report */
    uint8_t keys_len, enc_len;                 /* 0: none seen */
    uint8_t mask_keys[INGATE_MAX], mask_enc[INGATE_MAX]; /* 1: held through a hold, passed on released */
} ingate_t;

/* the deck with serial is held: one of the n serials in hold (the deck the screen on demand is on,
 * and any deck still handing back after it; "" entries never match). */
bool ingate_held(const char *serial, const char *const *hold, int n);

/* A deck newly assigned to a dock: nothing known pressed, nothing masked; held from now on when
 * hold_all (the box-wide hold) is on and it has a screen. */
void ingate_init(ingate_t *g, bool gen2, bool screen, bool hold_all);

/* The box-wide hold turned on or off. On: the all-released copies to send Companion (up to 2) into
 * out[] with their lengths; returns how many. Off: what is pressed now is masked; returns 0. No change,
 * or a deck without a screen: 0. */
int ingate_hold(ingate_t *g, bool hold, uint8_t out[2][INGATE_MAX], uint8_t out_len[2]);

/* A report from the deck (kept as the deck's state either way). true: pass out on to Companion, its first
 * min(len, INGATE_MAX) bytes with the masked states released; false: it stays with the box. */
bool ingate_report(ingate_t *g, const uint8_t *r, size_t len, uint8_t *out);

#ifdef __cplusplus
}
#endif
