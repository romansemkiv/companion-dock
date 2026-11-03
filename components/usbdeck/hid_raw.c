/* Thin raw-HID layer on the USB host library: claim, interrupt IN/OUT, feature reports. */
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usbdeck_policy.h"
#include "usbdeck_priv.h"

#define TAG USBDECK_TAG

#define XFER_TIMEOUT_MS 1000
#define FEATURE_GAP_US 1000 /* Stream Deck quirk: ~1 ms between feature reports */
#define FEATURE_GET_LEN 32
#define CTL_DATA_MIN 64
/* IDF 5.5 hcd_dwc.c: one interrupt transfer may span at most XFER_LIST_LEN_INTR (= 32) packets
 * and asserts beyond that without an earlier check: UPOL_INTR_MAX_PACKETS caps every chunk. */

#define HID_GET_REPORT 0x01
#define HID_SET_REPORT 0x09
#define HID_RT_OUTPUT 0x02
#define HID_RT_FEATURE 0x03
#define BM_CLASS_INTF_OUT 0x21
#define BM_CLASS_INTF_IN 0xA1

#define CLOSE_POLL_MS 10
#define RELEASE_TRIES 20
#define OUT_GONE_WAIT_MS 1000  /* after a bus reset, for the deck's DEV_GONE */
#define OUT_CANCEL_WAIT_MS 100
#define SWEEP_TAKE_MS 20       /* deferred sweep: gate wait before re-arming the timer */
#define SWEEP_RETRY_US 5000
/* Timing values (OUT timeout and stuck time, chunk size, backoff, IN window) are in
 * usbdeck_policy.h, with the measurements behind them. An OUT transfer not taken within its
 * timeout is "late"; it is never cancelled while the deck is there: IDF 5.5 can't halt a periodic
 * channel whose device keeps NAKing (its halt waits forever for a channel-halted interrupt; seen
 * on hardware). UPOL_OUT_STUCK_MS later the deck is escalated (deck_dev_escalate: rate-limited
 * bus reset, strikes, quarantine), and the reset makes the halt safe. */

/* ---- interrupt OUT / IN gate (box-wide; hwdebug-1) ----
 * IDF 5.5's DWC host driver (descriptor DMA) sometimes closes an interrupt OUT qTD without sending
 * it (rem_len = MPS, status "success") while another periodic channel is re-armed, then asserts in
 * hcd_dwc.c _buffer_parse_intr. hwdebug-1 tied it to our IN transfers being resubmitted around the
 * OUT transfer. Back then every IN completed about every 250 us with 0 bytes; that turned out to be
 * the RX FIFO at its bare minimum (130 lines: usb_host_task.c), where the core closed the IN
 * descriptor without a usable transaction, so no key data ever arrived. With the RX FIFO sized for
 * the decks' 512-byte HS IN packets, an idle IN transfer stays queued (the deck's NAKs are retried
 * inside it) and completes only with data.
 *
 * So every interrupt OUT transfer runs inside a gate (out_begin .. gate_release, one at a time
 * box-wide) that keeps IN resubmissions away from it: s_hold is set, and an IN transfer that
 * completes meanwhile parks (upol_in_next) instead of being resubmitted. A queued IN stays queued
 * through the OUT: the gate never waits for it and never halts or cancels it (IDF 5.5 can't halt a
 * periodic channel whose device keeps NAKing; its halt waits forever). A queued IN that never
 * completes is an idle deck, not a stall. The gate restarts parked IN transfers itself (in_sweep):
 * when it is released and the last restart is UPOL_IN_WINDOW_MS old, otherwise from s_sweep_timer
 * (via the usbdeck service task) once the window is over. A parked IN's data was delivered when it
 * completed; the next report waits at most ~UPOL_IN_WINDOW_MS + one OUT transfer (<=
 * UPOL_OUT_CHUNK_MAX_US of bus time while decks take data promptly). A deck that holds off OUT data
 * stretches that transfer (Module 32: ~200 ms about once per 1000 reports under continuous writes).
 * After ~1 s it is escalated; until the reset is allowed the gate stays held. What can hold it back
 * is bounded: the floor (another reset less than 10 s ago, and this one wouldn't quarantine the deck;
 * 1 s if it would), a reset still in progress, or our last power-on less than 3 s ago with nothing
 * enumerated since. So up to ~10 s.
 *
 * The hold check and the submit can't be one atomic step: a resubmit (in_done) or a start/restart
 * (submit_in) decides with s_hold clear, then calls into the HCD. Each counts itself in s_submitting
 * in the same critical section as that check, and out_begin, once it has set s_hold, waits until
 * the count is 0 before the OUT goes out (a few microseconds; upol_out_may_start). An idle queued IN
 * is never waited for. in_sweep needs no count: it submits holding the gate.
 *
 * h->in_state, s_hold, s_submitting and s_reg[] are guarded by s_lock (short sections, no calls). The gate
 * touches other decks' handles only with s_out_mtx held; hid_raw_close takes it to unregister, so a
 * handle is never freed under a gate operation. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_hold;                 /* s_lock: an OUT transfer is (about to be) on the bus */
static unsigned s_submitting;       /* s_lock: IN submits decided with s_hold clear, not yet returned
                                     * from the HCD; out_begin waits for them */
static hid_raw_t *s_reg[USBDECK_MAX]; /* s_lock: handles with a started IN pipe */
static SemaphoreHandle_t s_out_mtx; /* the gate */
static esp_timer_handle_t s_sweep_timer;
static int64_t s_last_sweep_us;     /* s_out_mtx */

static TickType_t ticks_ceil_us(int64_t us)
{
    return (TickType_t)((us * configTICK_RATE_HZ + 999999) / 1000000) + 1; /* +1: partial tick */
}

static uint8_t in_state(hid_raw_t *h)
{
    taskENTER_CRITICAL(&s_lock);
    uint8_t st = h->in_state;
    taskEXIT_CRITICAL(&s_lock);
    return st;
}

/* The IN transfer left the USB stack for good (stopped) or until the gate restarts it (parked). */
static void in_left_stack(hid_raw_t *h, uint8_t st)
{
    taskENTER_CRITICAL(&s_lock);
    h->in_state = st;
    taskEXIT_CRITICAL(&s_lock); /* last access to h: hid_raw_close may free it from here on */
}

/* Gate held, s_hold clear: resubmits every parked IN transfer of a present deck. */
static void in_sweep(void)
{
    hid_raw_t *go[USBDECK_MAX];
    int n = 0;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < USBDECK_MAX; i++) {
        hid_raw_t *h = s_reg[i];
        if (h && h->in_state == IN_PARKED && !atomic_load(&h->stopping) && !atomic_load(&h->gone)) {
            h->in_state = IN_QUEUED; /* claimed: nobody else submits it */
            go[n++] = h;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    s_last_sweep_us = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        if (usb_host_transfer_submit(go[i]->xin) == ESP_OK) continue;
        go[i]->in_cb(go[i]->in_ctx, USB_TRANSFER_STATUS_ERROR, NULL, 0); /* owner restarts it */
        in_left_stack(go[i], IN_IDLE);
    }
}

/* esp_timer task: no USB calls here (they can block); the usbdeck service task sweeps. */
static void sweep_timer_cb(void *arg)
{
    (void)arg;
    usbdeck_svc_request(USBDECK_SVC_SWEEP);
}

void hid_raw_sweep_deferred(void)
{
    if (xSemaphoreTake(s_out_mtx, pdMS_TO_TICKS(SWEEP_TAKE_MS)) != pdTRUE) {
        /* A writer (its release sweeps or re-arms) or a closing deck (doesn't) holds the gate:
         * try again shortly either way, so parked IN transfers are never forgotten. */
        esp_timer_start_once(s_sweep_timer, SWEEP_RETRY_US);
        return;
    }
    in_sweep();
    xSemaphoreGive(s_out_mtx);
}

esp_err_t hid_raw_init(void)
{
    if (!s_out_mtx) s_out_mtx = xSemaphoreCreateMutex();
    if (!s_out_mtx) return ESP_ERR_NO_MEM;
    if (!s_sweep_timer) {
        const esp_timer_create_args_t args = {.callback = sweep_timer_cb, .name = "usbdeck_in"};
        return esp_timer_create(&args, &s_sweep_timer);
    }
    return ESP_OK;
}

/* Releases the gate: parked IN transfers restart now or when the window ends. */
static void gate_release(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_hold = false;
    taskEXIT_CRITICAL(&s_lock);
    int64_t delay = 0;
    if (upol_sweep_due(esp_timer_get_time(), s_last_sweep_us, &delay))
        in_sweep();
    else /* already armed: fine, it fires within the window */
        esp_timer_start_once(s_sweep_timer, (uint64_t)delay);
    xSemaphoreGive(s_out_mtx);
}

/* An IN submit counted in s_submitting returned from the HCD. */
static void submit_done(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_submitting--;
    taskEXIT_CRITICAL(&s_lock);
}

/* Takes the gate (release it with gate_release): until then no IN transfer is resubmitted; one that
 * completes parks. Queued IN transfers stay queued and are not waited for; an IN submit already
 * on its way into the HCD is. ESP_ERR_TIMEOUT if one hasn't returned within
 * UPOL_IN_SUBMIT_WAIT_US (it never should): the gate is free again and the OUT must not be sent. */
static esp_err_t out_begin(void)
{
    xSemaphoreTake(s_out_mtx, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    s_hold = true;
    taskEXIT_CRITICAL(&s_lock);
    const int64_t deadline = esp_timer_get_time() + UPOL_IN_SUBMIT_WAIT_US;
    for (int spin = 0;; spin++) {
        unsigned queued = 0;
        taskENTER_CRITICAL(&s_lock);
        const unsigned submitting = s_submitting;
        for (int i = 0; i < USBDECK_MAX; i++)
            if (s_reg[i] && s_reg[i]->in_state == IN_QUEUED) queued++;
        taskEXIT_CRITICAL(&s_lock);
        if (upol_out_may_start(queued, submitting)) return ESP_OK;
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "an IN submit didn't return in %lld ms: OUT report not sent", UPOL_IN_SUBMIT_WAIT_US / 1000);
            gate_release();
            return ESP_ERR_TIMEOUT;
        }
        /* the submitter is on the other core (microseconds), or on this one at a lower priority */
        if (spin < 50) esp_rom_delay_us(2);
        else vTaskDelay(1);
    }
}

static esp_err_t status_err(usb_transfer_status_t st)
{
    switch (st) {
    case USB_TRANSFER_STATUS_COMPLETED: return ESP_OK;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED: return ESP_ERR_INVALID_STATE;
    case USB_TRANSFER_STATUS_TIMED_OUT: return ESP_ERR_TIMEOUT;
    default: return ESP_FAIL;
    }
}

static void delay_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

/* ---- transfer callbacks (USB client task) ---- */

static void in_done(usb_transfer_t *t)
{
    hid_raw_t *h = t->context;
    const bool completed = t->status == USB_TRANSFER_STATUS_COMPLETED;
    if (!atomic_load(&h->stopping)) {
        if (completed) h->in_cb(h->in_ctx, t->status, t->data_buffer, t->actual_num_bytes);
        else h->in_cb(h->in_ctx, t->status, NULL, 0); /* the owner restarts it */
    }
    /* Decided under the lock with s_hold, so a park can't slip past gate_release's sweep, and a
     * resubmit is counted before an OUT can begin (out_begin waits for it). */
    taskENTER_CRITICAL(&s_lock);
    const upol_in_next_t next = upol_in_next(completed, atomic_load(&h->stopping), s_hold);
    if (next == UPOL_IN_PARK) h->in_state = IN_PARKED; /* the gate restarts it after the OUT */
    if (next == UPOL_IN_RESUBMIT) s_submitting++;
    taskEXIT_CRITICAL(&s_lock);
    if (next == UPOL_IN_PARK) return;
    if (next == UPOL_IN_RESUBMIT) {
        const esp_err_t err = usb_host_transfer_submit(t);
        submit_done();
        if (err == ESP_OK) return; /* still queued */
        h->in_cb(h->in_ctx, USB_TRANSFER_STATUS_ERROR, NULL, 0);
    }
    in_left_stack(h, IN_IDLE);
}

static void out_done(usb_transfer_t *t)
{
    hid_raw_t *h = t->context;
    xSemaphoreGive(h->out_done);
}

static void ctl_done(usb_transfer_t *t)
{
    hid_raw_t *h = t->context;
    xSemaphoreGive(h->ctl_done);
}

/* ---- open / close ---- */

static bool find_hid(hid_raw_t *h, const usb_config_desc_t *cfg)
{
    int off = 0;
    const usb_standard_desc_t *p = (const usb_standard_desc_t *)cfg;
    while ((p = usb_parse_next_descriptor_of_type(p, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off))) {
        const usb_intf_desc_t *id = (const usb_intf_desc_t *)p;
        if (id->bInterfaceClass != USB_CLASS_HID || id->bAlternateSetting != 0) continue;
        uint8_t in = 0, out = 0, out_ival = 1;
        uint16_t mps_in = 0, mps_out = 0;
        for (int i = 0; i < id->bNumEndpoints; i++) {
            int eoff = off;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(id, i, cfg->wTotalLength, &eoff);
            if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_INTR || USB_EP_DESC_GET_MPS(ep) == 0)
                continue;
            if (USB_EP_DESC_GET_EP_DIR(ep)) {
                if (!in) {
                    in = ep->bEndpointAddress;
                    mps_in = USB_EP_DESC_GET_MPS(ep);
                }
            } else if (!out) {
                out = ep->bEndpointAddress;
                mps_out = USB_EP_DESC_GET_MPS(ep);
                out_ival = ep->bInterval ? ep->bInterval : 1;
            }
        }
        if (in) {
            h->intf = id->bInterfaceNumber;
            h->ep_in = in;
            h->ep_out = out;
            h->mps_in = mps_in;
            h->mps_out = mps_out;
            h->out_binterval = out_ival;
            return true;
        }
    }
    return false;
}

esp_err_t hid_raw_open(hid_raw_t *h, usb_host_client_handle_t client, usb_device_handle_t dev,
                       size_t report_len)
{
    memset(h, 0, sizeof *h); /* in_state IN_IDLE */
    atomic_init(&h->stopping, false);
    atomic_init(&h->gone, false);
    h->client = client;
    h->dev = dev;
    h->report_len = report_len;

    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    usb_device_info_t info;
    esp_err_t err = usb_host_get_device_descriptor(dev, &dd);
    if (err == ESP_OK) err = usb_host_get_active_config_descriptor(dev, &cfg);
    if (err == ESP_OK) err = usb_host_device_info(dev, &info);
    if (err != ESP_OK) return err;
    h->mps0 = dd->bMaxPacketSize0;
    h->addr = info.dev_addr;
    if (!find_hid(h, cfg)) {
        ESP_LOGE(TAG, "no HID interface with an interrupt IN endpoint");
        return ESP_ERR_NOT_FOUND;
    }
    /* OUT packet spacing (USB 2.0 table 9-13) and how many packets one gated transfer may carry. */
    h->out_interval_us = upol_interval_us(info.speed == USB_SPEED_HIGH, h->out_binterval);
    h->out_chunk_pkts = upol_chunk_pkts(h->out_interval_us);

    /* Control data: a GET_REPORT IN stage rounded up to EP0's MPS, or a whole output report
     * when the interface has no interrupt OUT endpoint. */
    size_t ctl_data = (size_t)usb_round_up_to_mps(FEATURE_GET_LEN, h->mps0);
    if (ctl_data < CTL_DATA_MIN) ctl_data = CTL_DATA_MIN;
    if (!h->ep_out && report_len > ctl_data) ctl_data = report_len;

    h->out_done = xSemaphoreCreateBinary();
    h->ctl_done = xSemaphoreCreateBinary();
    if (!h->out_done || !h->ctl_done) return ESP_ERR_NO_MEM;
    err = usb_host_transfer_alloc(h->mps_in, 0, &h->xin);
    if (err == ESP_OK && h->ep_out) err = usb_host_transfer_alloc(report_len, 0, &h->xout);
    if (err == ESP_OK) err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + ctl_data, 0, &h->xctl);
    if (err != ESP_OK) return err;

    /* IN: one MPS-sized packet per transfer (host library: IN num_bytes must be a multiple of
     * MPS); Stream Deck input data always sits in the first packet of a report. */
    h->xin->device_handle = dev;
    h->xin->bEndpointAddress = h->ep_in;
    h->xin->num_bytes = h->mps_in;
    h->xin->callback = in_done;
    h->xin->context = h;
    if (h->xout) {
        h->xout->device_handle = dev;
        h->xout->bEndpointAddress = h->ep_out;
        h->xout->callback = out_done;
        h->xout->context = h;
    }
    h->xctl->device_handle = dev;
    h->xctl->bEndpointAddress = 0;
    h->xctl->callback = ctl_done;
    h->xctl->context = h;

    err = usb_host_interface_claim(client, dev, h->intf, 0);
    if (err != ESP_OK) return err;
    h->claimed = true;
    ESP_LOGD(TAG, "intf %u: IN 0x%02x mps %u, OUT 0x%02x mps %u every %lu us", h->intf, h->ep_in, h->mps_in,
             h->ep_out, h->mps_out, (unsigned long)h->out_interval_us);
    return ESP_OK;
}

/* Off the gate's books, even if the IN transfer leaks (a present deck's queued IN isn't halted: the
 * deck's memory is kept then, hid_raw_close fails). */
static void unregister(hid_raw_t *h)
{
    if (!h->registered) return;
    xSemaphoreTake(s_out_mtx, portMAX_DELAY); /* no gate operation is using h meanwhile */
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < USBDECK_MAX; i++)
        if (s_reg[i] == h) s_reg[i] = NULL;
    taskEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_out_mtx);
    h->registered = false;
}

esp_err_t hid_raw_close(hid_raw_t *h)
{
    atomic_store(&h->stopping, true);
    if (h->claimed) {
        /* A queued IN transfer (an idle deck's stays queued): its callback sees `stopping` and won't
         * resubmit, the gate's sweep skips stopping handles (and a resubmit racing the halt fails on
         * the halted pipe). A gone deck's is given time to retire by itself, then halted (the port
         * is invalid, so the halt doesn't wait); a present deck's is never halted (that halt may
         * never return): it leaks with the interface (upol_in_close). */
        const int64_t t0 = esp_timer_get_time();
        int64_t last_halt = -1;
        while (in_state(h) == IN_QUEUED) {
            const int64_t waited = esp_timer_get_time() - t0;
            const upol_in_close_t a = upol_in_close(atomic_load(&h->gone), waited, last_halt);
            if (a == UPOL_CLOSE_GIVE_UP) break;
            if (a == UPOL_CLOSE_HALT) {
                last_halt = waited;
                usb_host_endpoint_halt(h->dev, h->ep_in);
                usb_host_endpoint_flush(h->dev, h->ep_in);
            }
            delay_ms(CLOSE_POLL_MS);
        }
    }
    unregister(h); /* before the waits below: the gate no longer waits for this handle */
    if (h->out_stuck && xSemaphoreTake(h->out_done, pdMS_TO_TICKS(2 * XFER_TIMEOUT_MS)) == pdTRUE)
        h->out_stuck = false;
    if (h->ctl_stuck && xSemaphoreTake(h->ctl_done, pdMS_TO_TICKS(2 * XFER_TIMEOUT_MS)) == pdTRUE)
        h->ctl_stuck = false;
    if (h->ctl_stuck) { /* EP0 isn't covered by the interface release check below */
        ESP_LOGE(TAG, "control transfer still in flight; leaking the interface");
        return ESP_ERR_INVALID_STATE;
    }
    /* An IN or OUT transfer we still count as in flight: ask IDF. It releases the interface only
     * when none of its endpoints holds a URB (in flight or awaiting its callback), so on success
     * nothing of ours is referenced any more, whatever our bookkeeping says. */
    const bool doubtful = in_state(h) == IN_QUEUED || h->out_stuck;
    if (h->claimed) {
        esp_err_t err = ESP_OK;
        for (int i = 0; i < RELEASE_TRIES; i++) {
            err = usb_host_interface_release(h->client, h->dev, h->intf);
            if (err != ESP_ERR_INVALID_STATE) break;
            delay_ms(CLOSE_POLL_MS); /* client task still retiring the cancelled URB */
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: %s", doubtful ? "transfers still in flight; leaking the interface"
                                             : "interface release failed", esp_err_to_name(err));
            return err;
        }
        h->claimed = false;
    } else if (doubtful) {
        ESP_LOGE(TAG, "transfers still in flight; leaking the interface");
        return ESP_ERR_INVALID_STATE;
    }
    if (doubtful) ESP_LOGW(TAG, "dev %u: a transfer counted in flight had left the USB stack", h->addr);
    usb_host_transfer_free(h->xin);
    usb_host_transfer_free(h->xout);
    usb_host_transfer_free(h->xctl);
    h->xin = h->xout = h->xctl = NULL;
    if (h->out_done) vSemaphoreDelete(h->out_done);
    if (h->ctl_done) vSemaphoreDelete(h->ctl_done);
    h->out_done = h->ctl_done = NULL;
    return ESP_OK;
}

/* ---- IN ---- */

/* The caller owns the IN transfer (IN_IDLE). */
static esp_err_t submit_in(hid_raw_t *h)
{
    taskENTER_CRITICAL(&s_lock);
    const bool hold = s_hold;
    h->in_state = hold ? IN_PARKED : IN_QUEUED;
    if (!hold) s_submitting++; /* out_begin waits for it */
    taskEXIT_CRITICAL(&s_lock);
    if (hold) return ESP_OK; /* an OUT transfer is on the bus: the gate starts it afterwards */
    esp_err_t err = usb_host_transfer_submit(h->xin);
    submit_done();
    if (err != ESP_OK) in_left_stack(h, IN_IDLE);
    return err;
}

esp_err_t hid_raw_start_in(hid_raw_t *h, hid_raw_in_cb_t cb, void *ctx)
{
    if (!h->claimed || !cb || in_state(h) != IN_IDLE) return ESP_ERR_INVALID_STATE;
    h->in_cb = cb;
    h->in_ctx = ctx;
    if (!h->registered) {
        bool ok = false;
        taskENTER_CRITICAL(&s_lock);
        for (int i = 0; i < USBDECK_MAX && !ok; i++)
            if (!s_reg[i]) {
                s_reg[i] = h;
                ok = true;
            }
        taskEXIT_CRITICAL(&s_lock);
        if (!ok) return ESP_ERR_NO_MEM;
        h->registered = true;
    }
    return submit_in(h);
}

esp_err_t hid_raw_restart_in(hid_raw_t *h)
{
    if (!h->claimed || atomic_load(&h->stopping)) return ESP_ERR_INVALID_STATE;
    /* The failing callback notifies before it leaves IN_QUEUED: give it a moment. */
    for (int i = 0; i < 10 && in_state(h) == IN_QUEUED; i++) delay_ms(CLOSE_POLL_MS);
    if (in_state(h) != IN_IDLE) return ESP_OK; /* running again, or parked (the gate restarts it) */
    usb_host_endpoint_clear(h->dev, h->ep_in); /* the pipe halts on a transfer error */
    return submit_in(h);
}

/* ---- control transfers ---- */

/* Waits for a previous timed-out transfer if its completion has arrived meanwhile. */
static bool slot_free(bool *stuck, SemaphoreHandle_t done)
{
    if (*stuck && xSemaphoreTake(done, 0) == pdTRUE) *stuck = false;
    return !*stuck;
}

/* out_data NULL: the OUT data is already in place after the setup packet. */
static esp_err_t ctl_xfer(hid_raw_t *h, uint8_t bm, uint8_t req, uint16_t value, uint16_t len,
                          const uint8_t *out_data, size_t *in_len)
{
    if (!h->claimed || !h->xctl) return ESP_ERR_INVALID_STATE;
    if (!slot_free(&h->ctl_stuck, h->ctl_done)) return ESP_ERR_INVALID_STATE;
    usb_transfer_t *t = h->xctl;
    const bool in = (bm & USB_BM_REQUEST_TYPE_DIR_IN) != 0;
    const size_t data = in ? (size_t)usb_round_up_to_mps(len, h->mps0) : len;
    if (USB_SETUP_PACKET_SIZE + data > t->data_buffer_size) return ESP_ERR_INVALID_SIZE;

    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = bm;
    s->bRequest = req;
    s->wValue = value;
    s->wIndex = h->intf;
    s->wLength = len;
    if (!in && out_data && len) memcpy(t->data_buffer + USB_SETUP_PACKET_SIZE, out_data, len);
    t->num_bytes = (int)(USB_SETUP_PACKET_SIZE + data);
    t->timeout_ms = XFER_TIMEOUT_MS;

    esp_err_t err = usb_host_transfer_submit_control(h->client, t);
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(h->ctl_done, pdMS_TO_TICKS(XFER_TIMEOUT_MS)) != pdTRUE) {
        /* Unlike interrupt endpoints, EP0 can't be halted/flushed by a client in IDF 5.5
         * (usbh.c check_ep_addr: "EP0 is owned/managed by USBH"). Leave it in flight: later
         * control requests fail fast until its completion arrives (slot_free), image writes
         * and input carry on, and a detach always retires it. */
        ESP_LOGW(TAG, "control request 0x%02x/0x%04x timed out", req, value);
        h->ctl_stuck = true;
        return ESP_ERR_TIMEOUT;
    }
    err = status_err(t->status); /* EP0 halt/flush/clear is handled by the host library */
    if (err == ESP_OK && in_len) {
        int n = t->actual_num_bytes - USB_SETUP_PACKET_SIZE;
        *in_len = n < 0 ? 0 : (size_t)n > len ? len : (size_t)n;
    }
    return err;
}

esp_err_t hid_raw_set_feature(hid_raw_t *h, const uint8_t *rep, size_t len)
{
    if (!rep || len == 0 || len > UINT16_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ctl_xfer(h, BM_CLASS_INTF_OUT, HID_SET_REPORT, (HID_RT_FEATURE << 8) | rep[0],
                             (uint16_t)len, rep, NULL);
    esp_rom_delay_us(FEATURE_GAP_US);
    return err;
}

esp_err_t hid_raw_get_feature(hid_raw_t *h, uint8_t id, uint8_t *out, size_t cap, size_t *got)
{
    size_t n = 0;
    *got = 0;
    esp_err_t err = ctl_xfer(h, BM_CLASS_INTF_IN, HID_GET_REPORT, (HID_RT_FEATURE << 8) | id,
                             FEATURE_GET_LEN, NULL, &n);
    if (err == ESP_OK) {
        if (n > cap) n = cap;
        memcpy(out, h->xctl->data_buffer + USB_SETUP_PACKET_SIZE, n);
        *got = n;
    }
    esp_rom_delay_us(FEATURE_GAP_US);
    return err;
}

/* ---- OUT ---- */

uint8_t *hid_raw_out_buf(hid_raw_t *h)
{
    if (h->xout) return h->xout->data_buffer;
    return h->xctl ? h->xctl->data_buffer + USB_SETUP_PACKET_SIZE : NULL;
}

/* A transfer that even halt + flush didn't bring back: take its late completion, re-arm the pipe. */
static bool out_ready(hid_raw_t *h)
{
    if (h->out_stuck && xSemaphoreTake(h->out_done, 0) == pdTRUE) {
        h->out_stuck = false;
        usb_host_endpoint_clear(h->dev, h->ep_out); /* halted when we tried to cancel it */
    }
    return !h->out_stuck;
}

/* Gate held: retires an OUT transfer that hasn't completed. Only call it when halting can't block:
 * the device is gone or the bus was reset (then IDF marks the pipe halted without waiting). */
static esp_err_t out_cancel(hid_raw_t *h)
{
    usb_host_endpoint_halt(h->dev, h->ep_out);
    usb_host_endpoint_flush(h->dev, h->ep_out);
    if (xSemaphoreTake(h->out_done, ticks_ceil_us(OUT_CANCEL_WAIT_MS * 1000LL)) != pdTRUE) {
        h->out_stuck = true; /* out_ready takes the late completion */
        return ESP_ERR_TIMEOUT;
    }
    usb_host_endpoint_clear(h->dev, h->ep_out);
    return ESP_ERR_TIMEOUT;
}

/* Gate held: waits for the submitted OUT transfer. Late (past timeout_ms): keep waiting, since the
 * channel can't be halted while the deck is there; h->out_late makes the caller back off. Still
 * queued UPOL_OUT_STUCK_MS later: escalate (bus reset as soon as deck_dev_escalate allows, within
 * ~10 s; until then keep waiting). Once the deck is gone (after the reset or a replug): cancel. */
static esp_err_t out_wait(hid_raw_t *h, uint32_t timeout_ms)
{
    if (xSemaphoreTake(h->out_done, ticks_ceil_us(timeout_ms * 1000LL)) == pdTRUE) return ESP_OK;
    ESP_LOGW(TAG, "dev %u: OUT 0x%02x not taken in %lu ms, waiting", h->addr, h->ep_out, (unsigned long)timeout_ms);
    h->out_late = true;
    const int64_t late_at = esp_timer_get_time(), stuck_at = late_at + UPOL_OUT_STUCK_MS * 1000LL;
    int64_t gone_by = 0; /* set once our reset has started */
    bool floor_logged = false;
    while (!atomic_load(&h->gone)) {
        if (xSemaphoreTake(h->out_done, ticks_ceil_us(gone_by ? 10000 : 100000)) == pdTRUE) {
            ESP_LOGW(TAG, "dev %u: OUT 0x%02x taken %lld ms late", h->addr, h->ep_out,
                     (esp_timer_get_time() - late_at) / 1000);
            return ESP_OK;
        }
        const int64_t now = esp_timer_get_time();
        if (now < stuck_at) continue;
        if (!gone_by) {
            if (deck_dev_escalate(h, UPOL_STRIKE_OUT)) {
                gone_by = now + OUT_GONE_WAIT_MS * 1000LL;
            } else if (!floor_logged) {
                floor_logged = true;
                ESP_LOGW(TAG, "dev %u: OUT stuck, waiting until a USB reset is allowed", h->addr);
            }
        } else if (now > gone_by) { /* port off, nothing on the bus: leave it in flight */
            h->out_stuck = true;
            return ESP_ERR_TIMEOUT;
        }
    }
    /* The deck left. Behind a hub the transfer errors out by itself (and a halt racing that error
     * would never return: IDF doesn't wake the halt on an error); on the root port nothing
     * completes it, but the port is invalid then, so the halt doesn't wait. */
    if (xSemaphoreTake(h->out_done, ticks_ceil_us(OUT_GONE_WAIT_MS * 1000LL)) == pdTRUE) return ESP_ERR_INVALID_STATE;
    return out_cancel(h);
}

/* One interrupt OUT transfer of the first n bytes of the OUT buffer (n <= out_chunk_pkts x MPS),
 * gate held. */
static esp_err_t out_xfer(hid_raw_t *h, size_t n)
{
    usb_transfer_t *t = h->xout;
    const uint32_t pkts = (uint32_t)((n + h->mps_out - 1) / h->mps_out);
    const uint32_t timeout_ms = upol_out_timeout_ms(pkts, h->out_interval_us);
    t->num_bytes = (int)n;
    t->timeout_ms = timeout_ms; /* not implemented by IDF 5.5: we time out ourselves */
    esp_err_t err = usb_host_transfer_submit(t);
    if (err != ESP_OK) return err;
    err = out_wait(h, timeout_ms);
    if (err != ESP_OK) return err;
    err = status_err(t->status);
    if (err == ESP_FAIL || err == ESP_ERR_TIMEOUT) {
        /* Transfer error: the library halted and flushed the pipe; make it usable again. */
        usb_host_endpoint_clear(h->dev, h->ep_out);
    }
    return err;
}

void hid_raw_mark_gone(hid_raw_t *h) { atomic_store(&h->gone, true); }

void hid_raw_set_out_quarantine(hid_raw_t *h, bool on) { h->out_quarantined = on; }

bool hid_raw_out_backoff(const hid_raw_t *h)
{
    return h->out_backoff_ms && esp_timer_get_time() < h->out_retry_us;
}

esp_err_t hid_raw_send_out(hid_raw_t *h)
{
    if (!h->claimed) return ESP_ERR_INVALID_STATE;
    if (!h->xout) { /* no interrupt OUT endpoint: SET_REPORT(Output) on EP0, data already in place */
        const uint8_t id = h->xctl->data_buffer[USB_SETUP_PACKET_SIZE];
        return ctl_xfer(h, BM_CLASS_INTF_OUT, HID_SET_REPORT, (HID_RT_OUTPUT << 8) | id,
                        (uint16_t)h->report_len, NULL, NULL);
    }
    if (h->out_quarantined) return ESP_ERR_NOT_ALLOWED; /* its stalls kept resetting the bus */
    if (!out_ready(h)) return ESP_ERR_INVALID_STATE;
    if (hid_raw_out_backoff(h)) return ESP_ERR_TIMEOUT; /* don't take the gate for a deck that won't listen */
    /* The whole report as consecutive transfers of whole packets (remainder in the last one), each
     * completed before the next: the same packet sequence on the wire as one transfer. Each one is
     * gated on its own, so parked IN transfers are re-armed between them (upol_sweep_due). */
    const size_t chunk_max = (size_t)h->out_chunk_pkts * h->mps_out;
    uint8_t *buf = h->xout->data_buffer;
    size_t left = h->report_len;
    esp_err_t err = ESP_OK;
    while (left > 0) {
        const size_t n = left < chunk_max ? left : chunk_max;
        err = out_begin();
        if (err != ESP_OK) break; /* gate free, nothing sent */
        err = out_xfer(h, n);
        gate_release();
        if (err != ESP_OK) break;
        left -= n;
        if (left) memmove(buf, buf + n, left); /* the next chunk starts at the buffer start */
    }
    if (err == ESP_OK && !h->out_late) {
        h->out_backoff_ms = 0;
    } else if (h->out_late) { /* back off: it may hold the gate once per period */
        h->out_late = false;
        h->out_backoff_ms = upol_backoff_next_ms(h->out_backoff_ms);
        h->out_retry_us = esp_timer_get_time() + h->out_backoff_ms * 1000LL;
    }
    return err;
}
