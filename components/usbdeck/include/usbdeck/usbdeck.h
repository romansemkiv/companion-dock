#pragma once
/* USB host (hubs enabled) + raw HID access to Elgato Stream Decks. ESP-only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "deckmodel/model.h"
#include "deckproto/deckproto.h"
#include "usbdeck/problem.h"

typedef struct usbdeck usbdeck_t; /* opaque; valid from on_attach until on_detach returns */

/* All callbacks run on usbdeck's own tasks (a per-deck task, or the USB client task for
 * on_problem), never inside the USB host library's callbacks. Keep them short and never block
 * on another deck's work. `serial` and `msg` are only valid during the call: copy them.
 * on_restored (deck task, between on_attach and on_detach, may be NULL): the deck's quarantine for
 * stalling USB ended while it stayed plugged in (it is retried automatically, see the write calls
 * below). Every image written to it meanwhile was dropped, so redraw all of it.
 * A quarantined deck's problem text (usbdeck/problem.h) is raised right after its on_attach, and
 * when a reset quarantines it: it is obsolete at the deck's on_restored, on_detach and next
 * on_attach. */
typedef struct {
    void (*on_attach)(usbdeck_t *, const deck_model_t *, const char *serial);
    void (*on_detach)(usbdeck_t *); /* once per on_attach; the handle is freed after it returns */
    void (*on_input)(usbdeck_t *, const deck_event_t *);
    void (*on_problem)(const char *msg); /* at most USBDECK_PROBLEM_MAX bytes with the NUL */
    void (*on_restored)(usbdeck_t *);
} usbdeck_cb_t;

/* Installs the USB host library on the high-speed root port, registers the client, powers the
 * port. Call once; `cb` is copied. */
esp_err_t usbdeck_start(const usbdeck_cb_t *cb);

/* Queued per deck (data copied, written by the deck task in order). Never block:
 * ESP_ERR_INVALID_STATE after the deck is gone, ESP_ERR_TIMEOUT if its queue is full (48),
 * ESP_ERR_NO_MEM if the copy can't be allocated, ESP_ERR_NOT_SUPPORTED if the model has no
 * such output, ESP_ERR_INVALID_ARG for bad arguments. Images for a deck quarantined for stalling
 * USB (reported as a problem) are dropped at once: ESP_OK, nothing queued. The quarantine ends by
 * itself (after 10 min, doubling to 60 min if it keeps stalling) and on_restored then asks for a
 * full redraw. */
esp_err_t usbdeck_write_key(usbdeck_t *d, uint8_t hw, const uint8_t *img, size_t len);
esp_err_t usbdeck_write_lcd(usbdeck_t *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const uint8_t *jpeg, size_t len);
esp_err_t usbdeck_write_info(usbdeck_t *d, const uint8_t *jpeg, size_t len);
esp_err_t usbdeck_fill_key(usbdeck_t *d, uint8_t hw, uint8_t r, uint8_t g, uint8_t b);
esp_err_t usbdeck_set_brightness(usbdeck_t *d, uint8_t pct);
/* Everything handed to the deck so far (the writes above and the raw passthrough below) has been
 * written: its command queue is empty and nothing is being sent, so an image queued before this was
 * looked at is on the deck (or was dropped: a quarantined deck, one that doesn't take reports). A write
 * queued after the look is not covered. Lock-free and never blocks; the handle must be valid. */
bool usbdeck_writes_drained(const usbdeck_t *d);

/* ---- Raw passthrough (Network Dock mode) ----
 * Queued like the writes above and run by the deck's task in order with them, through the same
 * box-wide OUT/IN gate. Unlike them each call waits up to wait_ms for queue space and only then
 * fails with ESP_ERR_TIMEOUT, so a caller that must not drop data can hold back instead (and
 * retry). Their caller's on_detach must not return while it is inside one of them: the handle is
 * freed then. A deck that is gone gives ESP_ERR_INVALID_STATE at once. */
#define USBDECK_FEATURE_MAX 32 /* longest feature report sent or read */

/* One output report as given: sent as one interrupt OUT report (SET_REPORT(Output) on a deck
 * without that endpoint), padded with zeros or cut to the model's report length. While the deck is
 * quarantined it is dropped with ESP_OK, as images are. */
esp_err_t usbdeck_write_raw(usbdeck_t *d, const uint8_t *report, size_t len, uint32_t wait_ms);
/* SET_REPORT(Feature) of buf: buf[0] = report ID, 1..USBDECK_FEATURE_MAX bytes. */
esp_err_t usbdeck_feature_set(usbdeck_t *d, const uint8_t *buf, size_t len, uint32_t wait_ms);
/* GET_REPORT(Feature) of report_id, answered by the deck task in order with the writes (so after
 * every write queued before it), from the copy read when the deck was opened: after open the deck is
 * never asked a feature report again, as on the Module 32 a GET_REPORT right after image data never
 * completed and left EP0 stuck until the deck was replugged (seen on hardware). The copies are
 * the reports deckproto_kept_report_ids names: the serial number and firmware reports, as the
 * Stream Deck host library reads them. Any other report: ESP_ERR_NOT_FOUND. Waits up to wait_ms in
 * all (queue space and its turn): ESP_ERR_TIMEOUT after that. The copy (starting with the report
 * ID, at most USBDECK_FEATURE_MAX bytes) goes to buf, cut to cap, its length to *out_len. */
esp_err_t usbdeck_feature_get(usbdeck_t *d, uint8_t report_id, uint8_t *buf, size_t cap, size_t *out_len,
                              uint32_t wait_ms);
/* The same copy at once, not in order with the writes. Thread-safe while the handle is valid. */
esp_err_t usbdeck_feature_kept(const usbdeck_t *d, uint8_t report_id, uint8_t *buf, size_t cap, size_t *out_len);
/* While fn is set, every input report goes to fn on the deck task first (starting with its report ID
 * 0x01; the first 64 bytes, which hold all input data of every model), and is then parsed for
 * on_input all the same (the box's key click: the report has already gone on). fn must not block.
 * NULL: on_input only. A call of fn already under way may end after this returns: ctx must stay
 * valid. */
typedef void (*usbdeck_raw_input_fn)(void *ctx, const uint8_t *report, size_t len);
void usbdeck_set_raw_input(usbdeck_t *d, usbdeck_raw_input_fn fn, void *ctx);
/* A raw input fn is set (dock mode: Companion drives the deck). Lock-free: for on_input, on the deck
 * task. */
bool usbdeck_passthrough(const usbdeck_t *d);

typedef struct {
    char model[32];
    char serial[32];
    char fw[16];
    bool high_speed;
    uint8_t hub_depth; /* 0 = root port */
    uint8_t port;      /* parent hub port number, 0 = root port */
} usbdeck_info_t;

/* The deck's serial, as passed to on_attach (valid from on_attach until on_detach returns). */
const char *usbdeck_serial(const usbdeck_t *d);
/* The deck's firmware version as read at open (usbdeck_info_t.fw, e.g. "2.02.000"; "" when unread),
 * valid as usbdeck_serial. */
const char *usbdeck_firmware(const usbdeck_t *d);
/* The deck task's stack never used so far, bytes (uxTaskGetStackHighWaterMark; 0 without a task). The handle
 * must be valid. */
unsigned usbdeck_stack_free(const usbdeck_t *d);

/* Copies up to `max` attached decks; returns the count. Thread-safe. */
int usbdeck_list(usbdeck_info_t *out, int max);

/* Deck trouble, for the status light and sound (components/feedback). Thread-safe. */
typedef struct {
    uint32_t reports; /* every report since the start that a deck stopped responding, isn't taking
                       * images, is being reset for it or is quarantined (on_problem says which) */
    int troubled;     /* attached decks that are quarantined, or not responding / not taking images
                       * now (a reported streak with a failure in the last UPOL_FAILING_HOLD_S, 10 s) */
} usbdeck_trouble_t;
void usbdeck_trouble(usbdeck_trouble_t *out);

/* True once usbdeck_start has brought the USB host up (library installed, client registered, root
 * port powered); it stays up from then on. */
bool usbdeck_running(void);
