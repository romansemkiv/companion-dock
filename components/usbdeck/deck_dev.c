/* Per-deck task: open sequence, input events, serialised writes, detach. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usbdeck_policy.h"
#include "usbdeck_priv.h"

#define TAG USBDECK_TAG

#define DECK_STACK 4096
#define DECK_PRIO 6
#define CMD_QUEUE_DEPTH 48
#define IN_QUEUE_DEPTH 8
#define IN_REPORT_MAX 64 /* every model's input data fits in the first 64 bytes */
#define IN_REPORT_ID 0x01
#define EVENTS_PER_PARSE 16
#define PARSE_PASSES 4 /* 4 x 16 >= 40 keys */
#define FEATURE_MAX USBDECK_FEATURE_MAX
#define SERIAL_TRIES 3
#define SERIAL_RETRY_MS 20
#define IN_RESTART_DELAY_MS 50  /* first restarts: quick (a detach usually lands first) */
#define IN_RESTART_MAX 5        /* failed restarts before the problem is reported */
#define IN_RETRY_SLOW_MS 1000   /* then keep retrying at this pace, forever */
#define IN_FAIL_WINDOW_MS 10000 /* a failure this long after the previous one starts a new count */
#define PROBLEM_MAX USBDECK_PROBLEM_MAX
#define WRITE_FAILS_PROBLEM 3   /* failed image writes in a row before the problem is reported */

typedef enum { CMD_KEY, CMD_LCD, CMD_INFO, CMD_FEATURE, CMD_RAW, CMD_FEATURE_GET } cmd_kind_t;

/* A usbdeck_feature_get request. Two references while queued (the caller and the queue), so the
 * caller may give up waiting: the last one to let go frees it. */
typedef struct {
    atomic_int refs;
    SemaphoreHandle_t done; /* given once the request has run or was dropped */
    uint8_t id;
    esp_err_t err;
    size_t len;
    uint8_t data[FEATURE_MAX];
} feat_get_t;

typedef struct {
    uint8_t kind;
    union {
        struct {
            uint8_t *data; /* heap copy, freed by the deck task */
            size_t len;
            uint16_t x, y, w, h;
            uint8_t hw;
        } img;
        struct {
            uint8_t len;
            uint8_t buf[FEATURE_MAX];
        } feat;
        feat_get_t *get;
    };
} deck_cmd_t;

typedef struct {
    uint16_t len;
    uint8_t data[IN_REPORT_MAX];
} in_report_t;

struct usbdeck {
    const deck_model_t *model;
    deck_link_t link;
    usbdeck_info_t info; /* written by the deck task before `attached`, then read-only */
    atomic_bool gone;    /* set under s.mtx */
    bool attached;       /* s.mtx */
    bool ids_read;       /* s.mtx: key is final */
    char key[UPOL_KEY_MAX]; /* record key: serial if one was read, else VID/PID + port path */
    /* upol_quarantine_t: stored by the deck task at open (before on_attach) and when its lift
     * clears it (and by deck_dev_escalate if its reset didn't start); writers on other tasks read
     * it (post_image). */
    _Atomic uint8_t quarantine;
    int64_t lift_at_us;  /* deck task: when to ask whether the quarantine may be lifted (0: none) */
    TaskHandle_t task;
    QueueHandle_t cmdq, inq;
    bool cmdq_caps, inq_caps; /* made with xQueueCreateWithCaps (PSRAM): deleted with vQueueDeleteWithCaps */
    /* Commands queued or being run (usbdeck_writes_drained): counted up before a command goes into
     * cmdq (down again if it doesn't) and down once the deck task has finished it, so 0 means every
     * write handed over so far is on the deck (hid_raw_send_out returns once its transfer is done). */
    atomic_uint pending;
    atomic_bool in_err; /* IN pipe stopped on an error (set by the IN callback) */
    /* input reports dropped (inq full) and not logged yet: counted by the IN callback, logged by the deck
     * task (drain_input), the first at once and then at most once a second (in_drop_log_at, in_drop_logged). */
    atomic_uint in_dropped;
    TickType_t in_drop_log_at;
    bool in_drop_logged;
    /* Input recovery, deck task only. */
    bool in_retry;           /* a restart is scheduled at in_retry_at */
    TickType_t in_retry_at, in_last_fail;
    uint8_t in_fails;        /* consecutive failures (reset by a report or a quiet window) */
    atomic_bool in_problem_sent; /* written by the deck task, read by usbdeck_trouble */
    atomic_uint in_fail_s;       /* uptime in s at the last input failure (usbdeck_trouble) */
    /* Output health, deck task only: image writes failed in a row, problem reported. */
    uint8_t write_fails;
    atomic_bool write_problem_sent; /* as in_problem_sent */
    atomic_uint write_fail_s;       /* as in_fail_s, for image writes */
    hid_raw_t hid;
    deck_input_state_t in_state;
    /* The feature reports read at open (deckproto_kept_report_ids), written by the deck task before
     * on_attach and read-only after: usbdeck_feature_get answers from here and never asks the deck
     * again. len 0: not read. */
    struct {
        uint8_t id, len;
        uint8_t data[FEATURE_MAX];
    } kept[DECK_KEPT_REPORTS_MAX];
    /* usbdeck_set_raw_input (raw_mux): input reports go here instead of on_input */
    portMUX_TYPE raw_mux;
    usbdeck_raw_input_fn raw_fn;
    void *raw_ctx;
    atomic_bool raw_on; /* raw_fn is set: usbdeck_passthrough reads it without the lock */
};

static struct {
    SemaphoreHandle_t mtx;
    usbdeck_cb_t cb;
    usbdeck_t *decks[USBDECK_MAX]; /* mtx */
    atomic_uint trouble_reports;   /* usbdeck_trouble: deck trouble reported since the start */
} s;

/* Bus reset floor and per-deck records (mtx). They survive detach, and a software restart too
 * (no-init RAM, checked by magic and sum; the magic encodes the record's layout, so an update that
 * changes it starts afresh): the restart that a USB that won't come back triggers must not let a
 * bad deck start over. */
#define POL_MAGIC (0x55504f4cu ^ ((uint32_t)UPOL_LAYOUT_VERSION << 24) ^ (uint32_t)sizeof(upol_t))
static __NOINIT_ATTR struct {
    uint32_t magic, sum;
    upol_t pol;
} s_keep;
#define POL (&s_keep.pol)

static void pol_saved(void) { s_keep.sum = upol_sum(POL); } /* after every change, under mtx */

/* ---- helpers ---- */

static uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

void usbdeck_problem(const char *fmt, ...)
{
    char msg[PROBLEM_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", msg);
    if (s.cb.on_problem) s.cb.on_problem(msg);
}

/* Companion DEVICEID material: keep [A-Za-z0-9_-] only. */
static void sanitize_serial(const char *in, char *out, size_t cap, uint8_t addr)
{
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        char c = *in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
            c == '-')
            out[n++] = c;
    }
    out[n] = 0;
    if (n == 0) snprintf(out, cap, "unknown-%u", addr);
}

static void sanitize_printable(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 1 < cap; in++)
        if (*in >= 0x20 && *in < 0x7F) out[n++] = *in;
    out[n] = 0;
}

static bool is_gone(usbdeck_t *d) { return atomic_load(&d->gone); }

/* The same text when the reset quarantines the deck and right after on_attach at the re-attach
 * (surfaces keeps one per deck, dropped at its on_attach, on_detach and on_restored): what is off,
 * and in how many minutes (left_us) it is retried. */
static void quarantine_problem(const char *name, upol_quarantine_t q, int64_t left_us)
{
    atomic_fetch_add(&s.trouble_reports, 1);
    char msg[PROBLEM_MAX];
    upol_quarantine_msg(msg, sizeof msg, name, q, left_us);
    usbdeck_problem("%s", msg);
}

static void get_put(feat_get_t *g)
{
    if (atomic_fetch_sub(&g->refs, 1) == 1) {
        vSemaphoreDelete(g->done);
        free(g);
    }
}

/* The deck task's side of a request: result in, waiter woken, its reference dropped. */
static void get_finish(feat_get_t *g, esp_err_t err)
{
    g->err = err;
    xSemaphoreGive(g->done);
    get_put(g);
}

/* A command that won't run (or has run): frees what it owns. */
static void free_cmd(deck_cmd_t *c)
{
    if (c->kind == CMD_KEY || c->kind == CMD_LCD || c->kind == CMD_INFO || c->kind == CMD_RAW)
        heap_caps_free(c->img.data);
    else if (c->kind == CMD_FEATURE_GET)
        get_finish(c->get, ESP_ERR_INVALID_STATE);
}

/* A queued command taken off the queue without running it (teardown). */
static void drop_cmd(usbdeck_t *d, deck_cmd_t *c)
{
    free_cmd(c);
    atomic_fetch_sub(&d->pending, 1);
}

/* ---- input (IN callback runs on the USB client task) ---- */

static void on_in_report(void *ctx, usb_transfer_status_t st, const uint8_t *data, int len)
{
    usbdeck_t *d = ctx;
    if (st == USB_TRANSFER_STATUS_COMPLETED) {
        /* Reports longer than the IN MPS (gen2 at full speed) arrive as several packets: only
         * the first one starts with the report ID and carries input data. */
        if (len <= 0 || data[0] != IN_REPORT_ID) return;
        in_report_t r = {.len = (uint16_t)(len > IN_REPORT_MAX ? IN_REPORT_MAX : len)};
        memcpy(r.data, data, r.len);
        if (xQueueSend(d->inq, &r, 0) != pdTRUE) atomic_fetch_add(&d->in_dropped, 1); /* logged by the deck task */
    } else {
        atomic_store(&d->in_err, true);
    }
    xTaskNotifyGive(d->task);
}

/* One debug line per input event (CONFIG_LOG_MAXIMUM_LEVEL >= DEBUG and the usbdeck tag at DEBUG). */
static void log_input(const usbdeck_t *d, const deck_event_t *e)
{
    switch (e->kind) {
    case DECK_EV_KEY:
    case DECK_EV_ENC_PRESS:
        ESP_LOGD(TAG, "deck %u: %s %u %s", d->link.addr, e->kind == DECK_EV_KEY ? "key" : "encoder", e->index,
                 e->pressed ? "down" : "up");
        break;
    case DECK_EV_ENC_ROTATE:
        ESP_LOGD(TAG, "deck %u: encoder %u turned %d", d->link.addr, e->index, e->delta);
        break;
    default:
        ESP_LOGD(TAG, "deck %u: touch at %u,%u", d->link.addr, e->x, e->y);
        break;
    }
}

static void deliver_input(usbdeck_t *d, const in_report_t *r)
{
    deck_event_t ev[EVENTS_PER_PARSE];
    d->in_fails = 0; /* input works */
    d->in_problem_sent = false;
    portENTER_CRITICAL(&d->raw_mux);
    const usbdeck_raw_input_fn raw = d->raw_fn;
    void *const raw_ctx = d->raw_ctx;
    portEXIT_CRITICAL(&d->raw_mux);
    /* Passthrough: the report as it came, first. It is parsed for on_input after that all the same
     * (the box's key click, components/feedback), which also keeps the parser's key state current
     * for when the passthrough ends. */
    if (raw) raw(raw_ctx, r->data, r->len);
    /* Edges that don't fit are re-reported by the next call with the same report. */
    for (int pass = 0; pass < PARSE_PASSES; pass++) {
        int n = deckproto_parse_input(d->model, &d->in_state, r->data, r->len, ev, EVENTS_PER_PARSE);
        for (int i = 0; i < n; i++) {
            log_input(d, &ev[i]);
            if (s.cb.on_input) s.cb.on_input(d, &ev[i]);
        }
        if (n < EVENTS_PER_PARSE) break;
    }
}

/* Delivers queued input reports; true if there were any. */
static bool drain_input(usbdeck_t *d)
{
    bool any = false;
    in_report_t r;
    while (xQueueReceive(d->inq, &r, 0) == pdTRUE) {
        deliver_input(d, &r);
        any = true;
    }
    /* the reports the IN callback couldn't queue (it logs nothing on the USB client task): the first at once,
     * then how many at most once a second, after the input above is out */
    if (atomic_load(&d->in_dropped)) {
        const TickType_t now = xTaskGetTickCount();
        if (!d->in_drop_logged || now - d->in_drop_log_at >= pdMS_TO_TICKS(1000)) {
            const unsigned n = atomic_exchange(&d->in_dropped, 0);
            ESP_LOGW(TAG, "deck %u: %u input report%s dropped", d->link.addr, n, n == 1 ? "" : "s");
            d->in_drop_log_at = now;
            d->in_drop_logged = true;
        }
    }
    return any;
}

/* The IN pipe stopped (or a restart couldn't be submitted): schedule the next restart. Quick
 * retries first; after IN_RESTART_MAX failed restarts report once and retry every second. */
static void input_failed(usbdeck_t *d)
{
    const TickType_t now = xTaskGetTickCount();
    if (d->in_fails && now - d->in_last_fail > pdMS_TO_TICKS(IN_FAIL_WINDOW_MS)) {
        d->in_fails = 0; /* isolated glitch long after the last one */
        d->in_problem_sent = false;
    }
    d->in_last_fail = now;
    atomic_store(&d->in_fail_s, uptime_s());
    if (d->in_fails < UINT8_MAX) d->in_fails++;
    uint32_t delay_ms = IN_RESTART_DELAY_MS;
    if (d->in_fails > IN_RESTART_MAX) {
        delay_ms = IN_RETRY_SLOW_MS;
        if (!d->in_problem_sent) {
            d->in_problem_sent = true;
            atomic_fetch_add(&s.trouble_reports, 1);
            usbdeck_problem("Stream Deck %s stopped responding — unplug and replug it if it doesn't recover",
                            d->info.serial);
        }
    }
    if (!d->in_retry) {
        d->in_retry = true;
        d->in_retry_at = now + pdMS_TO_TICKS(delay_ms);
    }
}

static bool input_retry_due(const usbdeck_t *d)
{
    return d->in_retry && (int32_t)(xTaskGetTickCount() - d->in_retry_at) >= 0;
}

static void restart_input(usbdeck_t *d)
{
    d->in_retry = false;
    if (is_gone(d)) return;
    esp_err_t err = hid_raw_restart_in(&d->hid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deck %u: input restart failed: %s", d->link.addr, esp_err_to_name(err));
        input_failed(d);
    }
}

/* ---- output ---- */

/* One log line per failure streak, one problem report per streak that lasts. */
static void write_result(usbdeck_t *d, esp_err_t err)
{
    if (err == ESP_ERR_NOT_ALLOWED) return; /* quarantined, or another deck's reset pending */
    if (err == ESP_OK) {
        if (d->write_fails) ESP_LOGI(TAG, "deck %u: image writes work again", d->link.addr);
        d->write_fails = 0;
        d->write_problem_sent = false;
        return;
    }
    if (d->write_fails < UINT8_MAX) d->write_fails++;
    atomic_store(&d->write_fail_s, uptime_s());
    if (d->write_fails == 1) ESP_LOGW(TAG, "deck %u: image write failed: %s", d->link.addr, esp_err_to_name(err));
    if (d->write_fails >= WRITE_FAILS_PROBLEM && !d->write_problem_sent) {
        d->write_problem_sent = true;
        atomic_fetch_add(&s.trouble_reports, 1);
        usbdeck_problem("Stream Deck %s isn't taking images — unplug and replug it if it doesn't recover",
                        d->info.serial);
    }
}

/* The copy of report `id` read at open: ESP_ERR_NOT_FOUND without one. */
static esp_err_t kept_copy(const usbdeck_t *d, uint8_t id, uint8_t *buf, size_t cap, size_t *len)
{
    *len = 0;
    for (int i = 0; i < DECK_KEPT_REPORTS_MAX; i++) {
        if (!d->kept[i].len || d->kept[i].id != id) continue;
        *len = d->kept[i].len < cap ? d->kept[i].len : cap;
        memcpy(buf, d->kept[i].data, *len);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* One raw output report (usbdeck_write_raw), padded or cut to the model's report length. */
static void exec_raw(usbdeck_t *d, deck_cmd_t *c)
{
    const size_t rl = d->model->report_len;
    uint8_t *rep = hid_raw_out_buf(&d->hid);
    if (atomic_load(&d->quarantine) != UPOL_Q_NONE || hid_raw_out_backoff(&d->hid) || !rep || is_gone(d)) {
        free_cmd(c); /* as an image: quarantined, or it didn't take the last report */
        return;
    }
    const size_t n = c->img.len < rl ? c->img.len : rl;
    memcpy(rep, c->img.data, n);
    memset(rep + n, 0, rl - n);
    free_cmd(c);
    const esp_err_t err = hid_raw_send_out(&d->hid);
    if (!is_gone(d)) write_result(d, err);
}

static void exec_cmd(usbdeck_t *d, deck_cmd_t *c)
{
    const deck_model_t *m = d->model;
    if (c->kind == CMD_FEATURE) {
        esp_err_t err = hid_raw_set_feature(&d->hid, c->feat.buf, c->feat.len);
        if (err != ESP_OK && !is_gone(d))
            ESP_LOGW(TAG, "deck %u: feature report 0x%02x failed: %s", d->link.addr, c->feat.buf[0],
                     esp_err_to_name(err));
        return;
    }
    if (c->kind == CMD_FEATURE_GET) { /* from the copies only: never a control request after open */
        feat_get_t *g = c->get;
        get_finish(g, kept_copy(d, g->id, g->data, sizeof g->data, &g->len));
        return;
    }
    if (c->kind == CMD_RAW) {
        exec_raw(d, c);
        return;
    }
    deck_img_writer_t w;
    if (c->kind == CMD_KEY)
        deckproto_img_key(&w, m, c->img.hw, c->img.data, c->img.len);
    else if (c->kind == CMD_LCD)
        deckproto_img_lcd(&w, m, c->img.x, c->img.y, c->img.w, c->img.h, c->img.data, c->img.len);
    else
        deckproto_img_info(&w, m, c->img.data, c->img.len);
    if (atomic_load(&d->quarantine) != UPOL_Q_NONE || hid_raw_out_backoff(&d->hid)) {
        /* Quarantined (its stalls kept resetting the bus; post_image doesn't queue for it), or it
         * didn't take the last report: drop images (for a while). */
        free_cmd(c);
        return;
    }
    uint8_t *rep = hid_raw_out_buf(&d->hid);
    esp_err_t err = ESP_OK;
    while (rep && !is_gone(d) && deckproto_img_next(&w, rep)) {
        err = hid_raw_send_out(&d->hid);
        if (err != ESP_OK) break;
        drain_input(d); /* key presses don't wait for the rest of the image */
    }
    free_cmd(c);
    if (!is_gone(d)) write_result(d, err);
}

/* ---- open / run / teardown (deck task) ---- */

/* Deck task, at open: keeps (or replaces) the copy of report `id`. */
static void keep_report(usbdeck_t *d, uint8_t id, const uint8_t *rep, size_t len)
{
    int at = -1;
    for (int i = 0; i < DECK_KEPT_REPORTS_MAX && at < 0; i++)
        if (d->kept[i].len == 0 || d->kept[i].id == id) at = i;
    if (at < 0 || len == 0) return;
    d->kept[at].id = id;
    d->kept[at].len = (uint8_t)(len < FEATURE_MAX ? len : FEATURE_MAX);
    memcpy(d->kept[at].data, rep, d->kept[at].len);
}

static bool is_kept(const usbdeck_t *d, uint8_t id)
{
    for (int i = 0; i < DECK_KEPT_REPORTS_MAX; i++)
        if (d->kept[i].len && d->kept[i].id == id) return true;
    return false;
}

/* Reads report `id` at open and keeps it. */
static bool read_kept(usbdeck_t *d, uint8_t id, uint8_t *rep, size_t cap, size_t *got)
{
    if (hid_raw_get_feature(&d->hid, id, rep, cap, got) != ESP_OK || *got == 0) return false;
    keep_report(d, id, rep, *got);
    return true;
}

/* Returns true if a serial number was read (info.serial is then not the "unknown-<addr>" stand-in). */
static bool read_ids(usbdeck_t *d)
{
    const deck_model_t *m = d->model;
    uint8_t rep[FEATURE_MAX];
    char raw[sizeof d->info.serial + 1] = "";
    size_t got = 0;

    for (int i = 0; i < SERIAL_TRIES && !is_gone(d); i++) {
        if (read_kept(d, deckproto_serial_report_id(m), rep, sizeof rep, &got) &&
            deckproto_parse_serial(m, rep, got, raw, sizeof raw))
            break;
        raw[0] = 0;
        vTaskDelay(pdMS_TO_TICKS(SERIAL_RETRY_MS));
    }
    sanitize_serial(raw, d->info.serial, sizeof d->info.serial, d->link.addr);
    const bool serial_ok = raw[0] != 0 && strncmp(d->info.serial, "unknown-", 8) != 0;

    raw[0] = 0;
    if (!is_gone(d) &&
        (!read_kept(d, deckproto_fw_report_id(m), rep, sizeof rep, &got) ||
         !deckproto_parse_fw(m, rep, got, raw, sizeof raw)))
        raw[0] = 0;
    sanitize_printable(raw, d->info.fw, sizeof d->info.fw);

    /* Every other report a host reads (Network Dock passthrough answers only from these copies). */
    uint8_t ids[DECK_KEPT_REPORTS_MAX];
    const size_t n = deckproto_kept_report_ids(m, ids);
    for (size_t i = 0; i < n && !is_gone(d); i++)
        if (!is_kept(d, ids[i]) && !read_kept(d, ids[i], rep, sizeof rep, &got))
            ESP_LOGW(TAG, "deck %u: feature report 0x%02x unreadable at open", d->link.addr, ids[i]);
    return serial_ok;
}

/* Flushes a half-written image. After a re-attach, or a quarantine's lift, this is the deck's first
 * OUT report: the first test of its probation. */
static void send_flush(usbdeck_t *d)
{
    uint8_t *rep = hid_raw_out_buf(&d->hid);
    if (d->model->fmt == DECK_IMG_NONE || !rep || is_gone(d)) return;
    memset(rep, 0, d->model->report_len);
    rep[0] = 0x02;
    const esp_err_t err = hid_raw_send_out(&d->hid);
    if (err != ESP_OK && !is_gone(d))
        ESP_LOGW(TAG, "deck %u: flush report failed: %s", d->link.addr, esp_err_to_name(err));
}

static bool deck_open(usbdeck_t *d)
{
    const deck_model_t *m = d->model;
    if (is_gone(d)) return false;
    esp_err_t err = hid_raw_open(&d->hid, usbdeck_client(), d->link.dev, m->report_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "deck %u: HID open failed: %s", d->link.addr, esp_err_to_name(err));
        return false;
    }
    const bool serial_ok = read_ids(d);
    if (is_gone(d)) return false;
    char key[UPOL_KEY_MAX];
    upol_key(key, sizeof key, serial_ok ? d->info.serial : NULL, DECK_VID_ELGATO, m->pid, d->link.path);
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    memcpy(d->key, key, sizeof d->key);
    d->ids_read = true;
    const upol_quarantine_t q = upol_note_attach(POL, d->key, now);
    const int64_t end = upol_quarantine_end_us(POL, d->key);
    pol_saved();
    xSemaphoreGive(s.mtx);
    atomic_store(&d->quarantine, (uint8_t)q);
    if (q != UPOL_Q_NONE) { /* the problem text follows on_attach (deck_task) */
        ESP_LOGW(TAG, "deck %u (%s): quarantined, %s, retried in %lld s", d->link.addr, d->key,
                 q == UPOL_Q_ALL ? "no images, no input" : "no images",
                 (long long)(end > now ? (end - now) / 1000000 : 0));
        hid_raw_set_out_quarantine(&d->hid, true);
        d->lift_at_us = end; /* the deck task lifts it then (if that is past: as soon as allowed) */
    } else {
        send_flush(d);
    }
    if (is_gone(d)) return false;

    if (q == UPOL_Q_ALL) return true; /* an IN strike (no longer raised): leave it off */
    err = hid_raw_start_in(&d->hid, on_in_report, d);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "deck %u: IN transfer failed: %s", d->link.addr, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* ---- quarantine lift (deck task) ---- */

static bool lift_due(const usbdeck_t *d) { return d->lift_at_us && esp_timer_get_time() >= d->lift_at_us; }

/* The quarantine has run its period: retry the deck in place, without a bus reset. The reset that
 * quarantined it power-cycled it, and since that re-attach it has used EP0 only, so the first OUT
 * report after the lift (the flush) tests it just as the probation after a re-attach does; a reset
 * would drop every deck on the port for nothing. The policy lets one deck retry at a time. A stall
 * during the probation quarantines it again (deck_dev_escalate), twice as long. */
static void lift_quarantine(usbdeck_t *d)
{
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    const bool lifted = upol_note_lift(POL, d->key, now);
    const int64_t end = upol_quarantine_end_us(POL, d->key);
    pol_saved();
    xSemaphoreGive(s.mtx);
    if (!lifted) { /* not yet, or another deck's retry is running: ask again */
        d->lift_at_us = end > now ? end : now + UPOL_LIFT_RETRY_US;
        return;
    }
    d->lift_at_us = 0;
    hid_raw_set_out_quarantine(&d->hid, false);
    const uint8_t was = atomic_exchange(&d->quarantine, (uint8_t)UPOL_Q_NONE); /* writers queue again */
    ESP_LOGI(TAG, "deck %u (%s): quarantine lifted, retrying %s on probation", d->link.addr, d->key,
             was == UPOL_Q_ALL ? "images and input" : "images");
    /* Before the flush: if that stalls, the deck is quarantined again, and the new problem text
     * must come after the old ones are dropped. The redraw it asks for queues behind the flush. */
    if (s.cb.on_restored && !is_gone(d)) s.cb.on_restored(d);
    send_flush(d);
    if (was == UPOL_Q_ALL && !is_gone(d)) {
        const esp_err_t err = hid_raw_start_in(&d->hid, on_in_report, d);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "deck %u: IN transfer failed: %s", d->link.addr, esp_err_to_name(err));
            if (err != ESP_ERR_NO_MEM) input_failed(d); /* registered: restarted like a stopped pipe */
        }
    }
}

/* A quarantine stored without a lift scheduled (deck_dev_escalate, when its reset didn't start):
 * no OUT from now on, and the lift at the record's end (at once if the record doesn't hold it). */
static void schedule_lift(usbdeck_t *d)
{
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    const int64_t end = upol_quarantine_end_us(POL, d->key);
    xSemaphoreGive(s.mtx);
    hid_raw_set_out_quarantine(&d->hid, true);
    d->lift_at_us = end ? end : esp_timer_get_time();
    ESP_LOGW(TAG, "deck %u (%s): quarantined without a reset", d->link.addr, d->key);
}

/* Sleep until notified, or until a scheduled input restart or quarantine lift is due. */
static TickType_t run_wait(const usbdeck_t *d)
{
    TickType_t wait = portMAX_DELAY;
    if (d->in_retry) {
        const int32_t left = (int32_t)(d->in_retry_at - xTaskGetTickCount());
        wait = left > 0 ? (TickType_t)left : 0;
    }
    if (d->lift_at_us) { /* at most UPOL_QUARANTINE_MAX_US away: fits the tick count */
        const int64_t left_us = d->lift_at_us - esp_timer_get_time();
        const TickType_t t = left_us > 0 ? pdMS_TO_TICKS(left_us / 1000) + 1 : 0;
        if (t < wait) wait = t;
    }
    return wait;
}

/* Until the deck is gone. */
static void deck_run(usbdeck_t *d)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, run_wait(d));
        for (;;) {
            if (is_gone(d)) return;
            bool work = drain_input(d);
            if (atomic_exchange(&d->in_err, false)) input_failed(d);
            if (input_retry_due(d)) { /* writes keep flowing while a restart waits */
                restart_input(d);
                work = true;
            }
            if (!d->lift_at_us && atomic_load(&d->quarantine) != UPOL_Q_NONE) schedule_lift(d);
            if (lift_due(d)) {
                lift_quarantine(d);
                work = true;
            }
            deck_cmd_t c;
            if (xQueueReceive(d->cmdq, &c, 0) == pdTRUE) { /* one command, then input again */
                exec_cmd(d, &c);
                atomic_fetch_sub(&d->pending, 1); /* after it ran: its reports are on the deck */
                work = true;
            }
            if (!work) break;
        }
    }
}

static void registry_remove(usbdeck_t *d)
{
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    atomic_store(&d->gone, true); /* writers check this under the same lock */
    for (int i = 0; i < USBDECK_MAX; i++)
        if (s.decks[i] == d) s.decks[i] = NULL;
    xSemaphoreGive(s.mtx);
}

static void free_deck(usbdeck_t *d)
{
    if (d->cmdq) d->cmdq_caps ? vQueueDeleteWithCaps(d->cmdq) : vQueueDelete(d->cmdq); /* as it was made */
    if (d->inq) d->inq_caps ? vQueueDeleteWithCaps(d->inq) : vQueueDelete(d->inq);
    free(d);
}

static void deck_teardown(usbdeck_t *d)
{
    registry_remove(d);
    deck_cmd_t c;
    while (xQueueReceive(d->cmdq, &c, 0) == pdTRUE) drop_cmd(d, &c);

    const bool released = hid_raw_close(&d->hid) == ESP_OK;
    if (released) {
        esp_err_t err = usb_host_device_close(usbdeck_client(), d->link.dev);
        if (err != ESP_OK) ESP_LOGE(TAG, "deck %u: device close failed: %s", d->link.addr, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "deck %u (%s) detached", d->link.addr, d->info.serial);
    if (d->attached && s.cb.on_detach) s.cb.on_detach(d);
    /* A raw-passthrough caller blocked on queue space may have queued once the drain above made
     * room; on_detach has waited for it to leave, so this drain is the last. */
    while (xQueueReceive(d->cmdq, &c, 0) == pdTRUE) drop_cmd(d, &c);
    /* If the USB stack still holds a transfer of ours, keep the memory it points into. */
    if (released) free_deck(d);
}

static void deck_task(void *arg)
{
    usbdeck_t *d = arg;
    if (deck_open(d)) {
        xSemaphoreTake(s.mtx, portMAX_DELAY);
        d->attached = !is_gone(d);
        xSemaphoreGive(s.mtx);
        if (d->attached) {
            ESP_LOGI(TAG, "deck %u: %s serial=%s fw=%s %s-speed hub_depth=%u port=%u", d->link.addr,
                     d->info.model, d->info.serial, d->info.fw, d->info.high_speed ? "high" : "full",
                     d->info.hub_depth, d->info.port);
            if (s.cb.on_attach) s.cb.on_attach(d, d->model, d->info.serial);
            const upol_quarantine_t q = (upol_quarantine_t)atomic_load(&d->quarantine);
            if (q != UPOL_Q_NONE) /* after on_attach, which drops the deck's older text */
                quarantine_problem(d->info.serial, q, d->lift_at_us - esp_timer_get_time());
            deck_run(d);
        }
    }
    deck_teardown(d);
    vTaskDelete(NULL);
}

/* ---- internal API ---- */

esp_err_t deck_dev_init(const usbdeck_cb_t *cb)
{
    if (!s.mtx) s.mtx = xSemaphoreCreateMutex();
    if (!s.mtx) return ESP_ERR_NO_MEM;
    s.cb = *cb;
    if (s_keep.magic == POL_MAGIC && s_keep.sum == upol_sum(POL)) {
        upol_after_restart(POL);
        int decks, resets, quarantined;
        upol_summary(POL, &decks, &resets, &quarantined);
        ESP_LOGI(TAG, "kept the USB deck record across the restart: %d deck(s), %d reset(s), %d quarantined",
                 decks, resets, quarantined);
    } else {
        upol_init(POL);
        s_keep.magic = POL_MAGIC;
    }
    pol_saved();
    return hid_raw_init();
}

bool deck_dev_escalate(hid_raw_t *h, int kind)
{
    usbdeck_t *d = (usbdeck_t *)((char *)h - offsetof(usbdeck_t, hid)); /* h is always a deck's */
    char key[UPOL_KEY_MAX], name[sizeof d->info.serial];
    upol_quarantine_t quarantine;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    snprintf(key, sizeof key, "%s", d->ids_read ? d->key : "");
    snprintf(name, sizeof name, "%s", d->ids_read ? d->info.serial : "?");
    /* resetting first: a power-on is stamped before resetting clears, so settled then sees it */
    if (!upol_reset_allowed(POL, key, now) || usbdeck_bus_resetting() || !usbdeck_root_settled()) {
        xSemaphoreGive(s.mtx);
        return false;
    }
    quarantine = upol_note_reset(POL, key, (upol_strike_t)kind, now);
    const int64_t end = upol_quarantine_end_us(POL, key);
    pol_saved();
    xSemaphoreGive(s.mtx);
    atomic_fetch_add(&s.trouble_reports, 1);
    usbdeck_problem("Stream Deck %s %s — resetting the USB port", name,
                    kind == UPOL_STRIKE_IN ? "stopped answering" : "stopped taking data");
    if (quarantine != UPOL_Q_NONE) quarantine_problem(name, quarantine, end - now);
    if (usbdeck_bus_reset(d->link.dev)) return true;
    /* Not reset after all. Can't happen while every escalation holds the gate and checks
     * usbdeck_bus_resetting above, but if it did, the record says quarantined and the deck must be
     * too, or its task would never lift it and images would keep flowing: images off (its IN can't
     * be stopped safely), and the deck task schedules the lift (deck_run). */
    if (quarantine != UPOL_Q_NONE) {
        atomic_store(&d->quarantine, (uint8_t)UPOL_Q_WRITES);
        xTaskNotifyGive(d->task); /* d can't be freed meanwhile: its close waits for the gate */
    }
    return false;
}

esp_err_t deck_dev_spawn(const deck_link_t *link, const deck_model_t *m)
{
    usbdeck_t *d = calloc(1, sizeof *d);
    if (!d) return ESP_ERR_NO_MEM;
    d->model = m;
    d->link = *link;
    atomic_init(&d->gone, false);
    atomic_init(&d->in_err, false);
    atomic_init(&d->in_dropped, 0);
    atomic_init(&d->quarantine, (uint8_t)UPOL_Q_NONE);
    atomic_init(&d->in_problem_sent, false);
    atomic_init(&d->write_problem_sent, false);
    atomic_init(&d->in_fail_s, 0);
    atomic_init(&d->write_fail_s, 0);
    atomic_init(&d->raw_on, false);
    atomic_init(&d->pending, 0);
    portMUX_INITIALIZE(&d->raw_mux);
    snprintf(d->info.model, sizeof d->info.model, "%s", m->name);
    d->info.high_speed = link->high_speed;
    d->info.hub_depth = link->hub_depth;
    d->info.port = link->port;
    /* both queues' storage in PSRAM (internal when PSRAM is short): task context only (the deck task, the USB
     * client task's IN callback, writers), never an ISR or DMA; a report is copied in and out of them */
    d->cmdq = xQueueCreateWithCaps(CMD_QUEUE_DEPTH, sizeof(deck_cmd_t), MALLOC_CAP_SPIRAM);
    d->cmdq_caps = d->cmdq != NULL;
    if (!d->cmdq) d->cmdq = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(deck_cmd_t));
    d->inq = xQueueCreateWithCaps(IN_QUEUE_DEPTH, sizeof(in_report_t), MALLOC_CAP_SPIRAM);
    d->inq_caps = d->inq != NULL;
    if (!d->inq) d->inq = xQueueCreate(IN_QUEUE_DEPTH, sizeof(in_report_t));
    if (!d->cmdq || !d->inq) {
        free_deck(d);
        return ESP_ERR_NO_MEM;
    }

    int slot = -1;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    for (int i = 0; i < USBDECK_MAX && slot < 0; i++)
        if (!s.decks[i]) slot = i;
    if (slot >= 0) s.decks[slot] = d;
    xSemaphoreGive(s.mtx);
    if (slot < 0) {
        free_deck(d);
        usbdeck_problem("Too many Stream Decks (max %d)", USBDECK_MAX);
        return ESP_ERR_NOT_SUPPORTED;
    }

    char name[16];
    snprintf(name, sizeof name, "deck%u", link->addr);
    if (xTaskCreate(deck_task, name, DECK_STACK, d, DECK_PRIO, &d->task) != pdPASS) {
        xSemaphoreTake(s.mtx, portMAX_DELAY);
        s.decks[slot] = NULL;
        xSemaphoreGive(s.mtx);
        free_deck(d);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void deck_dev_gone(usb_device_handle_t dev)
{
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    for (int i = 0; i < USBDECK_MAX; i++) {
        usbdeck_t *d = s.decks[i];
        if (d && d->link.dev == dev) {
            atomic_store(&d->gone, true);
            hid_raw_mark_gone(&d->hid); /* a late OUT transfer may be cancelled now */
            if (d->ids_read) { /* during our own bus reset, or a replug / the deck restarting */
                upol_note_detach(POL, d->key, usbdeck_bus_resetting(), esp_timer_get_time());
                pol_saved();
            }
            xTaskNotifyGive(d->task); /* under the lock: the task can't have exited yet */
            break;
        }
    }
    xSemaphoreGive(s.mtx);
}

static esp_err_t post(usbdeck_t *d, const deck_cmd_t *c)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    if (is_gone(d)) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        atomic_fetch_add(&d->pending, 1); /* before it is in the queue: never seen drained with it there */
        if (xQueueSend(d->cmdq, c, 0) != pdTRUE) {
            atomic_fetch_sub(&d->pending, 1);
            err = ESP_ERR_TIMEOUT;
        } else {
            xTaskNotifyGive(d->task);
        }
    }
    xSemaphoreGive(s.mtx);
    return err;
}

/* ---- public write API ---- */

static esp_err_t post_image(usbdeck_t *d, deck_cmd_t *c, const uint8_t *img, size_t len)
{
    if (is_gone(d)) return ESP_ERR_INVALID_STATE;
    /* Quarantined: takes no images, dropped here and never queued (its lift asks for a redraw). */
    if (atomic_load(&d->quarantine) != UPOL_Q_NONE) return ESP_OK;
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) copy = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, img, len);
    c->img.data = copy;
    c->img.len = len;
    esp_err_t err = post(d, c);
    if (err != ESP_OK) heap_caps_free(copy);
    return err;
}

esp_err_t usbdeck_write_key(usbdeck_t *d, uint8_t hw, const uint8_t *img, size_t len)
{
    if (!d || !img || !len) return ESP_ERR_INVALID_ARG;
    if (d->model->fmt == DECK_IMG_NONE) return ESP_ERR_NOT_SUPPORTED;
    if (hw >= d->model->cols * d->model->rows) return ESP_ERR_INVALID_ARG;
    deck_cmd_t c = {.kind = CMD_KEY, .img = {.hw = hw}};
    return post_image(d, &c, img, len);
}

esp_err_t usbdeck_write_lcd(usbdeck_t *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const uint8_t *jpeg, size_t len)
{
    if (!d || !jpeg || !len) return ESP_ERR_INVALID_ARG;
    deck_img_writer_t probe;
    deckproto_img_lcd(&probe, d->model, x, y, w, h, jpeg, len);
    if (probe.mode == DECK_W_NONE) return ESP_ERR_NOT_SUPPORTED;
    deck_cmd_t c = {.kind = CMD_LCD, .img = {.x = x, .y = y, .w = w, .h = h}};
    return post_image(d, &c, jpeg, len);
}

esp_err_t usbdeck_write_info(usbdeck_t *d, const uint8_t *jpeg, size_t len)
{
    if (!d || !jpeg || !len) return ESP_ERR_INVALID_ARG;
    deck_img_writer_t probe;
    deckproto_img_info(&probe, d->model, jpeg, len);
    if (probe.mode == DECK_W_NONE) return ESP_ERR_NOT_SUPPORTED;
    deck_cmd_t c = {.kind = CMD_INFO};
    return post_image(d, &c, jpeg, len);
}

esp_err_t usbdeck_fill_key(usbdeck_t *d, uint8_t hw, uint8_t r, uint8_t g, uint8_t b)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    deck_cmd_t c = {.kind = CMD_FEATURE};
    c.feat.len = (uint8_t)deckproto_feat_fill_key(d->model, hw, r, g, b, c.feat.buf);
    if (c.feat.len == 0) return ESP_ERR_NOT_SUPPORTED; /* gen1 */
    return post(d, &c);
}

esp_err_t usbdeck_set_brightness(usbdeck_t *d, uint8_t pct)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    deck_cmd_t c = {.kind = CMD_FEATURE};
    c.feat.len = (uint8_t)deckproto_feat_brightness(d->model, pct, c.feat.buf);
    return post(d, &c);
}

bool usbdeck_writes_drained(const usbdeck_t *d) { return !d || atomic_load(&d->pending) == 0; }

/* ---- raw passthrough ---- */

static TickType_t ticks_ceil_ms(uint32_t ms)
{
    return (TickType_t)(((uint64_t)ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
}

/* Like post, but waits up to wait_ms for queue space, outside s.mtx: the queue outlives the call
 * (the caller's on_detach waits for it), and a command queued after deck_teardown's first drain
 * is freed by its last one. *queued: the queue owns the command now (also with
 * ESP_ERR_INVALID_STATE, when the deck went meanwhile). */
static esp_err_t post_wait(usbdeck_t *d, const deck_cmd_t *c, uint32_t wait_ms, bool *queued)
{
    *queued = false;
    if (is_gone(d)) return ESP_ERR_INVALID_STATE;
    atomic_fetch_add(&d->pending, 1); /* as post */
    if (xQueueSend(d->cmdq, c, ticks_ceil_ms(wait_ms)) != pdTRUE) {
        atomic_fetch_sub(&d->pending, 1);
        return ESP_ERR_TIMEOUT;
    }
    *queued = true;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    if (is_gone(d))
        err = ESP_ERR_INVALID_STATE;
    else
        xTaskNotifyGive(d->task); /* under the lock: the task can't have exited yet */
    xSemaphoreGive(s.mtx);
    return err;
}

esp_err_t usbdeck_write_raw(usbdeck_t *d, const uint8_t *report, size_t len, uint32_t wait_ms)
{
    if (!d || !report || !len) return ESP_ERR_INVALID_ARG;
    if (is_gone(d)) return ESP_ERR_INVALID_STATE;
    if (atomic_load(&d->quarantine) != UPOL_Q_NONE) return ESP_OK; /* as post_image */
    const size_t n = len < d->model->report_len ? len : d->model->report_len;
    uint8_t *copy = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!copy) copy = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, report, n);
    deck_cmd_t c = {.kind = CMD_RAW, .img = {.data = copy, .len = n}};
    bool queued;
    const esp_err_t err = post_wait(d, &c, wait_ms, &queued);
    if (!queued) heap_caps_free(copy);
    return err;
}

esp_err_t usbdeck_feature_set(usbdeck_t *d, const uint8_t *buf, size_t len, uint32_t wait_ms)
{
    if (!d || !buf || !len || len > FEATURE_MAX) return ESP_ERR_INVALID_ARG;
    deck_cmd_t c = {.kind = CMD_FEATURE};
    c.feat.len = (uint8_t)len;
    memcpy(c.feat.buf, buf, len);
    bool queued;
    return post_wait(d, &c, wait_ms, &queued);
}

esp_err_t usbdeck_feature_get(usbdeck_t *d, uint8_t report_id, uint8_t *buf, size_t cap, size_t *out_len,
                              uint32_t wait_ms)
{
    if (out_len) *out_len = 0;
    if (!d || !buf || !out_len) return ESP_ERR_INVALID_ARG;
    feat_get_t *g = calloc(1, sizeof *g);
    if (!g) return ESP_ERR_NO_MEM;
    g->done = xSemaphoreCreateBinary();
    if (!g->done) {
        free(g);
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&g->refs, 2);
    g->id = report_id;
    const TickType_t start = xTaskGetTickCount(), total = ticks_ceil_ms(wait_ms);
    deck_cmd_t c = {.kind = CMD_FEATURE_GET, .get = g};
    bool queued;
    esp_err_t err = post_wait(d, &c, wait_ms, &queued);
    if (!queued) atomic_fetch_sub(&g->refs, 1); /* the queue's reference was never taken */
    if (err == ESP_OK) {
        const TickType_t spent = xTaskGetTickCount() - start;
        if (xSemaphoreTake(g->done, spent < total ? total - spent : 0) != pdTRUE) {
            err = ESP_ERR_TIMEOUT; /* the deck task drops the request's other reference */
        } else {
            err = g->err;
            if (err == ESP_OK) {
                *out_len = g->len < cap ? g->len : cap;
                memcpy(buf, g->data, *out_len);
            }
        }
    }
    get_put(g);
    return err;
}

void usbdeck_set_raw_input(usbdeck_t *d, usbdeck_raw_input_fn fn, void *ctx)
{
    if (!d) return;
    portENTER_CRITICAL(&d->raw_mux);
    d->raw_fn = fn;
    d->raw_ctx = fn ? ctx : NULL;
    portEXIT_CRITICAL(&d->raw_mux);
    atomic_store(&d->raw_on, fn != NULL);
}

bool usbdeck_passthrough(const usbdeck_t *d)
{
    return d && atomic_load(&d->raw_on);
}

esp_err_t usbdeck_feature_kept(const usbdeck_t *d, uint8_t report_id, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!d || !buf || !out_len) return ESP_ERR_INVALID_ARG;
    return kept_copy(d, report_id, buf, cap, out_len); /* read-only since on_attach */
}

const char *usbdeck_serial(const usbdeck_t *d) { return d ? d->info.serial : ""; }

const char *usbdeck_firmware(const usbdeck_t *d) { return d ? d->info.fw : ""; }

unsigned usbdeck_stack_free(const usbdeck_t *d)
{
    return d && d->task ? (unsigned)uxTaskGetStackHighWaterMark(d->task) : 0;
}

void usbdeck_trouble(usbdeck_trouble_t *out)
{
    *out = (usbdeck_trouble_t){.reports = atomic_load(&s.trouble_reports)};
    if (!s.mtx) return;
    const uint32_t now = uptime_s();
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    for (int i = 0; i < USBDECK_MAX; i++) {
        usbdeck_t *d = s.decks[i];
        /* failing now: a reported streak with a failure in the last UPOL_FAILING_HOLD_S, so a deck
         * that recovered stops counting without waiting for a key press or an image */
        if (d && d->attached && !is_gone(d) &&
            (atomic_load(&d->quarantine) != UPOL_Q_NONE ||
             upol_failing(atomic_load(&d->in_problem_sent), atomic_load(&d->in_fail_s), now) ||
             upol_failing(atomic_load(&d->write_problem_sent), atomic_load(&d->write_fail_s), now)))
            out->troubled++;
    }
    xSemaphoreGive(s.mtx);
}

int usbdeck_list(usbdeck_info_t *out, int max)
{
    if (!out || max <= 0 || !s.mtx) return 0;
    int n = 0;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    for (int i = 0; i < USBDECK_MAX && n < max; i++) {
        usbdeck_t *d = s.decks[i];
        if (d && d->attached && !is_gone(d)) out[n++] = d->info;
    }
    xSemaphoreGive(s.mtx);
    return n;
}
