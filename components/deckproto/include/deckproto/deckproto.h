#pragma once
/* Pure C: Stream Deck HID report builders and input parser (no ESP-IDF headers). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "deckmodel/model.h"

/* ---- Output (image) reports ---- */
typedef enum { DECK_W_NONE, DECK_W_GEN2_KEY, DECK_W_GEN2_LCD, DECK_W_NEO_INFO, DECK_W_GEN1 } deck_w_mode_t;

typedef struct {
    const deck_model_t *m;
    deck_w_mode_t mode;
    const uint8_t *img;
    size_t len, off;
    uint16_t page;
    uint8_t key;
    uint16_t x, y, w, h;
    size_t half; /* Original v1: bytes in the first page (0 for other gen1) */
} deck_img_writer_t;

/* Start writing an image. hw_key is the HID key index. The image bytes must stay valid until
 * deckproto_img_next returns false. Unsupported model/combination leaves the writer empty. */
void deckproto_img_key(deck_img_writer_t *w, const deck_model_t *m, uint8_t hw_key, const uint8_t *img, size_t len);
void deckproto_img_lcd(deck_img_writer_t *w, const deck_model_t *m, uint16_t x, uint16_t y, uint16_t width,
                       uint16_t height, const uint8_t *jpeg, size_t len);
void deckproto_img_info(deck_img_writer_t *w, const deck_model_t *m, const uint8_t *jpeg, size_t len);
/* Fills exactly m->report_len bytes (zero padded) and returns true; false when done. */
bool deckproto_img_next(deck_img_writer_t *w, uint8_t *report);

/* ---- Parsing output (image) reports: the inverse of the writers above ---- */
typedef enum { DECK_TGT_KEY, DECK_TGT_LCD, DECK_TGT_INFO } deck_tgt_kind_t;

typedef struct {
    deck_tgt_kind_t kind;
    uint8_t key;         /* DECK_TGT_KEY: HID key index */
    uint16_t x, y, w, h; /* DECK_TGT_LCD: the region as the header carries it (the strip's own, unturned) */
} deck_img_target_t;

/* One output report of an image, as deckproto_img_next (or the Stream Deck host library, which
 * writes the same layouts) makes it: the target, the chunk index it carries, whether it is the
 * image's last report, and its image bytes (*payload points into report). False for anything else:
 * not an image report, an output the model doesn't have, a key it doesn't have, or a header whose
 * lengths don't fit the report. gen2 reports carry their payload length. gen1 reports don't: their
 * payload is the whole report after the 16-byte header, cut to the model's report length (the image
 * bytes, then zeros). Chunk indexes count from deckproto_image_first_chunk. */
bool deckproto_parse_image_report(const deck_model_t *m, const uint8_t *report, size_t len, deck_img_target_t *target,
                                  uint16_t *chunk_index, bool *last, const uint8_t **payload, size_t *payload_len);
/* The chunk index of an image's first report: 1 on the Original v1 (its pages are 1 and 2), else 0. */
uint16_t deckproto_image_first_chunk(const deck_model_t *m);

/* ---- Feature reports (buffer >= 32 bytes; return m->feature_len, 0 if unsupported) ---- */
size_t deckproto_feat_brightness(const deck_model_t *m, uint8_t pct, uint8_t *buf);
size_t deckproto_feat_reset(const deck_model_t *m, uint8_t *buf);
size_t deckproto_feat_fill_key(const deck_model_t *m, uint8_t hw, uint8_t r, uint8_t g, uint8_t b, uint8_t *buf);

uint8_t deckproto_serial_report_id(const deck_model_t *m);
bool deckproto_parse_serial(const deck_model_t *m, const uint8_t *rep, size_t len, char *out, size_t cap);
uint8_t deckproto_fw_report_id(const deck_model_t *m);
bool deckproto_parse_fw(const deck_model_t *m, const uint8_t *rep, size_t len, char *out, size_t cap);
/* the brightness a feature report sets (deckproto_feat_brightness's layout, capped at 100), or -1
 * for any other report. */
int deckproto_feat_brightness_of(const deck_model_t *m, const uint8_t *rep, size_t len);

/* The feature reports read when a deck is opened and kept (usbdeck answers GET_REPORT only from
 * these copies afterwards: on the Module 32 a GET after image data never completed and left EP0
 * stuck). The box's own serial and firmware reports plus every report @elgato-stream-deck/core
 * 7.8.0 reads for the model. Writes the IDs (no duplicates) to out; returns how many. */
#define DECK_KEPT_REPORTS_MAX 4
size_t deckproto_kept_report_ids(const deck_model_t *m, uint8_t out[DECK_KEPT_REPORTS_MAX]);

/* ---- Input ---- */
typedef enum {
    DECK_EV_KEY,
    DECK_EV_ENC_PRESS,
    DECK_EV_ENC_ROTATE,
    DECK_EV_TOUCH_TAP,
    DECK_EV_TOUCH_LONG,
    DECK_EV_TOUCH_SWIPE
} deck_ev_kind_t;

typedef struct {
    deck_ev_kind_t kind;
    uint8_t index; /* HID index as in the report */
    bool pressed;
    int8_t delta;
    uint16_t x, y, x2, y2;
} deck_event_t;

typedef struct {
    uint8_t keys[40];
    uint8_t enc[8];
} deck_input_state_t;

/* Emits edges only. Edges beyond max are not lost: their state is left unchanged and they are
 * reported by a later call with the same (or a newer) report. State is updated per stored event. Returns the number of events written (<= max). */
int deckproto_parse_input(const deck_model_t *m, deck_input_state_t *st, const uint8_t *rep, size_t len,
                          deck_event_t *out, int max);
