#pragma once
/* Pure: the Network Dock mode image cache and gate of one deck (dock mode). Everything
 * Companion sends to the deck through its dock passes through here, in order, on the dock's own
 * task; this module writes it on (dc_io_t.write), keeps the last complete image of every target
 * and, while a message is shown, holds Companion's key images back and draws the box's message
 * frames instead.
 *
 *  - Cache: every image report is parsed (deckproto_parse_image_report) and its sequence kept per
 *    target (key, LCD region, info bar) as the raw reports. Only a complete sequence (first chunk to
 *    the last, no gap) replaces the target's image; an incomplete one is dropped. The gen2 key fill
 *    (03 06) updates its key entry. The gen2 panel ops, clear (03 05, "fill the entire LCD" with the
 *    colour at +2) and logo (03 02), cover the whole panel: every key, and the strip or info bar inside
 *    it (Plus, Plus XL, Neo); the last one is kept itself, and what it covered is forgotten. gen1 has
 *    no such reports (its clear is images; its reset 0b 63 is passed on and not tracked). PSRAM via
 *    dc_mem_t; bounded per target (dc_target_cap) and per deck (dc_bound_bytes).
 *  - Hold (dc_set_hold): Companion's key image sequences that start, and key fills and panel ops that
 *    come, while it is on are held back: they only update the cache. LCD strip and info bar images
 *    and every other feature report (brightness, ...) keep going to the deck at once, as in Satellite
 *    mode where the message is drawn on the LCD keys only. A sequence already being passed on when
 *    the hold starts is passed on to its end; a report of no sequence known is passed on too.
 *  - The box's own writes (message frames from dc_box_key, replays) only ever go between Companion's
 *    image sequences: never while one that is being passed on is open (a deck takes an image's
 *    reports in one run, as the host library and usbdeck both send them).
 *  - When the hold ends, every key the message was drawn on and every target that changed meanwhile
 *    gets its latest state back from the cache, in the order Companion sent them. A held panel op
 *    goes at its turn, and everything newer than it is written again after it (strip and info images
 *    passed on meanwhile included: the op covers them): the deck then shows exactly what it would
 *    have shown had nothing been held. A key with no cached state is filled black (gen2; a gen1 key
 *    keeps the message frame).
 *  - dc_replay_all (a quarantine lifted: the deck lost what was sent to it) writes everything kept,
 *    the kept panel op first. dc_end_sequences: the connection was replaced (hand-over); the cache
 *    and the frames stay, as the deck still shows them.
 *
 * Not thread-safe: the caller serialises every call per deck. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "deckmodel/model.h"
#include "deckproto/deckproto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_KEYS_MAX 40      /* most LCD keys of any model (Plus XL: 36) */
#define DC_LCD_MAX 8        /* LCD strip regions kept per deck (Companion draws 1 + one per encoder) */
#define DC_SEQ_MAX 4        /* image sequences in progress at once (the host library sends one at a time) */
#define DC_STALE_MS 5000    /* a sequence with no report for this long is abandoned (Companion's own
                             * timeout is 5 s; elgdock drops a connection quiet for 6 s) */
#define DC_SLACK_BYTES 1024 /* per target: room above w*h*3 for headers (BMP) and coding overhead */

typedef struct {
    void *(*alloc)(size_t n);
    void *(*resize)(void *p, size_t n); /* may be NULL: buffers are then never shrunk */
    void (*release)(void *p);
} dc_mem_t;

/* Writes one report to the deck in order (feature: SET_REPORT(Feature), else an output report).
 * false: stop writing (the connection is ending or the deck is gone). */
typedef bool (*dc_write_fn)(void *ctx, bool feature, const uint8_t *report, size_t len);

typedef struct {
    dc_write_fn write;
    void *ctx;
    uint32_t now_ms;
} dc_io_t;

typedef enum { DC_NONE, DC_IMAGE, DC_FILL } dc_kind_t;

typedef struct {
    uint8_t kind;   /* dc_kind_t */
    bool dirty;     /* the deck doesn't show it: write it when the hold allows */
    uint8_t rgb[3]; /* DC_FILL */
    uint16_t n;     /* DC_IMAGE: reports */
    uint8_t *reps;  /* DC_IMAGE: n reports of the model's report length */
    uint32_t stamp; /* the order Companion sent them in */
    deck_img_target_t t;
} dc_entry_t;

typedef struct {
    bool active;
    bool held;    /* held back (else passed on) */
    bool nocache; /* too big, out of memory or broken: passed on or dropped, never kept */
    bool broken;  /* a chunk went missing: the deck can't show it either */
    deck_img_target_t t;
    uint16_t next; /* chunk index expected next */
    uint16_t n, cap;
    uint8_t *reps;
    uint32_t at_ms;
} dc_seq_t;

typedef struct {
    uint8_t *reps; /* NULL: none */
    uint16_t n;
    bool written;
} dc_box_t;

typedef struct {
    uint32_t held_reports;  /* Companion reports held back */
    uint32_t held_images;   /* complete images among them, now in the cache */
    uint32_t replayed;      /* targets written back from the cache */
    uint32_t box_frames;    /* message frames written */
    uint32_t uncached;      /* images that could not be kept (too big, no memory, broken) */
} dc_stats_t;

typedef struct {
    const deck_model_t *m;
    dc_mem_t mem;
    bool hold;
    uint32_t stamp;
    dc_entry_t key[DC_KEYS_MAX];
    dc_entry_t lcd[DC_LCD_MAX];
    dc_entry_t info;
    dc_seq_t seq[DC_SEQ_MAX];
    dc_box_t box[DC_KEYS_MAX];
    bool box_drew[DC_KEYS_MAX]; /* a message frame is on the key: restore it when the hold ends */
    bool hold_all;              /* dc_set_hold_all: the hold covers the strip and the info bar */
    dc_box_t box_lcd[DC_LCD_MAX]; /* the box's strip frames, one per region (box_lcd_t) */
    deck_img_target_t box_lcd_t[DC_LCD_MAX];
    dc_box_t box_info;
    bool box_drew_lcd, box_drew_info; /* a box frame is on the strip / the info bar */
    bool panel_valid;           /* the last panel op (clear / logo) is kept */
    bool panel_dirty;           /* ... and the deck hasn't had it (held, or a replay asks for it) */
    uint8_t panel_rep[32];
    size_t panel_len;
    uint32_t panel_stamp;
    /* brightness. Companion's last report (kept, written back when on demand ends); the box's while
     * on demand covers the deck */
    bool comp_bright_known, comp_bright_due;
    uint8_t comp_bright_rep[32];
    size_t comp_bright_len;
    bool box_bright_due, box_bright_on; /* to write; the box set the deck's brightness this time */
    uint8_t box_bright_rep[32];
    size_t box_bright_len;
    dc_stats_t stats;
} dc_t;

void dc_init(dc_t *c, const deck_model_t *m, const dc_mem_t *mem);
/* Frees every image and frame, forgets every sequence; the hold state stays. */
void dc_reset(dc_t *c);

/* Companion's output report: kept, then written on (or held back). Then pending work (dc_pump).
 * false when a write failed. */
bool dc_output(dc_t *c, const dc_io_t *io, const uint8_t *report, size_t len);
/* Companion's feature report to set: written on (or, a key fill or panel clear in a hold, held
 * back), then pending work. false when a write failed. */
bool dc_feature(dc_t *c, const dc_io_t *io, const uint8_t *report, size_t len);
/* Pending work, when Companion has no image sequence open: message frames while holding, the
 * cache replay after a hold or dc_replay_all. Call it when the connection is idle too (an open
 * sequence goes stale after DC_STALE_MS). false when a write failed. */
bool dc_pump(dc_t *c, const dc_io_t *io);
/* Starts or ends the hold. Ending it frees the message frames and asks for the replay. */
void dc_set_hold(dc_t *c, bool on);
/* The box's message frame for LCD key hw (an image as usbdeck_write_key takes it), written at the
 * next chance while the hold lasts; replaces an earlier one. false (nothing kept) without a hold,
 * for a key the model doesn't have, an image over dc_target_cap, or out of memory. */
bool dc_box_key(dc_t *c, uint8_t hw, const uint8_t *img, size_t len);
/* The maintenance screen on demand: while the hold is on, it covers the strip and the info bar too
 * (Companion's images there are held back and kept, as its key images are), and the box draws its own
 * frames there (dc_box_lcd, dc_box_info). Turning it off, or the hold ending, gives the regions the box
 * drew on Companion's latest state back (a region with nothing kept keeps the box's frame until
 * Companion draws there). Off: the hold is the message's, keys only. */
void dc_set_hold_all(dc_t *c, bool all);
/* The box's frame for the strip region x, y, w, h / for the info bar (an image as usbdeck_write_lcd /
 * usbdeck_write_info takes it), written at the next chance while the hold covers them; replaces an
 * earlier one for the same region. false (nothing kept) without hold_all, for a target the model
 * doesn't have, an image over dc_target_cap, DC_LCD_MAX regions already, or out of memory. */
bool dc_box_lcd(dc_t *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *img, size_t len);
bool dc_box_info(dc_t *c, const uint8_t *img, size_t len);
/* the box's brightness for the deck while the maintenance screen on demand covers it (hold_all),
 * written at the next chance; Companion's brightness reports meanwhile are kept, not written, and when
 * hold_all ends Companion's last one goes back to the deck (none known: the deck keeps the box's).
 * false without hold_all. */
bool dc_box_brightness(dc_t *c, uint8_t pct);
/* Writes everything kept again (the keys under a message once it ends). */
void dc_replay_all(dc_t *c);
/* The connection was replaced (hand-over): the images it had under way can never finish. */
void dc_end_sequences(dc_t *c);
/* Nothing left to write: no replay pending and, while holding, every message frame written. */
bool dc_settled(const dc_t *c);
/* Bytes held in images, frames and sequences now. */
size_t dc_bytes(const dc_t *c);

/* Reports one image of the target may take (0: none kept, e.g. a region larger than the strip):
 * ceil((w * h * 3 + DC_SLACK_BYTES) / payload per report). */
size_t dc_target_cap(const deck_model_t *m, const deck_img_target_t *t);
/* The most the cache of one deck of model m can hold, in bytes: every key image, DC_LCD_MAX strip
 * images and the info bar at their caps, a box frame for each of them (keys, strip regions, info bar),
 * and DC_SEQ_MAX sequences at the largest cap. */
size_t dc_bound_bytes(const deck_model_t *m);

#ifdef __cplusplus
}
#endif
