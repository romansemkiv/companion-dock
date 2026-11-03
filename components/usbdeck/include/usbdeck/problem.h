#pragma once
/* Problem texts usbdeck reports through on_problem. Pure (no ESP-IDF): shared with surfaces, which
 * keeps them, and with usbdeck's host-tested policy, which formats some of them. */
#include <stddef.h>
#include <string.h>

/* Bytes of one problem text, NUL included: surfaces keeps entries this size, longer texts are cut. */
#define USBDECK_PROBLEM_MAX 96

/* The text about a deck quarantined for stalling USB starts with USBDECK_QUARANTINE_PREFIX (printf
 * format, one %s: the serial usbdeck reports, which has no spaces), then says what is off and in how
 * many minutes it is retried. usbdeck raises it right after on_attach while the deck is quarantined
 * (and when a reset quarantines it), so it is current only until the deck's on_restored, on_detach
 * or next on_attach; a newer one for the same deck replaces it. */
#define USBDECK_QUARANTINE_HEAD "Stream Deck "
#define USBDECK_QUARANTINE_TAIL " keeps stalling USB: "
#define USBDECK_QUARANTINE_PREFIX USBDECK_QUARANTINE_HEAD "%s" USBDECK_QUARANTINE_TAIL

/* When msg is a quarantine text: the length of its per-deck part (USBDECK_QUARANTINE_PREFIX with the
 * serial filled in), which two texts about the same deck share and two decks never do. Else 0. */
static inline size_t usbdeck_quarantine_key_len(const char *msg)
{
    const size_t h = sizeof USBDECK_QUARANTINE_HEAD - 1, t = sizeof USBDECK_QUARANTINE_TAIL - 1;
    if (!msg || strncmp(msg, USBDECK_QUARANTINE_HEAD, h) != 0) return 0;
    const size_t serial = strcspn(msg + h, " ");
    if (serial == 0 || strncmp(msg + h + serial, USBDECK_QUARANTINE_TAIL, t) != 0) return 0;
    return h + serial + t;
}
