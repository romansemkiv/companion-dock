/* Deck connections (elgdock_priv.h): a dock's port presents its deck itself, so Companion's
 * Stream Deck library builds the deck's own driver and lists one surface, the deck. The connection
 * answers what the library asks a primary device (0x80 with the deck's VID/PID, 0x83, 0x84, 0x85,
 * 0x86 / 0x8a, 0x1c: nothing connected) and carries the deck's HID traffic (dockproto_deck_message).
 * Per dock, made at its first deck connection:
 *  - "dockrxK" reads the connection and hands Companion's traffic to usbdeck in order: output
 *    reports, feature reports to set, feature reports to read (answered from the copies usbdeck read
 *    at open; the deck is never asked again). When the deck's queue is full, or memory is short, it
 *    waits and stops reading, so TCP holds Companion back: nothing is dropped. Output and feature
 *    reports go through on_traffic first (surfaces' image cache, which holds key images back
 *    while the box shows a message and writes its own frames and replays in between, through
 *    elgdock_write on this same task); it is also called while the connection is quiet.
 *  - "docktxK" sends the deck's input reports (handed over by the deck task through a queue) and the
 *    keepalives, so both keep going while dockrxK waits on USB.
 * Their stacks and the queue are in PSRAM when it can be had: these tasks never touch flash. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "elgdock_priv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

#define TAG ELG_TAG

#define RX_STACK 6144 /* PSRAM; on_traffic's dockcache calls and a warning log from forward() on top */
#define RX_PRIO 5
/* PSRAM. Worst path measured from the image (externals counted): 1120 B sending a report;
 * 2864 B when lwip logs "thread_sem_init: out of memory" from send() (one log line is about 2 KB: the
 * web log's 256 B line and newlib's 1152 B vfprintf frame), plus 216 B for the interrupt frame and the
 * TLS/coprocessor header: over 3072 */
#define TX_STACK 4096
#define TX_PRIO 6        /* input reports: as soon as the deck task hands them over */
#define USB_WAIT_MS 50   /* one try at queueing a report while Companion is held back */
#define NOMEM_WAIT_MS 10 /* out of memory for a report's copy: try again this much later */
#define GET_WAIT_MS 4000 /* a feature read in order with the writes; then the copy at once (host: 5 s) */
#define RX_CAP (8191 + 1) /* largest payload taken: the Original's 8191-byte output report */
#define RX_CHUNK 1460
#define INQ_DEPTH 32
#define WARN_EVERY 100   /* a repeated warning is logged once per this many */

typedef enum { TX_INPUT, TX_KICK } tx_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t len;
    uint32_t gen; /* deck connection the report was taken for */
    uint8_t data[ELG_IN_MAX];
} tx_item_t;

static void rx_task(void *arg);
static void tx_task(void *arg);

static bool make_task(TaskFunction_t fn, const char *name, uint32_t stack, dock_t *k, UBaseType_t prio,
                      TaskHandle_t *out)
{
    if (xTaskCreateWithCaps(fn, name, stack, k, prio, out, MALLOC_CAP_SPIRAM) == pdPASS) return true;
    return xTaskCreate(fn, name, stack, k, prio, out) == pdPASS; /* internal RAM, then */
}

/* Server task: each piece made once, so a failure part way is completed at the next connection. */
static bool ensure_tasks(dock_t *k)
{
    char name[12];
    if (!k->inq) k->inq = xQueueCreateWithCaps(INQ_DEPTH, sizeof(tx_item_t), MALLOC_CAP_SPIRAM);
    if (!k->inq) k->inq = xQueueCreate(INQ_DEPTH, sizeof(tx_item_t));
    if (!k->inq) return false;
    snprintf(name, sizeof name, "docktx%d", k->k);
    if (!k->tx && !make_task(tx_task, name, TX_STACK, k, TX_PRIO, &k->tx)) return false;
    snprintf(name, sizeof name, "dockrx%d", k->k);
    if (!k->rx && !make_task(rx_task, name, RX_STACK, k, RX_PRIO, &k->rx)) return false;
    return true;
}

void elg_deck_accept(dock_t *k, int fd, const char *peer)
{
    if (!ensure_tasks(k)) {
        ESP_LOGE(TAG, "dock %d: no memory for the deck connection", k->k);
        close(fd);
        return;
    }
    elg_lock();
    const int old = k->cs_new;
    k->cs_new = fd;
    snprintf(k->peer_new, sizeof k->peer_new, "%s", peer);
    atomic_store(&k->c_stop, true); /* the connection being served (if any) ends first */
    elg_unlock();
    if (old >= 0) close(old); /* never taken: the newer one wins */
    xSemaphoreTake(k->cs_mtx, portMAX_DELAY);
    if (k->cs >= 0) shutdown(k->cs, SHUT_RDWR);
    xSemaphoreGive(k->cs_mtx);
    xTaskNotifyGive(k->rx);
}

void elg_deck_stop(dock_t *k)
{
    elg_lock();
    const int pending = k->cs_new;
    k->cs_new = -1;
    atomic_store(&k->c_stop, true);
    elg_unlock();
    if (pending >= 0) close(pending);
    xSemaphoreTake(k->cs_mtx, portMAX_DELAY);
    if (k->cs >= 0) shutdown(k->cs, SHUT_RDWR);
    xSemaphoreGive(k->cs_mtx);
}

/* The dock's deck now. Under usb_mtx it stays so while held; without it, only to compare. */
static usbdeck_t *deck_of(dock_t *k)
{
    elg_lock();
    usbdeck_t *d = k->deck;
    elg_unlock();
    return d;
}

static bool warn_now(dock_t *k) { return atomic_fetch_add(&k->warned, 1) % WARN_EVERY == 0; }

/* in_mtx held: one report for the host (never blocks). A drop is counted only: dockrxK reports it when the
 * connection ends (no log formatting on docktxK or the deck's task, whose stacks are small). */
static void queue_input(dock_t *k, const uint8_t *report, size_t len)
{
    if (!k->inq) return; /* no connection was ever made: nothing to tell */
    tx_item_t it = {.kind = TX_INPUT, .len = (uint8_t)(len < ELG_IN_MAX ? len : ELG_IN_MAX), .gen = atomic_load(&k->in_gen)};
    memcpy(it.data, report, it.len);
    if (xQueueSend(k->inq, &it, 0) != pdTRUE) atomic_fetch_add(&k->in_dropped, 1);
}

/* in_mtx held: what the corner gate gives, for the host. */
static void queue_corner(dock_t *k, const corner_gate_out_t *co)
{
    for (int i = 0; i < co->n; i++) queue_input(k, co->r[i], co->len[i]);
}

unsigned elgdock_tx_stack_free(int k)
{
    if (k < 0 || k >= ELGDOCK_MAX || !elg.dock[k].tx) return 0;
    return (unsigned)uxTaskGetStackHighWaterMark(elg.dock[k].tx);
}

/* No lock held: what the corner gate did on the dock's deck: a corner's press passed on, the
 * gesture. On docktxK or the deck's task (small stacks): on_corner only records it (the open, its log
 * and its click run on the surfaces task). */
static void corner_made(dock_t *k, const corner_gate_out_t *co)
{
    if (!co->open && !co->pressed) return;
    usbdeck_t *d = deck_of(k);
    if (!d || !elg.cb.on_corner) return;
    if (co->pressed) elg.cb.on_corner(d, false);
    if (co->open) elg.cb.on_corner(d, true);
}

/* Deck task: an input report for the host, unless the box holds the decks' input (ingate.h); then through
 * the gesture's gate (while armed, its two keys' key-downs held back 120 ms for the chord; elg.gesture).
 * Never blocks (in_mtx is held only briefly). */
static void raw_input(void *ctx, const uint8_t *report, size_t len)
{
    dock_t *k = ctx;
    uint8_t out[INGATE_MAX];
    corner_gate_out_t co = {0};
    xSemaphoreTake(k->in_mtx, portMAX_DELAY);
    if (ingate_report(&k->gate, report, len, out)) {
        corner_gate_report(&k->corner, out, len < INGATE_MAX ? len : INGATE_MAX, elg_now_ms(), &co);
        queue_corner(k, &co); /* docktxK wakes for it and looks at the gate's next deadline */
    }
    xSemaphoreGive(k->in_mtx);
    corner_made(k, &co);
}

bool elgdock_set_gesture(const corner_cfg_t *g)
{
    if (!elg.mtx) return false; /* not running yet: the caller gives it again */
    const corner_cfg_t use = corner_cfg_use(g);
    elg_lock(); /* then each dock's in_mtx: the order elgdock_assign takes them in */
    if (!corner_cfg_eq(&elg.gesture, &use)) {
        elg.gesture = use;
        for (int i = 0; i < ELGDOCK_MAX; i++) {
            dock_t *k = &elg.dock[i];
            if (!k->deck) continue; /* assigned later: elgdock_assign gives its gate elg.gesture */
            xSemaphoreTake(k->in_mtx, portMAX_DELAY);
            corner_gate_init(&k->corner, deck_model_find(k->vid, k->pid), &use);
            xSemaphoreGive(k->in_mtx);
        }
    }
    elg_unlock();
    return true;
}

void elgdock_hold_input(const char *const *serials, int n)
{
    if (!elg.mtx) return;
    if (n < 0 || !serials) n = 0;
    if (n > ELGDOCK_MAX) n = ELGDOCK_MAX;
    unsigned changed = 0, now_held = 0; /* logged after elg.mtx is let go (no log under it) */
    elg_lock(); /* then each dock's in_mtx: the order elgdock_assign takes them in */
    elg.nhold = n;
    const char *held[ELGDOCK_MAX];
    for (int h = 0; h < n; h++) {
        snprintf(elg.hold[h], sizeof elg.hold[h], "%s", serials[h] ? serials[h] : "");
        held[h] = elg.hold[h];
    }
    for (int i = 0; i < ELGDOCK_MAX; i++) {
        dock_t *k = &elg.dock[i];
        if (!k->deck) continue; /* assigned later: ingate_init takes the hold up */
        uint8_t rel[2][INGATE_MAX], rl[2];
        xSemaphoreTake(k->in_mtx, portMAX_DELAY); /* a dock whose deck's hold doesn't change: nothing happens */
        const bool hold = ingate_held(k->deck_serial, held, n), was = k->gate.hold;
        const int r = ingate_hold(&k->gate, hold, rel, rl);
        if (hold != was) corner_gate_reset(&k->corner); /* the gesture starts again either way */
        for (int j = 0; j < r; j++) queue_input(k, rel[j], rl[j]); /* Companion lets go of what it saw pressed */
        xSemaphoreGive(k->in_mtx);
        if (hold != was && k->gate.screen) {
            changed |= 1u << i;
            if (hold) now_held |= 1u << i;
        }
    }
    elg_unlock();
    /* one 32 B copy at a time, not one per dock (the frame stays small); the dock's deck as it is
     * now, so a deck reassigned in between is named by its successor: a log line only */
    for (int i = 0; i < ELGDOCK_MAX; i++)
        if (changed & (1u << i)) {
            char serial[sizeof elg.dock[0].deck_serial];
            elg_lock();
            memcpy(serial, elg.dock[i].deck_serial, sizeof serial);
            elg_unlock();
            ESP_LOGI(TAG, "dock %d: deck %s: its input %s", i, serial,
                     now_held & (1u << i) ? "stays with the box (maintenance screen)" : "goes to Companion again");
        }
}

/* dockrxK: a reply on the connection it serves. */
static void deck_send(dock_t *k, int fd, uint16_t flags, uint32_t id, const uint8_t *pl, size_t len)
{
    xSemaphoreTake(k->tx_mtx, portMAX_DELAY);
    if (k->cs == fd && !elg_send_msg(fd, flags, CORA_OP_GET_REPORT, id, pl, len)) shutdown(fd, SHUT_RDWR);
    xSemaphoreGive(k->tx_mtx);
}

/* An output report or feature report for the session's deck d, in order: retried while the deck's
 * queue is full or memory is short (the socket isn't read meanwhile). False when the connection is
 * to end (replaced, or d is no longer the dock's deck). */
static bool forward(dock_t *k, usbdeck_t *d, const uint8_t *payload, size_t len, bool feature)
{
    if (feature && len > USBDECK_FEATURE_MAX) len = USBDECK_FEATURE_MAX;
    esp_err_t err;
    for (;;) {
        xSemaphoreTake(k->usb_mtx, portMAX_DELAY);
        if (deck_of(k) != d)
            err = ESP_ERR_INVALID_STATE;
        else if (feature)
            err = usbdeck_feature_set(d, payload, len, USB_WAIT_MS);
        else
            err = usbdeck_write_raw(d, payload, len, USB_WAIT_MS);
        xSemaphoreGive(k->usb_mtx);
        if (err == ESP_ERR_NO_MEM)
            vTaskDelay(pdMS_TO_TICKS(NOMEM_WAIT_MS) ? pdMS_TO_TICKS(NOMEM_WAIT_MS) : 1);
        else if (err != ESP_ERR_TIMEOUT)
            break;
        if (atomic_load(&k->c_stop)) return false;
    }
    if (err == ESP_ERR_INVALID_STATE) return false;
    if (err != ESP_OK && warn_now(k))
        ESP_LOGW(TAG, "dock %d: %s not passed on: %s", k->k, feature ? "feature report" : "output report",
                 esp_err_to_name(err));
    return true;
}

/* Companion's output or feature report: through on_traffic (which may take care of it), else on as
 * it is. False when the connection is to end. */
static bool deck_forward(dock_t *k, usbdeck_t *d, const cora_msg_t *m, bool feature)
{
    if (elg.cb.on_traffic && !elg.cb.on_traffic(d, feature, m->payload, m->len))
        return !atomic_load(&k->c_stop) && deck_of(k) == d;
    return forward(k, d, m->payload, m->len, feature);
}

bool elgdock_write(usbdeck_t *d, bool feature, const uint8_t *report, size_t len)
{
    const int i = elgdock_dock_of(d);
    if (i < 0 || !report) return false;
    dock_t *k = &elg.dock[i];
    if (atomic_load(&k->c_stop)) return false;
    return forward(k, d, report, len, feature);
}

/* The deck's driver reading one of the deck's feature reports (GET_REPORT [0x03, id] on a primary).
 * Never asked of the deck: answered from usbdeck's copy read at open, in order with the writes (or at
 * once, should that take GET_WAIT_MS), flagged VERBATIM so that the host keys it by its first byte,
 * the report ID (hid-device/cora.ts:56-58). A report without a copy is left unanswered, as a deck
 * refusing it would be over USB: the host times the read out after 5 s (an empty answer would resolve
 * nothing either). */
static void deck_read(dock_t *k, usbdeck_t *d, int fd, uint8_t report_id, uint32_t msg_id)
{
    uint8_t rep[USBDECK_FEATURE_MAX];
    size_t n = 0;
    esp_err_t err;
    xSemaphoreTake(k->usb_mtx, portMAX_DELAY);
    if (deck_of(k) != d) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = usbdeck_feature_get(d, report_id, rep, sizeof rep, &n, GET_WAIT_MS);
        if (err == ESP_ERR_TIMEOUT) err = usbdeck_feature_kept(d, report_id, rep, sizeof rep, &n);
    }
    xSemaphoreGive(k->usb_mtx);
    if (err == ESP_OK && n) {
        deck_send(k, fd, CORA_F_VERBATIM | CORA_F_RESULT, msg_id, rep, n);
    } else if (err != ESP_ERR_INVALID_STATE && warn_now(k)) {
        ESP_LOGW(TAG, "dock %d: feature report 0x%02x left unanswered: %s", k->k, report_id,
                 err == ESP_ERR_NOT_FOUND ? "no copy (the deck is never asked after open)" : esp_err_to_name(err));
    }
}

/* One message from the host. False when the connection is to end. */
static bool deck_handle(dock_t *k, usbdeck_t *d, int fd, const dock_deck_t *deck, const cora_msg_t *m)
{
    uint8_t out[DOCK_CHILD_LEN];
    size_t n = 0;
    uint8_t id = 0;
    switch (dockproto_deck_message(deck, m, out, &n, &id)) {
    case DOCK_DECK_WRITE: return deck_forward(k, d, m, false);
    case DOCK_DECK_SET: return deck_forward(k, d, m, true);
    case DOCK_DECK_ANSWER:
        if (id == DOCK_REP_DEVINFO)
            ESP_LOGI(TAG, "dock %d: the host asked who is on port %d: the deck %s (VID 0x%04x PID 0x%04x)", k->k,
                     ELGDOCK_PRIMARY_PORT(k->k), deck->serial, deck->vid, deck->pid);
        deck_send(k, fd, CORA_F_RESULT, m->msg_id, out, n);
        return true;
    case DOCK_DECK_READ: deck_read(k, d, fd, id, m->msg_id); return true;
    default: return true; /* ACKs (the data itself shows the host is there), the probes */
    }
}

/* dockrxK: serves one deck connection until it ends. */
static void deck_session(dock_t *k, int fd, const char *peer, uint8_t *buf, uint8_t *chunk)
{
    cora_parser_t p;
    cora_parser_init(&p, buf, RX_CAP);

    xSemaphoreTake(k->usb_mtx, portMAX_DELAY);
    char serial[sizeof k->deck_serial], fw[sizeof k->deck_fw];
    elg_lock();
    usbdeck_t *d = k->deck;
    const dock_deck_t deck = {.vid = k->vid, .pid = k->pid, .serial = serial, .firmware = fw, .mac = elg.mac};
    memcpy(serial, k->deck_serial, sizeof serial);
    memcpy(fw, k->deck_fw, sizeof fw);
    elg_unlock();
    if (!d) { /* the deck went before the connection could be served: its host comes back with it */
        xSemaphoreGive(k->usb_mtx);
        close(fd);
        return;
    }
    xSemaphoreTake(k->tx_mtx, portMAX_DELAY);
    xSemaphoreTake(k->cs_mtx, portMAX_DELAY);
    k->cs = fd;
    atomic_store(&k->in_gen, ++k->c_gen);
    xSemaphoreGive(k->cs_mtx);
    xSemaphoreGive(k->tx_mtx);
    elg_lock();
    k->companion = true;
    snprintf(k->peer, sizeof k->peer, "%s", peer);
    elg_unlock();
    xSemaphoreTake(k->in_mtx, portMAX_DELAY); /* the corner gate starts afresh with the connection */
    corner_gate_reset(&k->corner);
    xSemaphoreGive(k->in_mtx);
    usbdeck_set_raw_input(d, raw_input, k);
    if (elg.cb.on_owner) elg.cb.on_owner(d, true, peer);
    xSemaphoreGive(k->usb_mtx);
    const tx_item_t kick = {.kind = TX_KICK};
    xQueueSend(k->inq, &kick, portMAX_DELAY); /* first keepalive now */
    ESP_LOGI(TAG, "dock %d: %s connected to the deck (port %d)", k->k, peer, ELGDOCK_PRIMARY_PORT(k->k));

    int64_t rx_at = elg_now_ms();
    bool go = true;
    while (go && !atomic_load(&k->c_stop)) { /* the deck going sets c_stop (elgdock_release) */
        const ssize_t n = recv(fd, chunk, RX_CHUNK, 0);
        if (n == 0) break;
        if (n < 0) {
            const bool idle = errno == EAGAIN || errno == EWOULDBLOCK;
            if (idle && elg_now_ms() - rx_at < ELG_IDLE_MS) {
                if (elg.cb.on_traffic) elg.cb.on_traffic(d, false, NULL, 0); /* quiet: the box's own writes */
                continue;
            }
            if (idle) ESP_LOGW(TAG, "dock %d: nothing from %s for 6 s", k->k, peer);
            break;
        }
        rx_at = elg_now_ms();
        size_t at = 0;
        while (go && at < (size_t)n) {
            size_t used = 0;
            cora_msg_t m;
            const int got = cora_feed(&p, chunk + at, (size_t)n - at, &used, &m);
            at += used;
            if (got) go = deck_handle(k, d, fd, &deck, &m);
        }
    }

    xSemaphoreTake(k->usb_mtx, portMAX_DELAY);
    const bool same = deck_of(k) == d; /* else d is gone: never touch it again */
    elg_lock();
    const bool handover = same && k->cs_new >= 0; /* replaced: the next connection takes the deck over */
    k->companion = false;
    k->peer[0] = '\0';
    elg_unlock();
    if (same && !handover) {
        usbdeck_set_raw_input(d, NULL, NULL);
        xSemaphoreTake(k->in_mtx, portMAX_DELAY); /* Companion gone: nothing held back, no chord */
        corner_gate_reset(&k->corner);
        xSemaphoreGive(k->in_mtx);
        if (elg.cb.on_owner) elg.cb.on_owner(d, false, NULL);
    }
    xSemaphoreGive(k->usb_mtx);
    xSemaphoreTake(k->tx_mtx, portMAX_DELAY);
    xSemaphoreTake(k->cs_mtx, portMAX_DELAY);
    close(fd);
    k->cs = -1;
    xSemaphoreGive(k->cs_mtx);
    xSemaphoreGive(k->tx_mtx);
    const unsigned dropped = atomic_exchange(&k->in_dropped, 0);
    ESP_LOGI(TAG, "dock %d: %s disconnected from the deck%s%s (dockrx stack: %u bytes never used)", k->k, peer,
             handover ? " (replaced)" : "", dropped ? " (input reports were dropped)" : "",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void rx_task(void *arg)
{
    dock_t *k = arg;
    uint8_t *buf = heap_caps_malloc(RX_CAP, MALLOC_CAP_SPIRAM);
    uint8_t *chunk = heap_caps_malloc(RX_CHUNK, MALLOC_CAP_SPIRAM);
    bool warned = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            char peer[16];
            elg_lock();
            const int fd = k->cs_new;
            k->cs_new = -1;
            memcpy(peer, k->peer_new, sizeof peer);
            if (fd >= 0) atomic_store(&k->c_stop, false); /* under elg.mtx: a newer accept sets it after */
            elg_unlock();
            if (fd < 0) break;
            if (!buf || !chunk) { /* tried again at each connection, a partial one freed first; only this one closed */
                heap_caps_free(buf);
                heap_caps_free(chunk);
                buf = heap_caps_malloc(RX_CAP, MALLOC_CAP_SPIRAM);
                chunk = heap_caps_malloc(RX_CHUNK, MALLOC_CAP_SPIRAM);
            }
            if (buf && chunk) {
                warned = false;
                deck_session(k, fd, peer, buf, chunk);
            } else {
                if (!warned) ESP_LOGE(TAG, "dock %d: no memory for the deck connection (tried again at the next)", k->k);
                warned = true;
                close(fd);
            }
        }
    }
}

/* docktxK: input reports and keepalives for the deck connection. */
/* the corner gate's turn in a frame of its own: its 204 B output is gone again before the sends below, the
 * deepest path of this 4 KB stack (noinline: at -O2 a static function called once would be inlined back). */
static __attribute__((noinline)) void tx_corner_tick(dock_t *k)
{
    corner_gate_out_t co = {0};
    xSemaphoreTake(k->in_mtx, portMAX_DELAY);
    if (!k->gate.hold) {
        corner_gate_tick(&k->corner, elg_now_ms(), &co);
        queue_corner(k, &co); /* sent on the next turn of the loop */
    }
    xSemaphoreGive(k->in_mtx);
    corner_made(k, &co);
}

static void tx_task(void *arg)
{
    dock_t *k = arg;
    uint32_t gen = 0, msg_id = 0;
    uint8_t conn = 0;
    int64_t next_ka = 0;
    for (;;) {
        xSemaphoreTake(k->tx_mtx, portMAX_DELAY);
        const bool open = k->cs >= 0;
        xSemaphoreGive(k->tx_mtx);
        TickType_t wait = portMAX_DELAY;
        if (open) {
            const int64_t left = next_ka - elg_now_ms();
            wait = left > 0 ? pdMS_TO_TICKS(left) + 1 : 0;
        }
        xSemaphoreTake(k->in_mtx, portMAX_DELAY);
        const int64_t corner_due = corner_gate_due(&k->corner); /* a window or the hold to end on time */
        xSemaphoreGive(k->in_mtx);
        if (corner_due) {
            const int64_t left = corner_due - elg_now_ms();
            const TickType_t t = left > 0 ? pdMS_TO_TICKS(left) + 1 : 0;
            if (t < wait) wait = t;
        }
        tx_item_t it;
        const bool got = xQueueReceive(k->inq, &it, wait) == pdTRUE;
        tx_corner_tick(k);

        xSemaphoreTake(k->tx_mtx, portMAX_DELAY);
        const int fd = k->cs;
        bool ok = true;
        if (fd >= 0) {
            if (k->c_gen != gen) { /* a new connection: its own number, a keepalive at once */
                gen = k->c_gen;
                conn++;
                next_ka = 0;
            }
            if (got && it.kind == TX_INPUT && it.gen == gen)
                ok = elg_send_msg(fd, CORA_F_VERBATIM, CORA_OP_WRITE, 0, it.data, it.len);
            if (ok && elg_now_ms() >= next_ka) {
                uint8_t ka[DOCK_REPLY_LEN];
                dockproto_keepalive(ka, conn);
                ok = elg_send_msg(fd, CORA_F_VERBATIM, CORA_OP_WRITE, ++msg_id, ka, sizeof ka);
                next_ka = elg_now_ms() + DOCK_KEEPALIVE_MS;
            }
            if (!ok) shutdown(fd, SHUT_RDWR); /* dockrxK ends the connection */
        }
        xSemaphoreGive(k->tx_mtx);
    }
}
