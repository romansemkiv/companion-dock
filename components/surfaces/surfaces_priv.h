#pragma once
/* Between surfaces.c and dockmode.c (Network Dock mode). ESP-only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "deckmodel/model.h"
#include "esp_err.h"
#include "usbdeck/usbdeck.h"

/* ---- dockmode.c ---- */

/* Starts elgdock (docks announced unless announce is false). */
esp_err_t dockmode_start(const char *box_name, const uint8_t mac[6], bool announce, int ndocks); /* ndocks docks */
/* usbdeck callbacks, in dock mode (deck tasks): a deck gets a dock and an image cache / leaves its
 * dock (returns once elgdock no longer uses d) / its quarantine ended (Companion's images come back
 * from the cache while Companion drives it). */
void dockmode_attach(usbdeck_t *d, const deck_model_t *m, const char *serial);
void dockmode_detach(usbdeck_t *d);
void dockmode_restored(usbdeck_t *d);
/* Surfaces task: whether a custom message is shown. While it is, Companion's key images for every
 * deck it drives are held back (kept in the cache); when it ends the cache is written back. */
void dockmode_message(bool on);
/* Surfaces task: the decks the box menu is open on (n 0: none; d only compared).
 * While it is, Companion's images for that deck are held back, the strip's and the info bar's too, and
 * the box's frames take their place; when it closes the cache is written back. Other decks go on. */
void dockmode_maint(const usbdeck_t *const *d, int n); /* the decks the box menu is open on */
/* imgwork: the box's frame (a message's or the maintenance screen's) for LCD key hw / the strip region
 * x, y, w, h / the info bar of a deck Companion drives (an image as usbdeck_write_key / _lcd / _info
 * takes it; copied). The dock's task writes it between Companion's images; replaces an earlier one not
 * yet written. Dropped for a deck without a cache. */
void dockmode_box_key(usbdeck_t *d, uint8_t hw, const uint8_t *img, size_t len);
void dockmode_box_lcd(usbdeck_t *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *img, size_t len);
void dockmode_box_info(usbdeck_t *d, const uint8_t *img, size_t len);
/* the box's brightness for a deck Companion drives while on demand covers it (dc_box_brightness): the
 * dock's task writes it between Companion's images; when on demand ends Companion's last one goes back. */
void dockmode_box_brightness(usbdeck_t *d, uint8_t pct);
/* d's dock's port (the one Companion adds), 0 when d has no dock (all six taken). d is only compared. */
uint16_t dockmode_port(const usbdeck_t *d);
/* The maintenance screen on demand takes its deck's input from Companion, and keeps that of any deck
 * still handing back (elgdock_hold_input; by serial; n 0: none). */
void dockmode_hold_input(const char *const *serials, int n);
/* Companion's images are all back on d after the box's frames (the hold ended and the cache is written
 * back), or there is nothing of Companion's to bring back. d is only compared. */
bool dockmode_settled(const usbdeck_t *d);
void dockmode_apply(const char *box_name, bool announce);

/* ---- surfaces.c ---- */

/* Companion took deck d over through its dock (owned: the box draws nothing on it from now on but
 * its message frames and the maintenance screen on demand, which go to dockmode_box_*, and nothing it
 * queued before reaches the deck after this returns) or let it go (the box's screens come back). */
void surfaces_dock_owner(usbdeck_t *d, bool owned, const char *peer_ip);
/* Dock mode (elgdock's corner gate): open, the gesture made on deck d while Companion drives it:
 * the maintenance screen on demand opens on that deck, unless it is open already; else a corner's press
 * reached Companion (its click). Any task: it only records it and wakes the surfaces task, which opens, logs
 * and clicks (docktxK's 3 KB stack overflowed when the open ran on it). */
void surfaces_dock_corner(usbdeck_t *d, bool open);
