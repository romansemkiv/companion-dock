/* Network Dock server (elgdock.h). Protocol from @elgato-stream-deck/tcp 7.8.0 (cora.h,
 * dockproto.h); the deck connections are elgdock_deck.c's, the shared state and the lock order
 * elgdock_priv.h's.
 *
 * The "elgdock" server task owns every dock's listening socket. A dock listens and is announced as
 * mDNS _elg._tcp only while a deck is assigned to it, dock 0 too (dockproto_docks_wanted): it opens
 * and announces its port when a deck comes, and withdraws and closes it when the deck goes (whose
 * connection elgdock_release has ended). A connection goes to the dock's own tasks (elgdock_deck.c:
 * the deck itself on that port); one that comes in the moment after the deck went is closed. No
 * empty Network Dock is ever presented. The task also saves the NVS dock map. It picks up assignment
 * changes within POLL_MS: a deck that goes and comes back faster than that keeps its port and its
 * announcement throughout.
 *
 * A new connection is vetted here first (dockproto_vet): it gets a keepalive, as from a dock, and only one that
 * answers in Cora framing goes to the dock's tasks and takes the deck. A port scan, a probe or a stray connection is
 * closed without touching the connection being served (before, any connection took the deck from Companion for the
 * 5 s until it reconnected) or the box's screens. */
#include "elgdock/elgdock.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "elgdock_priv.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "nvs.h"

#define TAG ELG_TAG

#define SRV_STACK 5120 /* internal: it writes the dock map to flash */
#define SRV_PRIO 4
#define POLL_MS 100
#define LISTEN_RETRY_MS 5000  /* a port that couldn't be opened is tried again this much later */
#define SEND_TIMEOUT_MS 2000  /* deck connections */
#define RECV_TIMEOUT_MS ELGDOCK_IDLE_MS /* deck connections: the quiet calls of on_traffic */
#define NVS_NS "elgdock"
#define MDNS_TYPE "_elg"
#define MDNS_PROTO "_tcp"
#define MDNS_DT "215" /* Network Dock (discoveryService.ts) */
#define VET_MAX 4 /* connections being vetted at once, all docks; a fifth closes the oldest */

elg_t elg;

void elg_lock(void) { xSemaphoreTake(elg.mtx, portMAX_DELAY); }
void elg_unlock(void) { xSemaphoreGive(elg.mtx); }
int64_t elg_now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---- sockets ---- */

static void sock_opts(int fd)
{
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {.tv_sec = SEND_TIMEOUT_MS / 1000, .tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct timeval rv = {.tv_sec = RECV_TIMEOUT_MS / 1000, .tv_usec = (RECV_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rv, sizeof rv);
}

/* -1 with *err = errno when the port can't be opened. */
static int listen_on(uint16_t port, int *err)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        *err = errno;
        return -1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 2) != 0) {
        *err = errno;
        close(fd);
        return -1;
    }
    return fd;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static bool send_all(int fd, const uint8_t *b, size_t n)
{
    while (n) {
        const ssize_t w = send(fd, b, n, 0);
        if (w <= 0) return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
}

bool elg_send_msg(int fd, uint16_t flags, uint8_t op, uint32_t id, const uint8_t *pl, size_t len)
{
    uint8_t buf[ELG_SEND_MAX];
    const cora_msg_t m = {.flags = flags, .hid_op = op, .msg_id = id, .payload = pl, .len = (uint32_t)len};
    const size_t n = cora_encode(&m, buf, sizeof buf);
    return n && send_all(fd, buf, n);
}

/* ---- dock map (NVS) ---- */

static void map_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return; /* nothing stored yet */
    for (int k = 0; k < ELGDOCK_MAX; k++) {
        char key[4];
        snprintf(key, sizeof key, "d%d", k);
        size_t n = sizeof elg.map[k];
        if (nvs_get_str(h, key, elg.map[k], &n) != ESP_OK) elg.map[k][0] = '\0';
    }
    nvs_close(h);
}

/* Server task: writes the map when an assignment changed it. */
static void map_save(void)
{
    char map[ELGDOCK_MAX][32];
    elg_lock();
    const bool dirty = elg.map_dirty;
    elg.map_dirty = false;
    if (dirty) memcpy(map, elg.map, sizeof map); /* copied only when it's to be saved */
    elg_unlock();
    if (!dirty) return;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        for (int k = 0; k < ELGDOCK_MAX && e == ESP_OK; k++) {
            char key[4];
            snprintf(key, sizeof key, "d%d", k);
            e = map[k][0] ? nvs_set_str(h, key, map[k]) : nvs_erase_key(h, key);
            if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        }
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "dock map not saved: %s", esp_err_to_name(e));
}

/* ---- vetting new connections (server task) ---- */

typedef struct {
    int fd; /* -1: free */
    dock_t *k;
    char peer[16];
    int64_t at_ms;
    size_t seen; /* bytes peeked last time, still short of the magic: polled, not selected (no busy loop) */
} vet_t;

static vet_t s_vet[VET_MAX];

static void vet_close(vet_t *v, const char *why)
{
    if (why) ESP_LOGI(TAG, "dock %d: %s turned away (%s): not a Stream Deck host", v->k->k, v->peer, why);
    close(v->fd);
    v->fd = -1;
}

/* A connection accepted on k's port: a keepalive, then it waits for its answer in s_vet. */
static void vet_add(dock_t *k, int fd, const char *peer)
{
    uint8_t ka[DOCK_REPLY_LEN];
    dockproto_keepalive(ka, 0);
    if (!elg_send_msg(fd, CORA_F_VERBATIM, CORA_OP_WRITE, 0, ka, sizeof ka)) {
        close(fd);
        return;
    }
    vet_t *v = NULL;
    for (int i = 0; i < VET_MAX && !v; i++)
        if (s_vet[i].fd < 0) v = &s_vet[i];
    if (!v) { /* full: the oldest goes (a host answers within milliseconds; a flood of probes can't starve it) */
        v = &s_vet[0];
        for (int i = 1; i < VET_MAX; i++)
            if (s_vet[i].at_ms < v->at_ms) v = &s_vet[i];
        vet_close(v, "too many connections at once");
    }
    *v = (vet_t){.fd = fd, .k = k, .at_ms = elg_now_ms()};
    snprintf(v->peer, sizeof v->peer, "%s", peer);
}

/* After select: each vetted connection's first bytes, peeked (left on the socket for the dock's tasks). */
static void vet_check(const fd_set *r)
{
    const int64_t now = elg_now_ms();
    for (int i = 0; i < VET_MAX; i++) {
        vet_t *v = &s_vet[i];
        if (v->fd < 0) continue;
        uint8_t pk[4];
        size_t n = 0;
        bool closed = false;
        if (v->seen || FD_ISSET(v->fd, r)) {
            const ssize_t got = recv(v->fd, pk, sizeof pk, MSG_PEEK | MSG_DONTWAIT);
            if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) closed = true;
            if (got > 0) n = (size_t)got;
        }
        v->seen = n;
        const dock_vet_t verdict = dockproto_vet(pk, n, closed, (uint32_t)(now - v->at_ms));
        if (verdict == DOCK_VET_WAIT) continue;
        if (verdict == DOCK_VET_REJECT) {
            vet_close(v, closed ? "closed without a word" : n ? "other bytes" : "silent");
            continue;
        }
        elg_lock();
        const bool has = v->k->deck != NULL;
        elg_unlock();
        if (!has) { /* the deck went meanwhile: no empty dock */
            vet_close(v, NULL);
            continue;
        }
        elg_deck_accept(v->k, v->fd, v->peer); /* the deck's now: it takes over from the connection served */
        v->fd = -1;
    }
}

/* ---- listening (server task) ---- */

/* A connection to the dock's port: the deck's (the dock's own tasks). One that comes in the moment
 * after the deck went, before apply() closes the port, is closed: no empty dock. */
static void port_accept(dock_t *k)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    const int fd = accept(k->lp, (struct sockaddr *)&a, &al);
    if (fd < 0) return;
    char ip[16];
    inet_ntoa_r(a.sin_addr, ip, sizeof ip);
    elg_lock();
    const bool has = k->deck != NULL;
    elg_unlock();
    if (!has) {
        ESP_LOGI(TAG, "dock %d: %s turned away: no deck", k->k, ip);
        close(fd);
        return;
    }
    sock_opts(fd);
    vet_add(k, fd, ip); /* the deck only once it answers as a host does */
}

/* ---- server task ---- */

/* dt=215 (a Network Dock) on every announcement: Companion's discovery lists only primaries with native
 * TCP, so a deck announced by its own VID/PID would be dropped. */
static void withdraw(dock_t *k)
{
    esp_err_t e = mdns_service_remove_for_host(k->ann_name, MDNS_TYPE, MDNS_PROTO, NULL);
    if (e != ESP_OK) ESP_LOGW(TAG, "dock %d: mDNS withdraw: %s", k->k, esp_err_to_name(e));
    k->announced = false;
    k->ann_name[0] = k->ann_sn[0] = '\0';
}

/* A failed add is logged once per streak and tried again at the next apply. */
static void add(dock_t *k, const char *name, const char *sn)
{
    mdns_txt_item_t txt[2] = {{"dt", MDNS_DT}, {"sn", sn}};
    esp_err_t e = mdns_service_add(name, MDNS_TYPE, MDNS_PROTO, (uint16_t)ELGDOCK_PRIMARY_PORT(k->k), txt, 2);
    if (e != ESP_OK) {
        if (!k->ann_failing) ESP_LOGW(TAG, "dock %d: not announced: %s", k->k, esp_err_to_name(e));
        k->ann_failing = true;
        return;
    }
    k->ann_failing = false;
    k->announced = true;
    snprintf(k->ann_name, sizeof k->ann_name, "%.*s", (int)sizeof k->ann_name - 1, name);
    snprintf(k->ann_sn, sizeof k->ann_sn, "%.*s", (int)sizeof k->ann_sn - 1, sn);
    ESP_LOGI(TAG, "dock %d: announced as \"%s\" (sn %s)", k->k, name, sn);
}

/* Opens a dock's port. A failure is logged once per streak and retried LISTEN_RETRY_MS later. */
static int open_port(dock_t *k, uint16_t port, int64_t now)
{
    if (now < k->listen_retry_ms) return -1;
    int err = 0;
    const int fd = listen_on(port, &err);
    if (fd < 0) {
        if (!k->listen_failing) ESP_LOGE(TAG, "dock %d: can't listen on port %u (errno %d)", k->k, port, err);
        k->listen_failing = true;
        k->listen_retry_ms = now + LISTEN_RETRY_MS;
    } else if (k->listen_failing) {
        ESP_LOGI(TAG, "dock %d: port %u open again", k->k, port);
        k->listen_failing = false;
    }
    return fd;
}

/* Brings ports and announcements in line with the assignments: the wanted state and the steps
 * to it are dockproto's (docks_wanted, dock_reconcile). Every port change first, then every withdraw,
 * then every add, so two docks never hold one mDNS name. Server task only: its buffers are
 * static, off the task's stack. */
static void apply(void)
{
    static char box_name[sizeof elg.box_name];
    static char model[ELGDOCK_MAX][32], serial[ELGDOCK_MAX][32];
    static dock_in_t in[ELGDOCK_MAX];
    static dock_want_t want[ELGDOCK_MAX];
    dock_act_t act[ELGDOCK_MAX];
    elg_lock();
    const bool on = elg.announce;
    memcpy(box_name, elg.box_name, sizeof box_name);
    for (int i = 0; i < ELGDOCK_MAX; i++) {
        const dock_t *k = &elg.dock[i];
        memcpy(model[i], k->model, sizeof model[i]);
        memcpy(serial[i], k->deck_serial, sizeof serial[i]);
        in[i] = (dock_in_t){.has = k->deck != NULL, .model = model[i], .serial = serial[i]};
    }
    elg_unlock();
    dockproto_docks_wanted(in, ELGDOCK_MAX, on, box_name, want);
    const int64_t now = elg_now_ms();
    for (int i = 0; i < ELGDOCK_MAX; i++) {
        dock_t *k = &elg.dock[i];
        const dock_now_t cur = {.listening = k->lp >= 0, .announced = k->announced, .name = k->ann_name, .sn = k->ann_sn};
        act[i] = dockproto_dock_reconcile(&want[i], &cur);
        bool changed = false;
        if (act[i].open) {
            k->lp = open_port(k, (uint16_t)ELGDOCK_PRIMARY_PORT(i), now);
            changed = k->lp >= 0;
        }
        if (act[i].close) {
            close_fd(&k->lp); /* its connection already ended with elgdock_release */
            changed = true;
        }
        if (!want[i].listen) { /* deck-less: a later deck opens at once, and its first failure is logged */
            k->listen_retry_ms = 0;
            k->listen_failing = false;
            k->ann_failing = false;
        }
        if (changed) {
            elg_lock();
            k->listening = k->lp >= 0;
            elg_unlock();
            ESP_LOGI(TAG, "dock %d: %s port %d", i, k->lp >= 0 ? "listening on" : "no deck: closed", ELGDOCK_PRIMARY_PORT(i));
        }
    }
    for (int i = 0; i < ELGDOCK_MAX; i++)
        if (act[i].withdraw) withdraw(&elg.dock[i]);
    for (int i = 0; i < ELGDOCK_MAX; i++)
        if (act[i].add && elg.dock[i].lp >= 0) add(&elg.dock[i], want[i].name, want[i].sn);
}

static void srv_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < VET_MAX; i++) s_vet[i].fd = -1;
    for (;;) {
        apply();
        map_save(); /* after apply, not in it: their stack frames never add up */
        fd_set r;
        FD_ZERO(&r);
        int maxfd = -1;
        for (int i = 0; i < ELGDOCK_MAX; i++) {
            const int fd = elg.dock[i].lp;
            if (fd < 0) continue;
            FD_SET(fd, &r);
            if (fd > maxfd) maxfd = fd;
        }
        for (int i = 0; i < VET_MAX; i++) { /* the ones with nothing yet wake the task when it comes */
            const int fd = s_vet[i].fd;
            if (fd < 0 || s_vet[i].seen) continue;
            FD_SET(fd, &r);
            if (fd > maxfd) maxfd = fd;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = POLL_MS * 1000};
        const int n = maxfd >= 0 ? select(maxfd + 1, &r, NULL, NULL, &tv) : 0;
        if (maxfd < 0) vTaskDelay(pdMS_TO_TICKS(POLL_MS)); /* no deck: nothing to listen for */
        if (n < 0) FD_ZERO(&r);
        vet_check(&r); /* before new ones: a connection is vetted from its next round */
        for (int i = 0; i < ELGDOCK_MAX && n > 0; i++) {
            dock_t *k = &elg.dock[i];
            if (k->lp >= 0 && FD_ISSET(k->lp, &r)) port_accept(k);
        }
    }
}

/* ---- public API ---- */

esp_err_t elgdock_start(const char *box_name, const uint8_t mac[6], const elgdock_cb_t *cb, int ndocks)
{
    if (elg.srv || elg.mtx) return ESP_ERR_INVALID_STATE;
    if (!box_name || !mac || !cb) return ESP_ERR_INVALID_ARG;
    /* elg.mtx is published last, as dockmode_start does: set means every dock's mutex is there (every entry point
     * checks it); a failure before it deletes what was made and leaves elgdock unusable, never half set up. once
     * published it stays, every dock's mutex with it (a concurrent elgdock_status may hold it): a later failure leaves
     * the docks unusable (no server task, so no port opens) */
    SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
    bool ok = mtx != NULL;
    elg.cb = *cb;
    memcpy(elg.mac, mac, sizeof elg.mac);
    snprintf(elg.box_name, sizeof elg.box_name, "%s", box_name);
    elg.announce = true;
    elg.ndocks = dockproto_ndocks(ndocks, ELGDOCK_MAX);
    elg.gesture = corner_cfg_default(); /* the default until elgdock_set_gesture */
    for (int i = 0; i < ELGDOCK_MAX; i++) {
        dock_t *k = &elg.dock[i];
        k->k = i;
        k->lp = k->cs = k->cs_new = -1;
        k->usb_mtx = xSemaphoreCreateMutex();
        k->tx_mtx = xSemaphoreCreateMutex();
        k->cs_mtx = xSemaphoreCreateMutex();
        k->in_mtx = xSemaphoreCreateMutex();
        ok = ok && k->usb_mtx && k->tx_mtx && k->cs_mtx && k->in_mtx;
    }
    if (ok) {
        map_load();
        elg.mtx = mtx; /* last: the server task and every entry point lock it */
        if (xTaskCreate(srv_task, "elgdock", SRV_STACK, NULL, SRV_PRIO, &elg.srv) != pdPASS) return ESP_ERR_NO_MEM;
    }
    if (!ok) {
        for (int i = 0; i < ELGDOCK_MAX; i++) {
            dock_t *k = &elg.dock[i];
            SemaphoreHandle_t *m[] = {&k->usb_mtx, &k->tx_mtx, &k->cs_mtx, &k->in_mtx};
            for (size_t j = 0; j < sizeof m / sizeof m[0]; j++)
                if (*m[j]) {
                    vSemaphoreDelete(*m[j]);
                    *m[j] = NULL;
                }
        }
        if (mtx) vSemaphoreDelete(mtx);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Network Dock mode: %d docks, each deck on its dock's port, %d..%d, open while the deck is here",
             elg.ndocks, ELGDOCK_PRIMARY_PORT(0), ELGDOCK_PRIMARY_PORT(elg.ndocks - 1));
    return ESP_OK;
}

/* usbdeck's stand-in for a deck whose serial couldn't be read is no identity to keep a dock for. */
static bool sticky_serial(const char *serial) { return serial[0] && strncmp(serial, "unknown-", 8) != 0; }

int elgdock_assign(usbdeck_t *d, const deck_model_t *m, const char *serial)
{
    if (!elg.mtx || !d || !m || !serial) return -1;
    elg_lock();
    int at = -1;
    for (int i = 0; i < ELGDOCK_MAX && at < 0; i++)
        if (elg.dock[i].deck == d) at = i; /* already has one */
    if (at < 0) { /* its dock before, else one nobody reserved, else an absent deck's */
        bool busy[ELGDOCK_MAX];
        const char *map[ELGDOCK_MAX];
        for (int i = 0; i < ELGDOCK_MAX; i++) {
            busy[i] = elg.dock[i].deck != NULL;
            map[i] = elg.map[i];
        }
        at = dockproto_pick_dock(busy, map, elg.ndocks, serial, sticky_serial(serial)); /* the docks in use */
    }
    if (at >= 0 && elg.dock[at].deck != d) {
        dock_t *k = &elg.dock[at];
        k->deck = d;
        k->vid = DECK_VID_ELGATO;
        k->pid = m->pid;
        snprintf(k->deck_serial, sizeof k->deck_serial, "%s", serial);
        snprintf(k->model, sizeof k->model, "%s", m->name);
        snprintf(k->deck_fw, sizeof k->deck_fw, "%s", usbdeck_firmware(d));
        xSemaphoreTake(k->in_mtx, portMAX_DELAY); /* a new deck: nothing pressed known; held if it is the one */
        const char *held[ELGDOCK_MAX];
        for (int h = 0; h < elg.nhold; h++) held[h] = elg.hold[h];
        ingate_init(&k->gate, m->gen == DECK_GEN2, !m->pedal && m->fmt != DECK_IMG_NONE,
                    ingate_held(serial, held, elg.nhold));
        corner_gate_init(&k->corner, m, &elg.gesture);
        xSemaphoreGive(k->in_mtx);
        if (sticky_serial(serial) && strcmp(elg.map[at], serial) != 0) {
            for (int i = 0; i < ELGDOCK_MAX; i++)
                if (strcmp(elg.map[i], serial) == 0) elg.map[i][0] = '\0'; /* one dock per deck */
            snprintf(elg.map[at], sizeof elg.map[at], "%s", serial);
            elg.map_dirty = true;
        }
    }
    elg_unlock();
    if (at < 0)
        ESP_LOGW(TAG, "no free dock for Stream Deck %s", serial);
    else
        ESP_LOGI(TAG, "dock %d (port %d): %s %s", at, ELGDOCK_PRIMARY_PORT(at), m->name, serial);
    return at;
}

int elgdock_dock_of(const usbdeck_t *d)
{
    if (!elg.mtx || !d) return -1;
    int at = -1;
    elg_lock();
    for (int i = 0; i < ELGDOCK_MAX && at < 0; i++)
        if (elg.dock[i].deck == d) at = i;
    elg_unlock();
    return at;
}

void elgdock_release(usbdeck_t *d)
{
    if (!elg.mtx || !d) return;
    dock_t *k = NULL;
    elg_lock();
    for (int i = 0; i < ELGDOCK_MAX && !k; i++)
        if (elg.dock[i].deck == d) k = &elg.dock[i];
    elg_unlock();
    if (!k) return;
    xSemaphoreTake(k->usb_mtx, portMAX_DELAY); /* dockrxK has left every call with d */
    elg_lock();
    k->deck = NULL;
    k->deck_serial[0] = k->model[0] = k->deck_fw[0] = '\0';
    elg_unlock();
    xSemaphoreGive(k->usb_mtx);
    elg_deck_stop(k); /* the server task then closes the port and withdraws it */
    ESP_LOGI(TAG, "dock %d: deck gone", k->k);
}

int elgdock_status(elgdock_status_t *out, int max)
{
    if (!elg.mtx || !out || max <= 0) return 0;
    int n = 0;
    elg_lock();
    for (int i = 0; i < elg.ndocks && n < max; i++, n++) {
        const dock_t *k = &elg.dock[i];
        elgdock_status_t *o = &out[n];
        memset(o, 0, sizeof *o);
        o->dock = i;
        o->port = (uint16_t)ELGDOCK_PRIMARY_PORT(i);
        o->listening = k->listening;
        snprintf(o->serial, sizeof o->serial, "%s", k->deck_serial);
        snprintf(o->model, sizeof o->model, "%s", k->model);
        o->companion = k->companion;
        snprintf(o->peer, sizeof o->peer, "%s", k->peer);
    }
    elg_unlock();
    return n;
}

void elgdock_set_announce(const char *box_name, bool on)
{
    if (!elg.mtx) return;
    elg_lock();
    if (box_name) snprintf(elg.box_name, sizeof elg.box_name, "%s", box_name);
    elg.announce = on;
    elg_unlock();
}
