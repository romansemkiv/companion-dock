#include "satclient/satclient.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "satclient_logic.h"
#include "satproto/lineasm.h"

#define TAG "satclient"

#define RX_LINE_CAP (512 * 1024)
#define TX_CAP (16 * 1024)
#define RECV_CHUNK 4096
#define QUEUE_DEPTH 32
#define SELECT_MS 50 /* every wait's bound; the connected loop is woken earlier by post() */
#define CONNECT_TIMEOUT_MS 3000
#define WRITE_TIMEOUT_MS 2000
#define TOO_OLD_RETRY_MS 30000
#define HOST_MAX 128

typedef enum { K_ADD, K_REMOVE, K_KEY, K_ROTATE, K_PAGE } item_kind_t;

typedef struct {
    item_kind_t kind;
    char device_id[48];
    char control_id[8];
    int val;
    bool flag;
    char *product_name, *manifest_json, *label; /* ADD only; heap */
} item_t;

static struct {
    SemaphoreHandle_t mtx;
    QueueHandle_t q;
    TaskHandle_t task;
    sat_session_cb_t user_cb;
    void *user_ctx;
    sat_session_t sess;
    sat_lineasm_t la;
    char *rxbuf, *txbuf;
    char *chunk; /* the recv() destination, RECV_CHUNK bytes (PSRAM; was a static in internal .bss) */
    int fd;
    int efd;              /* eventfd: post() makes it readable, ending the connected loop's select */
    atomic_uint dropped;  /* post(): items dropped (queue full or not started), logged by the task */
    atomic_uint efd_fails; /* post(): eventfd writes that failed, logged by the task */
    atomic_int efd_errno; /* the last one's errno */
    sat_caps_t caps;      /* mtx */
    char host[HOST_MAX];  /* mtx */
    uint16_t port;        /* mtx */
    uint32_t target_gen;  /* mtx */
    atomic_int state;
    bool io_failed, too_old, got_ready; /* task only */
} g = {.fd = -1, .efd = -1};

static const char *state_name(satc_state_t s)
{
    switch (s) {
    case SATC_IDLE: return "IDLE";
    case SATC_CONNECTING: return "CONNECTING";
    case SATC_CONNECTED: return "CONNECTED";
    default: return "TOO_OLD";
    }
}

static void set_state(satc_state_t s)
{
    int old = atomic_exchange(&g.state, (int)s);
    if (old != (int)s) ESP_LOGI(TAG, "state %s -> %s", state_name((satc_state_t)old), state_name(s));
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

satc_state_t satclient_state(void) { return (satc_state_t)atomic_load(&g.state); }

void satclient_caps(sat_caps_t *out)
{
    if (!g.mtx) { memset(out, 0, sizeof *out); return; }
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    *out = g.caps;
    xSemaphoreGive(g.mtx);
}

void satclient_get_target(char *host, size_t cap, uint16_t *port)
{
    if (host && cap) host[0] = '\0';
    if (port) *port = 0;
    if (!g.mtx) return;
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    if (host && cap) snprintf(host, cap, "%s", g.host);
    if (port) *port = g.port;
    xSemaphoreGive(g.mtx);
}

/* ---- session callbacks ---- */

static long io_send(void *ctx, const char *p, size_t n, bool *again)
{
    (void)ctx;
    const ssize_t r = send(g.fd, p, n, 0);
    *again = r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    return (long)r;
}

static void io_wait(void *ctx)
{
    (void)ctx;
    fd_set w;
    FD_ZERO(&w);
    FD_SET(g.fd, &w);
    struct timeval tv = {0, SELECT_MS * 1000};
    select(g.fd + 1, NULL, &w, NULL, &tv);
    esp_task_wdt_reset();
}

static void sess_send(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    if (g.fd < 0 || g.io_failed) return;
    /* satc_write_all: WRITE_TIMEOUT_MS per line, counted from the last line end sent, as when each line was
     * its own write */
    const satc_io_t io = {.send = io_send, .wait = io_wait, .ctx = NULL};
    const size_t off = satc_write_all(&io, data, len, WRITE_TIMEOUT_MS, SELECT_MS);
    if (off < len) {
        ESP_LOGW(TAG, "send failed (errno %d)", errno);
        g.io_failed = true;
    }
}

static void on_ready_w(void *ctx, const sat_caps_t *caps)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.caps = *caps;
    xSemaphoreGive(g.mtx);
    g.got_ready = true;
    set_state(SATC_CONNECTED);
    if (g.user_cb.on_ready) g.user_cb.on_ready(ctx, caps);
}

static void on_too_old_w(void *ctx, const char *api_version)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.caps = g.sess.caps; /* the refused Companion's version, for status screens */
    xSemaphoreGive(g.mtx);
    g.too_old = true;
    if (g.user_cb.on_too_old) g.user_cb.on_too_old(ctx, api_version);
}

static void on_line(void *ctx, char *line)
{
    (void)ctx;
    sat_session_on_line(&g.sess, line, now_ms());
}

/* ---- queue ---- */

static void item_free(item_t *it)
{
    free(it->product_name);
    free(it->manifest_json);
    free(it->label);
    it->product_name = it->manifest_json = it->label = NULL;
}

/* Any task. The connected loop waits in select() on Companion's socket, which a task notification
 * doesn't end: the eventfd write does, so a key press goes out at once instead of when the 50 ms
 * timeout expires. The notification wakes the waits outside a connection (wait_ms).
 * No log here: a deck's key comes from its 4 KB task, where a log line (vsnprintf, the web log's
 * 256 B line, the console's vprintf) would take most of the stack. Failures are counted, and the
 * client task logs them (report_drops). */
static void post(item_t *it)
{
    if (!g.q || xQueueSend(g.q, it, 0) != pdTRUE) {
        atomic_fetch_add(&g.dropped, 1);
        item_free(it);
        return;
    }
    if (g.efd >= 0) {
        const uint64_t one = 1;
        if (write(g.efd, &one, sizeof one) != sizeof one) {
            atomic_store(&g.efd_errno, errno);
            atomic_fetch_add(&g.efd_fails, 1);
        }
    }
    if (g.task) xTaskNotifyGive(g.task);
}

/* The client task: what post() counted since the last call. */
static void report_drops(void)
{
    const unsigned dropped = atomic_exchange(&g.dropped, 0), fails = atomic_exchange(&g.efd_fails, 0);
    if (dropped) ESP_LOGW(TAG, "queue full or not started: %u item(s) dropped", dropped);
    if (fails) ESP_LOGW(TAG, "eventfd write failed %u time(s) (errno %d)", fails, atomic_load(&g.efd_errno));
}

/* Any task (a deck's 4 KB one too): the ids copied without snprintf. */
static void item_init(item_t *it, item_kind_t k, const char *dev, const char *ctl)
{
    memset(it, 0, sizeof *it);
    it->kind = k;
    if (dev) strlcpy(it->device_id, dev, sizeof it->device_id);
    if (ctl) strlcpy(it->control_id, ctl, sizeof it->control_id);
}

void satclient_add_device(const sat_device_desc_t *d)
{
    item_t it;
    item_init(&it, K_ADD, d->device_id, NULL);
    it.flag = d->brightness;
    it.product_name = d->product_name ? strdup(d->product_name) : NULL;
    it.manifest_json = d->manifest_json ? strdup(d->manifest_json) : NULL;
    it.label = d->change_page_label ? strdup(d->change_page_label) : NULL;
    if ((d->product_name && !it.product_name) || (d->manifest_json && !it.manifest_json) ||
        (d->change_page_label && !it.label)) {
        ESP_LOGW(TAG, "out of memory copying device");
        item_free(&it);
        return;
    }
    post(&it);
}

void satclient_remove_device(const char *id)
{
    item_t it;
    item_init(&it, K_REMOVE, id, NULL);
    post(&it);
}

void satclient_key(const char *dev, const char *ctl, bool pressed)
{
    item_t it;
    item_init(&it, K_KEY, dev, ctl);
    it.flag = pressed;
    post(&it);
}

void satclient_rotate(const char *dev, const char *ctl, int delta)
{
    item_t it;
    item_init(&it, K_ROTATE, dev, ctl);
    it.val = delta;
    post(&it);
}

void satclient_change_page(const char *dev, bool forward)
{
    item_t it;
    item_init(&it, K_PAGE, dev, NULL);
    it.flag = forward;
    post(&it);
}

static void drain_queue(bool ready)
{
    item_t it;
    report_drops();
    while (xQueueReceive(g.q, &it, 0) == pdTRUE) {
        if (ready && !g.io_failed) {
            switch (it.kind) {
            case K_ADD: {
                sat_device_desc_t d = {.device_id = it.device_id,
                                       .product_name = it.product_name,
                                       .manifest_json = it.manifest_json,
                                       .brightness = it.flag,
                                       .change_page_label = it.label};
                if (!sat_session_add_device(&g.sess, &d)) ESP_LOGW(TAG, "ADD-DEVICE did not fit TX buffer");
                break;
            }
            case K_REMOVE: sat_session_remove_device(&g.sess, it.device_id); break;
            case K_KEY: sat_session_key(&g.sess, it.device_id, it.control_id, it.flag); break;
            case K_ROTATE: sat_session_rotate(&g.sess, it.device_id, it.control_id, it.val); break;
            case K_PAGE: sat_session_change_page(&g.sess, it.device_id, it.flag); break;
            }
        }
        item_free(&it);
    }
}

/* ---- connection ---- */

static bool target_changed(uint32_t seen)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    bool c = g.target_gen != seen;
    xSemaphoreGive(g.mtx);
    return c;
}

/* Waits up to ms, dropping queued items. Returns true if the target changed. */
static bool wait_ms(uint32_t ms, uint32_t seen)
{
    uint32_t waited = 0;
    while (waited < ms) {
        uint32_t step = ms - waited < SELECT_MS ? ms - waited : SELECT_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(step));
        esp_task_wdt_reset();
        drain_queue(false);
        if (target_changed(seen)) return true;
        waited += step;
    }
    return false;
}

static void close_conn(void)
{
    if (g.fd >= 0) {
        close(g.fd);
        g.fd = -1;
    }
}

/* Returns connected fd or -1. Aborts early if the target changes. */
static int connect_to(const char *host, uint16_t port, uint32_t seen)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    /* A host name with the DNS server gone can take longer than the task watchdog's 10 s (lwIP
     * retries each server for ~7 s): off the watchdog for the lookup alone. */
    esp_task_wdt_delete(NULL);
    int rc = getaddrinfo(host, portstr, &hints, &res);
    esp_task_wdt_add(NULL);
    if (rc != 0 || !res) {
        ESP_LOGW(TAG, "resolve %s failed (%d)", host, rc);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);

    int c = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (c < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect failed (errno %d)", errno);
        close(fd);
        return -1;
    }
    if (c < 0) {
        int waited = 0;
        for (;;) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(fd, &w);
            struct timeval tv = {0, SELECT_MS * 1000};
            int s = select(fd + 1, NULL, &w, NULL, &tv);
            esp_task_wdt_reset();
            if (s > 0) break;
            waited += SELECT_MS;
            if (waited >= CONNECT_TIMEOUT_MS || target_changed(seen)) {
                close(fd);
                return -1;
            }
        }
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) {
            ESP_LOGW(TAG, "connect failed (err %d)", err);
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void run_connection(void)
{
    sat_lineasm_init(&g.la, g.rxbuf, RX_LINE_CAP);
    sat_session_reset(&g.sess, now_ms());
    g.io_failed = g.too_old = g.got_ready = false;
    ESP_LOGI(TAG, "TCP connected, waiting for BEGIN");
}

static void client_task(void *arg)
{
    (void)arg;
    uint32_t backoff = 500;
    uint32_t seen = 0;
    char host[HOST_MAX];
    uint16_t port;

    /* Every wait below is bounded (SELECT_MS steps) and feeds the watchdog. */
    if (esp_task_wdt_add(NULL) != ESP_OK) ESP_LOGW(TAG, "not on the task watchdog");
    for (;;) {
        esp_task_wdt_reset();
        xSemaphoreTake(g.mtx, portMAX_DELAY);
        seen = g.target_gen;
        snprintf(host, sizeof host, "%s", g.host);
        port = g.port;
        xSemaphoreGive(g.mtx);

        if (host[0] == '\0') {
            set_state(SATC_IDLE);
            backoff = 500;
            wait_ms(1000, seen);
            continue;
        }

        set_state(SATC_CONNECTING);
        drain_queue(false);
        int fd = connect_to(host, port, seen);
        if (target_changed(seen)) {
            if (fd >= 0) close(fd);
            backoff = 500;
            continue;
        }
        if (fd < 0) {
            wait_ms(backoff, seen);
            backoff = backoff >= 2000 ? 5000 : backoff * 2;
            continue;
        }

        g.fd = fd;
        run_connection();
        while (!g.io_failed && !g.too_old && !target_changed(seen)) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(g.fd, &r);
            int maxfd = g.fd;
            if (g.efd >= 0) {
                FD_SET(g.efd, &r);
                if (g.efd > maxfd) maxfd = g.efd;
            }
            const uint32_t ms = satc_wait_ms((unsigned)uxQueueMessagesWaiting(g.q), g.sess.state == SAT_S_READY, SELECT_MS);
            struct timeval tv = {0, (suseconds_t)ms * 1000};
            int s = select(maxfd + 1, &r, NULL, NULL, &tv);
            esp_task_wdt_reset();
            if (s < 0 && errno != EINTR) { g.io_failed = true; break; }
            if (s > 0 && g.efd >= 0 && FD_ISSET(g.efd, &r)) {
                uint64_t v; /* clears it; the queue is drained below */
                (void)read(g.efd, &v, sizeof v);
            }
            if (s > 0 && FD_ISSET(g.fd, &r)) {
                ssize_t n = recv(g.fd, g.chunk, RECV_CHUNK, 0);
                if (n > 0) {
                    sat_lineasm_feed(&g.la, g.chunk, (size_t)n, on_line, NULL);
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    ESP_LOGW(TAG, "connection closed by peer (n=%d errno=%d)", (int)n, errno);
                    break;
                }
            }
            uint32_t t = now_ms();
            sat_session_tick(&g.sess, t);
            if (g.sess.state == SAT_S_READY) {
                ulTaskNotifyTake(pdTRUE, 0);
                drain_queue(true);
            } else {
                drain_queue(false);
            }
            if (!sat_session_alive(&g.sess, t)) {
                ESP_LOGW(TAG, "no data from Companion for 5 s");
                break;
            }
        }
        bool too_old = g.too_old;
        bool ready = g.got_ready;
        close_conn();
        if (target_changed(seen)) {
            backoff = 500;
            continue;
        }
        if (ready) backoff = 500;
        if (too_old) {
            set_state(SATC_TOO_OLD);
            wait_ms(TOO_OLD_RETRY_MS, seen);
            backoff = 500;
        } else {
            set_state(SATC_CONNECTING);
            wait_ms(backoff, seen);
            backoff = backoff >= 2000 ? 5000 : backoff * 2;
        }
    }
}

void satclient_set_target(const char *host, uint16_t port)
{
    if (!g.mtx) return;
    if (!host) host = "";
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    bool empty = host[0] == '\0';
    bool same = empty ? g.host[0] == '\0' : (strcmp(g.host, host) == 0 && g.port == port);
    if (!same) {
        snprintf(g.host, sizeof g.host, "%s", host);
        g.port = empty ? 0 : port;
        g.target_gen++;
    }
    xSemaphoreGive(g.mtx);
    if (!same && g.task) xTaskNotifyGive(g.task);
}

void satclient_retry(void)
{
    if (!g.mtx) return;
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    const bool go = g.host[0] && atomic_load(&g.state) != (int)SATC_CONNECTED;
    if (go) g.target_gen++; /* as a new target: the loop drops what it waits on and connects again */
    xSemaphoreGive(g.mtx);
    if (go && g.task) xTaskNotifyGive(g.task);
}

esp_err_t satclient_start(const sat_session_cb_t *cb, void *ctx)
{
    if (g.task) return ESP_ERR_INVALID_STATE;
    g.user_cb = *cb;
    g.user_ctx = ctx;
    g.mtx = xSemaphoreCreateMutex();
    /* the queue's storage, the TX line buffer and the recv chunk in PSRAM (internal when PSRAM is short): CPU
     * scratch only, from tasks; send() and recv() copy through lwIP's pbufs, no DMA or ISR touches them. Never freed. */
    g.q = xQueueCreateWithCaps(QUEUE_DEPTH, sizeof(item_t), MALLOC_CAP_SPIRAM);
    if (!g.q) g.q = xQueueCreate(QUEUE_DEPTH, sizeof(item_t));
    g.rxbuf = heap_caps_malloc(RX_LINE_CAP, MALLOC_CAP_SPIRAM);
    g.txbuf = heap_caps_malloc(TX_CAP, MALLOC_CAP_SPIRAM);
    if (!g.txbuf) g.txbuf = heap_caps_malloc(TX_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    g.chunk = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_SPIRAM);
    if (!g.chunk) g.chunk = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g.mtx || !g.q || !g.rxbuf || !g.txbuf || !g.chunk) {
        ESP_LOGE(TAG, "allocation failed");
        return ESP_ERR_NO_MEM;
    }
    sat_session_cb_t scb = *cb;
    scb.send = sess_send;
    scb.on_ready = on_ready_w;
    scb.on_too_old = on_too_old_w;
    sat_session_init(&g.sess, &scb, ctx, g.txbuf, TX_CAP);
    /* Without the eventfd the loop still works, on the 50 ms timeout (the latency before the eventfd). */
    const esp_vfs_eventfd_config_t ecfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t e = esp_vfs_eventfd_register(&ecfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "eventfd: %s", esp_err_to_name(e));
    g.efd = eventfd(0, 0);
    if (g.efd < 0) ESP_LOGW(TAG, "no eventfd (errno %d): queued input waits up to %d ms", errno, SELECT_MS);
    atomic_store(&g.state, (int)SATC_IDLE);
    if (xTaskCreate(client_task, "satclient", 6144, NULL, 5, &g.task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
