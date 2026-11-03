#pragma once
/* Internal interfaces shared by usb_host_task.c, hid_raw.c and deck_dev.c. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usbdeck/usbdeck.h"

#define USBDECK_TAG "usbdeck"
#define USBDECK_MAX 6
#define USBDECK_MSG_FS_ON_HUB "A full-speed device on a hub can't be used — plug it straight into the box"

/* ---- usb_host_task.c ---- */

/* Which DWC OTG controller (and so which root port / PHY) the host library runs on. Only one is
 * supported by IDF 5.5's host library; the full-speed one is for a later second USB-A socket. */
typedef enum { USBDECK_CTRL_HS, USBDECK_CTRL_FS } usbdeck_ctrl_t;

usb_host_client_handle_t usbdeck_client(void);
/* Last resort for a wedged deck (only via deck_dev_escalate, which first checks
 * usbdeck_root_settled and usbdeck_bus_resetting): root port power off now; the usbdeck service
 * task powers it back on once `culprit` is gone (retrying while the port recovers, esp_restart if
 * USB doesn't come back). Every device on the port detaches and re-enumerates. False if a reset
 * is already in progress. IDF 5.5 hub.c has two ways to abort on a root-port power-off:
 * - hub_root_stop asserts if the port has an event pending at that instant, e.g. the deck
 *   dropped off by itself at the same moment; the public API offers no guard;
 * - root_port_handle_events aborts on a power-off after a power-on, before the root device's
 *   connection was handled (no device-tree node yet): usbdeck_root_settled guards that after our
 *   own power-ons. IDF's own port recovery after a root device is unplugged isn't visible to a
 *   client, but can't race a reset either: the escalating deck holds its device open, so the root
 *   device (it, or the hub above it) keeps its node and the port isn't recycled meanwhile. */
bool usbdeck_bus_reset(usb_device_handle_t culprit);
bool usbdeck_bus_resetting(void);
/* False from our root-port power-on (at start and after a bus reset) until a device has finished
 * enumerating since, or UPOL_ROOT_SETTLE_US later (upol_root_settled). */
bool usbdeck_root_settled(void);

/* Work for the usbdeck service task (anything that may block on USB, off the esp_timer task). */
typedef enum { USBDECK_SVC_SWEEP = 1 << 0, USBDECK_SVC_RESET = 1 << 1 } usbdeck_svc_t;
void usbdeck_svc_request(usbdeck_svc_t what);

/* ---- hid_raw.c: one claimed HID interface with its interrupt endpoints ---- */

/* Called from the USB client task for every completed IN transfer (data valid only during the
 * call; with the RX FIFO sized for the IN packet an IN completes only with data, an idle one stays
 * queued) and once when the IN pipe stops on an error (st != COMPLETED, data NULL; this one may also
 * come from a task running the OUT gate, when restarting a parked IN transfer fails, even after
 * hid_raw_close has started, until it unregisters). */
typedef void (*hid_raw_in_cb_t)(void *ctx, usb_transfer_status_t st, const uint8_t *data, int len);

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    uint8_t intf, addr; /* addr: USB device address, for logs */
    bool claimed;
    uint8_t ep_in, ep_out; /* ep_out 0: no interrupt OUT, output reports go via SET_REPORT */
    uint16_t mps_in, mps_out, mps0;
    size_t report_len;
    usb_transfer_t *xin, *xout, *xctl;
    SemaphoreHandle_t out_done, ctl_done;
    /* Owner task only. out_stuck: an OUT transfer not even halt + flush brought back;
     * ctl_stuck: a control transfer still in flight after its timeout (EP0 can't be cancelled). */
    bool out_stuck, ctl_stuck;
    /* OUT pacing (open): packet spacing, packets per gated transfer. Write backoff (owner task):
     * after a late OUT transfer, reports fail fast until out_retry_us; 1 s doubling to 8 s. */
    bool out_late, out_quarantined; /* out_quarantined: hid_raw_set_out_quarantine */
    uint8_t out_binterval;
    uint16_t out_chunk_pkts;
    uint32_t out_interval_us, out_backoff_ms;
    int64_t out_retry_us;
    /* IN transfer, see the gate in hid_raw.c. in_state: hid_raw.c's lock. registered: owner task. */
    uint8_t in_state; /* hid_raw_in_state_t */
    bool registered;
    atomic_bool stopping, gone; /* gone: hid_raw_mark_gone */
    hid_raw_in_cb_t in_cb;
    void *in_ctx;
} hid_raw_t;

/* IN_IDLE: not started or stopped. IN_QUEUED: owned by the USB stack (or claimed for submission);
 * an idle deck's stays queued until it has data. IN_PARKED: completed while an OUT transfer held the
 * gate; the gate restarts it. */
typedef enum { IN_IDLE = 0, IN_QUEUED, IN_PARKED } hid_raw_in_state_t;

/* Once, before any hid_raw_open. */
esp_err_t hid_raw_init(void);
/* usbdeck service task: the IN window timer fired. */
void hid_raw_sweep_deferred(void);
/* Finds the first HID interface with an interrupt IN endpoint, allocates DMA-capable transfers
 * (OUT = report_len bytes) and claims the interface. On error call hid_raw_close anyway. */
esp_err_t hid_raw_open(hid_raw_t *h, usb_host_client_handle_t client, usb_device_handle_t dev,
                       size_t report_len);
/* Starts the single IN transfer. Its completion callback resubmits it, except while an interrupt
 * OUT transfer holds the box-wide gate: then it parks and the gate restarts it (hid_raw.c). A queued
 * IN is never waited for, halted or cancelled by the gate. */
esp_err_t hid_raw_start_in(hid_raw_t *h, hid_raw_in_cb_t cb, void *ctx);
/* After the IN pipe stopped on an error: clear the endpoint halt and resubmit. */
esp_err_t hid_raw_restart_in(hid_raw_t *h);
/* Buffer for the next output report (report_len bytes, DMA-capable); send with hid_raw_send_out.
 * Sending consumes the buffer contents (refill it for every report). Interrupt OUT data goes out
 * in gated transfers (no IN transfer resubmitted on any deck meanwhile) of at most ~8 ms bus time. One
 * not taken within ~500 ms is waited for (it can't be halted while the deck is there), cancelled
 * if the deck leaves, and 500 ms later the deck is escalated (deck_dev_escalate). After a late
 * transfer the handle backs off (1 s doubling to 8 s): hid_raw_send_out fails fast
 * (ESP_ERR_TIMEOUT) while hid_raw_out_backoff, without taking the gate. */
uint8_t *hid_raw_out_buf(hid_raw_t *h);
/* ESP_ERR_NOT_ALLOWED: nothing sent, the deck is quarantined. ESP_ERR_TIMEOUT also when an IN submit
 * already on its way into the HCD didn't return in time (the gate waits for those). */
esp_err_t hid_raw_send_out(hid_raw_t *h);
bool hid_raw_out_backoff(const hid_raw_t *h);
/* Owner task only (it alone sends OUT reports): no interrupt OUT at all while on
 * (ESP_ERR_NOT_ALLOWED). Set before the first report; cleared when the quarantine is lifted. */
void hid_raw_set_out_quarantine(hid_raw_t *h, bool on);
/* Feature reports over EP0 (rep[0] / id = report ID). Waits for completion (1000 ms), then 1 ms. */
esp_err_t hid_raw_set_feature(hid_raw_t *h, const uint8_t *rep, size_t len);
esp_err_t hid_raw_get_feature(hid_raw_t *h, uint8_t id, uint8_t *out, size_t cap, size_t *got);
/* USB client task: the device left. A late OUT transfer can then be cancelled. */
void hid_raw_mark_gone(hid_raw_t *h);
/* hid_raw (gate held) -> deck_dev: h's deck stopped taking OUT data (kind UPOL_STRIKE_OUT; the gate
 * raises no IN strike: a queued IN that never completes is an idle deck).
 * Starts the bus reset if allowed (box-wide 10 s floor; a reset that quarantines the deck only
 * needs 1 s since the last one; never while a reset is in progress, nor within 3 s of our last
 * power-on unless a device has enumerated since), recording it in the deck's record (keyed by
 * serial, else VID/PID and port path). The deck is quarantined (reported once) when it stalls
 * again within 30 s of re-attaching after our reset or of its quarantine's lift, or at its 4th
 * reset within 10 min. True if the reset started. So a stuck OUT holds the gate for at most ~10 s
 * past its escalation. */
bool deck_dev_escalate(hid_raw_t *h, int kind /* upol_strike_t */);
/* Stops IN, waits for in-flight transfers, releases the interface and frees the transfers.
 * ESP_OK means nothing of h is referenced by the USB stack any more (device may be closed). */
esp_err_t hid_raw_close(hid_raw_t *h);

/* ---- deck_dev.c ---- */

#define USBDECK_PATH_MAX 16
typedef struct {
    uint8_t addr;
    usb_device_handle_t dev; /* opened by the client; owned by the deck once spawned */
    bool high_speed;
    uint8_t hub_depth, port;
    char path[USBDECK_PATH_MAX]; /* hub port numbers from the root, e.g. "0" or "0.3": stable */
} deck_link_t;

esp_err_t deck_dev_init(const usbdeck_cb_t *cb);
/* Registers the deck and starts its task, which opens the HID interface and attaches. On error
 * the caller still owns (and must close) link->dev. Reports "too many decks" itself. */
esp_err_t deck_dev_spawn(const deck_link_t *link, const deck_model_t *m);
/* USB client callback context: marks the deck owning `dev` gone and wakes its task. */
void deck_dev_gone(usb_device_handle_t dev);
/* Logs and forwards to on_problem. Not for the USB host library's callback context. */
void usbdeck_problem(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
