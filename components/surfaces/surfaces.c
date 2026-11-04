/* ESP: Stream Decks <-> Companion wiring, image pipeline, the box's maintenance screens.
 *
 * Who runs what:
 *  - satclient task: Companion callbacks. Routing and copying only (the key-state strings live
 *    only for the call): the base64 text is copied into a job for the image worker.
 *  - deck tasks: on_attach / on_detach update the registry (cheap) and post an event; on_input
 *    translates and forwards input (or posts it to the surfaces task for the maintenance screens);
 *    on_restored (a deck's quarantine is over) posts a redraw.
 *    on_attach, on_detach and on_restored also drop the deck's quarantine problem: each of them
 *    ends what it said (usbdeck raises it again right after on_attach if the deck is still
 *    quarantined). usb_client task: on_problem (problem ring, problems.c). A deck plugged in or
 *    removed and every key-down Companion gets (Satellite: a press sent; dock mode: a press in the
 *    report passed through to Companion) sound (components/feedback: queued, never waited for).
 *  - "surfaces" task: ADD-DEVICE / REMOVE-DEVICE, the maintenance screens (deckui: the snapshot, the
 *    navigation per deck, the keys whose fingerprint changed; a 50 ms tick while any is shown),
 *    Companion-lost timers, custom message timer (1 s tick), the message's pages (3 s).
 *  - "imgwork" task: base64 decode, size check, transform, JPEG/BMP, USB write. The only caller of
 *    img_jpeg_encode.
 *
 * What the LCD keys show, highest priority first: the maintenance screen on demand, the custom
 * message (surfaces_show_message), the maintenance screens (deckui: 30 s after
 * Companion is lost, or while it never was there), the last images dimmed with "Reconnecting" over
 * them (2 s after the loss), Companion's images. Strip cells, the info bar and colour keys never show
 * the message; the maintenance screens draw their status on the strip and the info bar. While a message
 * is active (and nothing is on demand) imgwork draws it for every job on an LCD key: a Companion job
 * only updates the cached image, and also draws the message when it took over a pending box-drawn
 * frame (job flag `msg`). So Companion's images are never lost to the message and come back from the
 * cache when it ends. On demand over a deck Companion drives (`ui_od`), Companion's images go straight
 * to the cache (on the satclient task; dock mode: dockmode's cache holds them) and come back the same
 * way when it closes.
 *
 * The message is laid out one word per key (statusui); a deck with too few keys for it shows
 * it in pages, the next every sui_page_us (3 s, 2 s past 3 pages). "Reconnecting" over the dimmed
 * images is drawn on every key, the same on each: it never pages. The maintenance screens page
 * by themselves (deckui's navigation).
 *
 * Network Dock mode (surfaces_start_dock): no satclient; dockmode.c gives every deck a
 * dock and an image cache. While Companion has a deck's dock connection the deck is `owned`: the box
 * queues nothing for it but the custom message's frames and the maintenance screen on demand, which
 * imgwork hands to dockmode (it writes them between Companion's images, holds Companion's images back
 * meanwhile and writes them back from the cache when they end); elgdock forwards Companion's traffic.
 * When Companion lets go, or never was there, the deck shows the maintenance screens.
 *
 * One mutex guards the registry, coalescer, cached images and shared flags. It is never held
 * across satclient_*, usbdeck_* or another module's callbacks. A second, inmtx, serialises the
 * Satellite key presses sent to Companion with the key-ups the box sends for them when the screen on
 * demand opens (satclient_key never blocks); on_input takes mtx inside it, never the other way. A deck
 * handle is used outside the
 * lock only after being published in surf.inuse[]; on_detach removes the deck from the registry and
 * waits until nobody holds its handle, because usbdeck frees it as soon as on_detach returns. */
#include "surfaces/surfaces.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "deckmodel/manifest.h"
#include "deckui/deckui.h"
#include "deckui/nav.h"
#include "deckui/screens.h"
#include "elgdock/elgdock.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "feedback/feedback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imgproc/imgproc.h"
#include "imgproc/jpeg_enc.h"
#include "imgproc/jpeg_dec.h"
#include "imgproc/png.h"
#include "maintmap.h"
#include "netmgr/netmgr.h"
#include "satclient/satclient.h"
#include "satproto/b64.h"
#include "slotsync/slotsync.h"
#include "problems.h"
#include "statusui/statusui.h"
#include "surfaces_int.h"
#include "surfaces_priv.h"
#include "dockcache.h"
#include "usbdeck/usbdeck.h"

#define TAG "surfaces"

#define SURF_STACK 8192
#define SURF_PRIO 5
#define IMG_STACK 8192
#define IMG_PRIO 4
#define EVQ_DEPTH 48 /* attach / detach events and the maintenance screens' input */
#define TICK_MS 1000
#define DIM_AFTER_US SURF_LOSS_DIM_US
#define DIM_PCT SURF_DIM_PCT
#define RETRY_US (20 * 1000LL)           /* deck queue full: try that deck again this much later */
#define IMG_MAX_BYTES (256 * 128 * 3)    /* largest control: Plus LCD cell 200x100; keys <= 120x120 */
#define BMP_MAX_BYTES (IMG_MAX_BYTES + 1024)
#define PROBLEM_LEN SURF_PROBLEM_LEN /* usbdeck's texts fit whole */
_Static_assert(PROBLEM_LEN == 96, "surfaces_problem_list() hands out char[96] entries (surfaces.h)");
#define PAGE_LABEL "Horizontal Swipe Changes Page"
#define UI_TICK_MS 50          /* the maintenance screens: holds, hints, paging, animation (5 fps) */


typedef enum { EV_ATTACHED, EV_DETACHED, EV_RESTORED, EV_POKE, EV_INPUT, EV_PIN } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    int slot;
    uint32_t gen;
    void *deck;
    bool reused;    /* EV_ATTACHED: device id was still registered (replug before detach) */
    bool was_added; /* EV_DETACHED: REMOVE-DEVICE needed */
    deck_event_t in; /* EV_INPUT: the deck's input, for its maintenance screen */
    int pin_result;  /* EV_PIN: SURF_PIN_*, for the check gen (its ticket) */
    int wait_s;
    char device_id[48];
} surf_ev_t;

typedef struct {
    const deck_model_t *m;
    deck_ctl_t ctl;
    int idx;
    uint32_t gen;
} target_t;

surf_t surf; /* the module's state (surfaces_int.h) */

static void lock(void) { xSemaphoreTake(surf.mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(surf.mtx); }
void surf_lock(void) { lock(); }
void surf_unlock(void) { unlock(); }

void surf_od_cands_locked(surf_od_cand_t *c)
{
    for (int i = 0; i < SURF_MAX_DECKS; i++) {
        const surf_slot_t *sl = &surf.reg.slot[i];
        c[i] = (surf_od_cand_t){.model = sl->device_id[0] ? sl->model : NULL, .device_id = sl->device_id,
                                .seq = surf.att_seq[i]};
    }
}

bool surf_od_slot_locked(int slot) { return surf_od_here(&surf.od, surf.reg.slot[slot].device_id); }

/* mtx held: opens (on) or closes the box menu on device_id; true when that changed something. */
static bool od_set_locked(const char *device_id, bool on)
{
    const bool changed = on ? surf_od_add(&surf.od, device_id) : surf_od_del(&surf.od, device_id);
    atomic_store(&surf.ondemand, surf.od.n > 0); /* after the set: on_input reads them together under the lock */
    return changed;
}

static bool ensure_mutex(void)
{
    if (!surf.mtx) surf.mtx = xSemaphoreCreateMutex();
    return surf.mtx != NULL;
}


static bool is_image_ctl(deck_ctl_kind_t k) { return k == CTL_KEY_LCD || k == CTL_LCD_CELL || k == CTL_INFO; }
bool surf_has_display(const deck_model_t *m) { return deck_model_has_display(m); }
bool surf_is_image_ctl(deck_ctl_kind_t k) { return is_image_ctl(k); }

/* ---- problems ---- */

static void problem_add(const char *msg)
{
    if (!msg || !msg[0] || !surf.mtx) return;
    lock();
    surf_problems_add(&surf.problems, msg);
    unlock();
}

/* The deck's quarantine ended (or it is gone, or attaches anew): its quarantine text is obsolete. */
static void problem_drop_quarantine(const char *serial)
{
    if (!surf.mtx) return;
    lock();
    surf_problems_drop_quarantine(&surf.problems, serial);
    unlock();
}

/* ---- jobs ---- */

static job_t *job_new(job_kind_t kind, size_t len, uint32_t gen, bool local)
{
    job_t *j = heap_caps_malloc(sizeof *j + len, MALLOC_CAP_SPIRAM);
    if (!j) {
        ESP_LOGW(TAG, "out of memory for an image job (%u bytes)", (unsigned)len);
        return NULL;
    }
    memset(j, 0, sizeof *j);
    j->kind = (uint8_t)kind;
    j->local = local;
    j->pct = 100;
    j->gen = gen;
    j->len = len;
    return j;
}

static void job_free(void *p) { heap_caps_free(p); }

static void kick_imgwork(void)
{
    if (surf.img_task) xTaskNotifyGive(surf.img_task);
}

static job_t *last_job(uint32_t gen, uint8_t pct, bool overlay)
{
    job_t *j = job_new(JOB_LAST, 0, gen, true);
    if (j) {
        j->pct = pct;
        j->overlay = overlay;
    }
    return j;
}

static void cache_job(int slot, int idx, const job_t *j, deck_ctl_kind_t kind, int w, int h);

/* Companion image: replaces whatever is pending for the control. A box-drawn frame it replaces
 * would have shown the active message, so then this job draws the message too. Under the screen on
 * demand (it may have opened since the caller looked) it goes to the cache only. */
static void put_companion(int slot, int idx, job_t *j)
{
    void *drop = j;
    bool cache = false;
    deck_ctl_kind_t kind = CTL_KEY_NONE;
    int w = 0, h = 0;
    lock();
    if (surf.gen[slot] == j->gen && surf.ui_od[slot] && idx < surf.reg.slot[slot].nctl) {
        const deck_ctl_t *c = &surf.reg.slot[slot].ctls[idx];
        cache = true;
        kind = c->kind;
        w = c->w;
        h = c->h;
    } else if (surf.gen[slot] == j->gen) {
        const job_t *p = surf.co.pending[slot][idx];
        if (p && (p->local || p->msg)) j->msg = true;
        drop = coalesce_put(&surf.co, slot, idx, j);
    }
    unlock();
    if (cache) cache_job(slot, idx, j, kind, w, h);
    if (drop) job_free(drop);
}

/* Box-drawn image: never replaces a pending Companion image; offline_only also refuses it while
 * Companion drives the decks. Returns true when queued. */
static bool put_local(int slot, int idx, job_t *j, bool offline_only)
{
    void *drop = j;
    lock();
    if (surf.gen[slot] == j->gen && !(offline_only && surf.live) && !surf.owned[slot]) {
        job_t *p = surf.co.pending[slot][idx];
        if (!p || p->local) drop = coalesce_put(&surf.co, slot, idx, j);
    }
    unlock();
    if (drop) job_free(drop);
    return drop != j;
}

/* Message frame for an LCD key; unlike other box-drawn frames it is queued while Companion is live,
 * and in dock mode for a deck Companion drives (imgwork hands it to dockmode then).
 * A pending Companion job stays (its image must reach the cache) and draws the message instead.
 * Returns true when the message will be drawn. */
static bool put_msg(int slot, int idx, uint32_t gen)
{
    job_t *j = job_new(JOB_MSG, 0, gen, true);
    if (!j) return false;
    void *drop = j;
    bool ok = false;
    lock();
    if (surf.gen[slot] == gen && (!surf.owned[slot] || surf.dock)) {
        job_t *p = surf.co.pending[slot][idx];
        if (p && !p->local)
            p->msg = true;
        else
            drop = coalesce_put(&surf.co, slot, idx, j);
        ok = true;
    }
    unlock();
    if (drop) job_free(drop);
    return ok;
}

/* After ESP_ERR_TIMEOUT: keep the job unless something newer is pending (which then takes over
 * drawing the message, if this job would have). */
static bool requeue(int slot, int idx, job_t *j)
{
    bool kept = false;
    lock();
    job_t *p = surf.gen[slot] == j->gen ? surf.co.pending[slot][idx] : NULL;
    if (surf.gen[slot] == j->gen && !p) {
        coalesce_put(&surf.co, slot, idx, j);
        kept = true;
    } else if (p && (j->local || j->msg)) {
        p->msg = true;
    }
    unlock();
    if (!kept) job_free(j);
    return kept;
}

job_t *surf_job_new(job_kind_t kind, size_t len, uint32_t gen, bool local) { return job_new(kind, len, gen, local); }
void surf_job_free(void *p) { job_free(p); }
job_t *surf_last_job(uint32_t gen, uint8_t pct, bool overlay) { return last_job(gen, pct, overlay); }
bool surf_put_local(int slot, int idx, job_t *j, bool offline_only) { return put_local(slot, idx, j, offline_only); }
void surf_kick_imgwork(void) { kick_imgwork(); }

/* ---- deck handles outside the lock ---- */

/* owned (may be NULL): dock mode, Companion drives the deck (read with the handle). */
static usbdeck_t *deck_acquire(int user, int slot, uint32_t gen, bool *owned)
{
    lock();
    usbdeck_t *d = surf.gen[slot] == gen ? surf.reg.slot[slot].deck : NULL;
    surf.inuse[user] = d;
    if (owned) *owned = surf.owned[slot];
    unlock();
    return d;
}

static void deck_release(int user)
{
    lock();
    surf.inuse[user] = NULL;
    unlock();
}

usbdeck_t *surf_deck_acquire(int user, int slot, uint32_t gen, bool *owned) { return deck_acquire(user, slot, gen, owned); }
void surf_deck_release(int user) { deck_release(user); }

static bool inuse_locked(const usbdeck_t *d)
{
    for (int i = 0; i < USER_N; i++)
        if (surf.inuse[i] == d) return true;
    return false;
}

static void free_last_locked(int slot)
{
    for (int c = 0; c < SURF_MAX_CTLS; c++) {
        heap_caps_free(surf.last[slot][c]);
        surf.last[slot][c] = NULL;
    }
}

/* ---- events to the surfaces task ---- */

static void post_event(const surf_ev_t *ev)
{
    if (!surf.evq) return;
    /* deck tasks may wait; never dropped. The surfaces task itself never waits on its own queue (it is
     * the one reader: a full queue would never drain): from it, a full queue drops, logged. */
    const bool self = surf.surf_task && xTaskGetCurrentTaskHandle() == surf.surf_task;
    if (xQueueSend(surf.evq, ev, self ? 0 : portMAX_DELAY) != pdTRUE)
        ESP_LOGW(TAG, "event %d for slot %d dropped: posted by the surfaces task to its own full queue",
                 (int)ev->kind, ev->slot);
}

/* An encoder turn or a strip swipe for a maintenance screen: dropped when the queue is full, so a
 * spinning dial never stalls its deck's task (keys and pushes stay lossless: post_event). */
static void post_event_lossy(const surf_ev_t *ev)
{
    if (surf.evq) xQueueSend(surf.evq, ev, 0);
}

/* Asks the surfaces task to re-check state now (it also does so every second). */
static void poke(void)
{
    atomic_store(&surf.snap_dirty, true); /* the maintenance screens' snapshot too */
    if (!surf.evq || atomic_exchange(&surf.poke_pending, true)) return;
    surf_ev_t ev = {.kind = EV_POKE};
    if (xQueueSend(surf.evq, &ev, 0) != pdTRUE) atomic_store(&surf.poke_pending, false);
}

/* ---- satclient callbacks (satclient task) ---- */

static void on_ready(void *ctx, const sat_caps_t *caps)
{
    (void)ctx;
    lock();
    surf.live = true;
    surf.ready_gen++;
    surf.too_old = false;
    surf.sat_lost = false;
    unlock();
    ESP_LOGI(TAG, "Companion %s ready (API %d.%d.%d), %s images", caps->companion_version, caps->major, caps->minor,
             caps->patch, caps->png ? "PNG" : "raw RGB");
    poke();
}

static void store_last_gen(int slot, int idx, uint32_t gen, uint8_t *img);

/* Companion's image (KEY-STATE's BITMAP: raw RGB in base64, or a PNG data URL with BITMAP_FORMAT=png) into img,
 * w x h RGB (bytes). False when it doesn't decode to exactly that. A PNG is decoded through a scratch copy of its
 * bytes in PSRAM. */
static bool decode_bitmap(const char *v, size_t len, int w, int h, uint8_t *img, size_t bytes)
{
    const char *b64;
    size_t n, got = 0;
    switch (sat_bitmap_split(v, len, &b64, &n)) {
    case SAT_BITMAP_RGB:
        return sat_b64_decode(b64, n, img, bytes, &got) && surf_bitmap_ok(bytes, got);
    case SAT_BITMAP_PNG: {
        const size_t cap = n / 4 * 3 + 3;
        uint8_t *png = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!png) return false;
        const bool ok = sat_b64_decode(b64, n, png, cap, &got) && img_png_decode_rgb(png, got, w, h, img);
        heap_caps_free(png);
        return ok;
    }
    default:
        return false;
    }
}

/* On the satclient task, while the screen on demand covers the deck: Companion's image straight into the
 * cache it comes back from (one key's image, about 30 KB at most once decoded). */
static void cache_companion(int slot, int idx, uint32_t gen, const char *v, size_t len, int w, int h)
{
    const size_t bytes = (size_t)w * h * 3;
    uint8_t *img = bytes ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM) : NULL;
    if (!img) return;
    if (!decode_bitmap(v, len, w, h, img, bytes)) {
        heap_caps_free(img);
        return;
    }
    store_last_gen(slot, idx, gen, img);
}

/* A Companion job that came under the screen on demand: what it says, into the cache only. */
static void cache_job(int slot, int idx, const job_t *j, deck_ctl_kind_t kind, int w, int h)
{
    if (kind == CTL_KEY_RGB) {
        if (j->kind != JOB_SOLID) return;
        lock();
        surf.rgb[slot][idx][0] = j->r;
        surf.rgb[slot][idx][1] = j->g;
        surf.rgb[slot][idx][2] = j->b;
        surf.rgb_set[slot][idx] = true;
        unlock();
    } else if (j->kind == JOB_B64) {
        cache_companion(slot, idx, j->gen, (const char *)j->data, j->len, w, h);
    } else if (j->kind == JOB_SOLID) {
        store_last_gen(slot, idx, j->gen, NULL); /* cleared: black when it comes back */
    }
}

static void on_too_old(void *ctx, const char *api_version)
{
    (void)ctx;
    char host[64], msg[PROBLEM_LEN];
    sat_caps_t caps;
    satclient_get_target(host, sizeof host, NULL);
    satclient_caps(&caps);
    lock();
    surf.too_old = true;
    snprintf(surf.too_old_host, sizeof surf.too_old_host, "%s", host);
    unlock();
    snprintf(msg, sizeof msg, "Companion %s (API %s) is too old: version 5.0 or newer is needed",
             caps.companion_version[0] ? caps.companion_version : "?", api_version);
    problem_add(msg);
    poke();
}

static void on_key_state(void *ctx, const sat_key_state_t *ks)
{
    (void)ctx;
    static bool warned;
    int slot = -1, idx = 0;
    const deck_ctl_t *ctl = NULL;
    size_t expected = 0;
    uint32_t gen = 0;
    deck_ctl_kind_t kind = CTL_KEY_NONE;
    int cw = 0, ch = 0;

    bool cache_only = false;
    lock();
    int rc = surf_route_keystate(&surf.reg, ks, &slot, &ctl, &expected);
    if (rc == 0) {
        idx = (int)(ctl - surf.reg.slot[slot].ctls);
        gen = surf.gen[slot];
        kind = ctl->kind;
        cw = ctl->w;
        ch = ctl->h;
        cache_only = surf.ui_od[slot];
    }
    unlock();
    if (rc != 0) return; /* unknown (e.g. just unplugged) device or control: drop */
    if (kind == CTL_KEY_RGB && ks->has_color) { /* a colour key's colour: kept, to bring it back after on demand */
        lock();
        if (surf.gen[slot] == gen) {
            surf.rgb[slot][idx][0] = ks->r;
            surf.rgb[slot][idx][1] = ks->g;
            surf.rgb[slot][idx][2] = ks->b;
            surf.rgb_set[slot][idx] = true;
        }
        unlock();
    }
    if (cache_only) { /* the screen on demand is over this deck: the image only to the cache */
        if (kind != CTL_KEY_RGB && surf_bitmap_text_ok(ks->bitmap_b64, ks->bitmap_b64_len, expected))
            cache_companion(slot, idx, gen, ks->bitmap_b64, ks->bitmap_b64_len, cw, ch);
        return;
    }

    job_t *j;
    if (kind == CTL_KEY_RGB) {
        if (!ks->has_color || !(j = job_new(JOB_SOLID, 0, gen, false))) return;
        j->r = ks->r;
        j->g = ks->g;
        j->b = ks->b;
    } else {
        if (!ks->bitmap_b64) return;
        /* raw RGB of exactly w*h*3 bytes in base64, or a PNG data URL; anything else can't be drawn */
        if (!surf_bitmap_text_ok(ks->bitmap_b64, ks->bitmap_b64_len, expected)) {
            if (!warned) ESP_LOGW(TAG, "%s %s: bitmap of the wrong size dropped", ks->device_id, ks->control_id);
            warned = true;
            return;
        }
        if (!(j = job_new(JOB_B64, ks->bitmap_b64_len, gen, false))) return;
        memcpy(j->data, ks->bitmap_b64, ks->bitmap_b64_len);
    }
    put_companion(slot, idx, j);
    kick_imgwork();
}

static void on_clear(void *ctx, const char *device_id)
{
    (void)ctx;
    uint8_t kinds[SURF_MAX_CTLS];
    int n = 0;
    uint32_t gen = 0;
    lock();
    int slot = surfreg_find(&surf.reg, device_id);
    /* the colour keys black too, also under the screen on demand: restore_last brings these back */
    if (slot >= 0) memset(surf.rgb[slot], 0, sizeof surf.rgb[slot]);
    if (slot >= 0 && surf.ui_od[slot]) { /* the screen on demand covers it: the cache only (black) */
        free_last_locked(slot);
        slot = -1;
    }
    if (slot >= 0) {
        gen = surf.gen[slot];
        n = surf.reg.slot[slot].nctl;
        for (int c = 0; c < n; c++) kinds[c] = (uint8_t)surf.reg.slot[slot].ctls[c].kind;
    }
    unlock();
    for (int c = 0; c < n; c++) {
        if (!is_image_ctl((deck_ctl_kind_t)kinds[c]) && kinds[c] != CTL_KEY_RGB) continue;
        job_t *j = job_new(JOB_SOLID, 0, gen, false); /* black */
        if (j) put_companion(slot, c, j);
    }
    kick_imgwork();
}

static void on_brightness(void *ctx, const char *device_id, int pct)
{
    (void)ctx;
    lock();
    int slot = surfreg_find(&surf.reg, device_id);
    usbdeck_t *d = slot >= 0 && !surf.reg.slot[slot].model->pedal ? surf.reg.slot[slot].deck : NULL;
    if (slot >= 0) surf.comp_bright[slot] = (int16_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct); /* for the hand-back */
    if (slot >= 0 && surf_bright_comp_path(surf.ui_od[slot]) == SURF_BRIGHT_KEEP) d = NULL; /* covered: kept */
    surf.inuse[USER_SAT] = d;
    unlock();
    if (!d) return;
    esp_err_t err = usbdeck_set_brightness(d, (uint8_t)pct);
    deck_release(USER_SAT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "%s: brightness not set: %s", device_id, esp_err_to_name(err));
}

static void on_add_result(void *ctx, const char *device_id, bool ok, const char *message)
{
    (void)ctx;
    if (ok) {
        ESP_LOGI(TAG, "Companion added %s", device_id);
        return;
    }
    lock();
    int slot = surfreg_find(&surf.reg, device_id);
    if (slot >= 0) surf.reg.slot[slot].added = false; /* retried on the next READY */
    unlock();
    char msg[PROBLEM_LEN];
    snprintf(msg, sizeof msg, "Companion refused %s: %s", device_id, message ? message : "");
    problem_add(msg);
}

/* the slot handle d is registered in, -1 none: the last match, as the loops it replaces (mtx held). */
static int slot_of_locked(const usbdeck_t *d)
{
    int slot = -1;
    for (int i = 0; i < SURF_MAX_DECKS; i++)
        if (surf.reg.slot[i].device_id[0] && surf.reg.slot[i].deck == d) slot = i;
    return slot;
}

/* ---- usbdeck callbacks (deck tasks / usb client task) ---- */

static void on_attach(usbdeck_t *d, const deck_model_t *m, const char *serial)
{
    problem_drop_quarantine(serial); /* raised again after this returns if it still applies */
    surf_ev_t ev = {.kind = EV_ATTACHED, .deck = d};
    snprintf(ev.device_id, sizeof ev.device_id, DEVICE_ID_PREFIX "%s", serial);
    lock();
    int prev = surfreg_find(&surf.reg, ev.device_id);
    bool same_model = prev >= 0 && surf.reg.slot[prev].model == m;
    int slot = surfreg_attach(&surf.reg, ev.device_id, d, m);
    if (slot >= 0 && !same_model) { /* a different deck in this slot: older jobs and images are void */
        surf.gen[slot]++;
        coalesce_drop_slot(&surf.co, slot, job_free);
        free_last_locked(slot);
    }
    if (slot >= 0) {
        surf.lost[slot] = false;
        /* a handle that has just attached owns no dock connection yet (dockmode_attach runs after this): a deck
         * re-attached before its old detach must not keep the old handle's */
        surf.owned[slot] = false;
        surf.peer[slot][0] = '\0';
        surf.att_seq[slot] = ++surf.att_next; /* the earliest attached wins a tie */
        if (!same_model) surf.comp_bright[slot] = -1; /* another deck: Companion's brightness unknown */
    }
    ev.slot = slot;
    ev.gen = slot >= 0 ? surf.gen[slot] : 0;
    ev.reused = prev >= 0;
    /* a replugged deck comes back closed, even when its new attach is handled before the old detach */
    const bool od_was = prev >= 0 && od_set_locked(ev.device_id, false);
    unlock();
    if (od_was) ESP_LOGI(TAG, "box menu closed on %s: its deck was replugged", ev.device_id);
    feedback_event(FB_EV_DECK_IN);
    if (slot < 0) { /* over the deck limit: ignored (no dock, no screen, never Companion's) until unplugged */
        lock();
        bool known = false;
        for (int i = 0; i < surf.nignored; i++) known |= surf.ignored[i] == d;
        if (!known && surf.nignored < SURF_MAX_DECKS) {
            surf.ignored[surf.nignored] = d;
            snprintf(surf.ignored_sn[surf.nignored], sizeof surf.ignored_sn[0], "%s", serial);
            surf.nignored++;
        }
        unlock();
        char msg[PROBLEM_LEN];
        surf_problem_too_many(msg, sizeof msg, surf.limit, serial);
        problem_add(msg);
        return;
    }
    if (prev < 0 && surf.inmtx) { /* a fresh slot: nothing in it known pressed to Companion (a same-deck
                                    * re-attach keeps its own; a session's end or start clears them all) */
        xSemaphoreTake(surf.inmtx, portMAX_DELAY);
        surf.nsat_down[slot] = 0;
        xSemaphoreGive(surf.inmtx);
    }
    if (surf.dock) dockmode_attach(d, m, serial); /* before the status screen, which shows its port */
    ESP_LOGI(TAG, "%s (%s) in slot %d%s", ev.device_id, m->name, slot, ev.reused ? ", replugged" : "");
    post_event(&ev);
}

static void on_detach(usbdeck_t *d)
{
    surf_ev_t ev = {.kind = EV_DETACHED, .deck = d, .slot = -1};
    bool od_was = false;
    surf_dock_snap_t *dsnap = NULL;
    lock(); /* an ignored deck unplugged is ignored no more */
    for (int i = 0; i < surf.nignored; i++)
        if (surf.ignored[i] == d) {
            surf.nignored--;
            surf.ignored[i] = surf.ignored[surf.nignored];
            memcpy(surf.ignored_sn[i], surf.ignored_sn[surf.nignored], sizeof surf.ignored_sn[0]);
            break;
        }
    unlock();
    if (surf.dock) dockmode_detach(d); /* first: elgdock stops using d */
    problem_drop_quarantine(usbdeck_serial(d)); /* gone (raised again at its re-attach) */
    lock();
    ev.slot = slot_of_locked(d);
    if (ev.slot >= 0) {
        memcpy(ev.device_id, surf.reg.slot[ev.slot].device_id, sizeof ev.device_id);
        ev.was_added = surf.reg.slot[ev.slot].added;
    }
    if (ev.slot >= 0) {
        surfreg_detach(&surf.reg, d);
        coalesce_drop_slot(&surf.co, ev.slot, job_free);
        free_last_locked(ev.slot);
        surf.gen[ev.slot]++;
        surf.owned[ev.slot] = false;
        surf.lost[ev.slot] = false;
        dsnap = surf.dsnap[ev.slot]; /* freed below, outside the lock */
        surf.dsnap[ev.slot] = NULL;
        surf.dlost_at_us[ev.slot] = 0;
        surf.ddimmed[ev.slot] = false;
        surf.keeping[ev.slot] = surf.woke[ev.slot] = false;
        surf.ui[ev.slot] = surf.ui_od[ev.slot] = surf.handback[ev.slot] = false;
        memset(surf.rgb_set[ev.slot], 0, sizeof surf.rgb_set[ev.slot]);
        surf.comp_bright[ev.slot] = -1;
        od_was = od_set_locked(ev.device_id, false); /* closed on it; replugged, it comes back closed */
    }
    while (inuse_locked(d)) { /* a usbdeck call with this handle is in flight */
        unlock();
        vTaskDelay(1);
        lock();
    }
    unlock();
    surf_dock_snap_free(dsnap);
    if (od_was) ESP_LOGI(TAG, "box menu closed on %s: its deck was unplugged", ev.device_id);
    feedback_event(FB_EV_DECK_OUT);
    if (ev.slot >= 0) post_event(&ev);
}

/* Satellite, inmtx held: Companion now knows control id is pressed (or not any more). */
static void sat_down_note(int slot, const char *id, bool pressed)
{
    uint8_t *n = &surf.nsat_down[slot];
    for (int i = 0; i < *n; i++)
        if (!strcmp(surf.sat_down[slot][i], id)) {
            if (!pressed) memmove(surf.sat_down[slot][i], surf.sat_down[slot][i + 1], (size_t)(*n - i - 1) * 8);
            if (!pressed) (*n)--;
            return;
        }
    if (pressed && *n < SAT_DOWN_MAX) strlcpy(surf.sat_down[slot][(*n)++], id, 8); /* deck task: no snprintf */
}

/* A deck's input: to Companion when it drives the deck, to the deck's maintenance screen when it shows
 * one (the surfaces task runs its navigation), else nowhere (surf_input_route; never Companion
 * while the screen on demand is asked for on this deck; every other deck goes on as before). Deck
 * task. */
/* inmtx held: one deck event to Companion (Satellite), as the control's press, release, turn or page; true
 * for a press (the click). */
static bool sat_send(int slot, const char *id, const deck_model_t *m, const deck_event_t *ev)
{
    surf_action_t a[2];
    const int n = inputmap_translate(m, ev, a, 2);
    bool click = false;
    for (int i = 0; i < n; i++) {
        switch (a[i].kind) {
        case ACT_PRESS:
            satclient_key(id, a[i].control_id, true);
            sat_down_note(slot, a[i].control_id, true);
            click = true;
            break;
        case ACT_RELEASE:
            satclient_key(id, a[i].control_id, false);
            sat_down_note(slot, a[i].control_id, false);
            break;
        case ACT_TAP:
            satclient_key(id, a[i].control_id, true);
            satclient_key(id, a[i].control_id, false);
            click = true;
            break;
        case ACT_ROTATE: satclient_rotate(id, a[i].control_id, a[i].delta); break;
        case ACT_PAGE: satclient_change_page(id, a[i].forward); break;
        }
    }
    return click;
}

/* inmtx held: the slot's gesture gate, started again when another deck has the slot or the gesture changed
 * (gesture, as read with the slot under the lock). */
static corner_t *corner_of(int slot, const usbdeck_t *d, const corner_cfg_t *gesture)
{
    if (surf.corner_deck[slot] != d || !corner_cfg_eq(&surf.corner[slot].cfg, gesture)) {
        corner_init(&surf.corner[slot], gesture);
        surf.corner_deck[slot] = d;
    }
    return &surf.corner[slot];
}

/* the gesture on the slot's deck (as configured; by default tap top-right, then top-right + bottom-right
 * held 2 s): the box menu opens on that deck (each deck on its own), unless it is open on it already; the click
 * and the light's flash as for any maintenance press. */
static void corner_open(int slot)
{
    char id[48] = "", model[33] = ""; /* the 32-character model names whole */
    lock();
    const surf_slot_t *sl = &surf.reg.slot[slot];
    /* only over a deck Companion drives (a chord held across a disconnect: nothing) */
    const bool companion = surf.dock ? surf.owned[slot] : surf.live && sl->added;
    const bool ok = companion && sl->device_id[0] && sl->model && deck_model_has_display(sl->model) && od_set_locked(sl->device_id, true);
    if (ok) {
        snprintf(model, sizeof model, "%s", sl->model->name);
        snprintf(id, sizeof id, "%s", sl->device_id);
    }
    unlock();
    if (!ok) return;
    ESP_LOGI(TAG, "maintenance screen on demand opened on %s (%s): the corner gesture", id, model);
    feedback_event(FB_EV_KEY_CLICK);
    maintui_maint(true); /* dock mode: the deck's input held from Companion at once */
    poke();
}

void surfaces_dock_corner(usbdeck_t *d, bool open)
{
    /* recorded only: the caller is docktxK or the deck's task */
    if (open) atomic_store(&surf.corner_open_deck, (uintptr_t)d);
    else atomic_fetch_add(&surf.corner_clicks, 1); /* a corner's press reached Companion late: its click */
    poke();
}

/* The surfaces task: the stacks of the tasks the gesture's gates run on, once, after the first gesture or
 * on-demand open (docktx0 overflowed its 3 KB at the open). */
static void log_gate_stacks(void)
{
    static bool logged;
    if (logged) return;
    logged = true;
    /* one line for the docks' tasks, one for the decks' (6 of each overran one 160 B line) */
    char line[96] = ""; /* 6 x " docktx5 65535" (14) = 84 */
    int n = 0;
    for (int k = 0; k < ELGDOCK_MAX && surf.dock; k++) {
        const unsigned f = elgdock_tx_stack_free(k);
        if (f && n < (int)sizeof line) n += snprintf(line + n, sizeof line - (size_t)n, " docktx%d %u", k, f);
    }
    if (n) ESP_LOGI(TAG, "stack never used after the first on-demand open:%s bytes", line);
    line[0] = '\0';
    n = 0;
    lock();
    for (int i = 0; i < SURF_MAX_DECKS; i++) {
        const usbdeck_t *d = surf.reg.slot[i].device_id[0] ? surf.reg.slot[i].deck : NULL;
        if (d && n < (int)sizeof line) n += snprintf(line + n, sizeof line - (size_t)n, " deck%d %u", i, usbdeck_stack_free(d));
    }
    unlock();
    if (n) ESP_LOGI(TAG, "stack never used after the first on-demand open:%s bytes", line);
}

/* The surfaces task: what the small tasks recorded: the gestures' opens and clicks, the service button's toggles
 * (surfaces_maint_toggle: an odd count toggles from the state now; surfaces_maint logs it, and a refusal, once). */
static void corner_requests(void)
{
    if (atomic_exchange(&surf.maint_toggles, 0) & 1u) surfaces_maint(!atomic_load(&surf.ondemand));
    for (unsigned c = atomic_exchange(&surf.corner_clicks, 0); c; c--) feedback_event(FB_EV_KEY_CLICK);
    const usbdeck_t *d = (const usbdeck_t *)atomic_exchange(&surf.corner_open_deck, (uintptr_t)0);
    unsigned slots = atomic_exchange(&surf.corner_open_slots, 0);
    if (d) {
        lock();
        for (int i = 0; i < SURF_MAX_DECKS; i++)
            if (surf.reg.slot[i].device_id[0] && surf.reg.slot[i].deck == d) slots |= 1u << i;
        unlock();
    }
    for (int i = 0; i < SURF_MAX_DECKS; i++)
        if (slots & (1u << i)) corner_open(i);
}

/* The surfaces task, dock mode: a new gesture (surfaces_set_settings) to the docks' gates, as it is now; kept
 * for the next pass while the docks don't run yet. */
static void gesture_step(void)
{
    if (!surf.dock || !atomic_exchange(&surf.gesture_dirty, false)) return;
    lock();
    const corner_cfg_t gesture = surf.settings.gesture;
    unlock();
    if (!elgdock_set_gesture(&gesture)) atomic_store(&surf.gesture_dirty, true);
}

/* The surfaces task (Satellite): the corner gates' time: a window that ended passes its press on to
 * Companion; a chord held 2 s opens the screen on that deck. A slot Companion doesn't take the input of
 * (the box's screen, on demand, handing back, not added) starts again. Returns when the next is due (us,
 * 0: none). */
static int64_t corner_step(int64_t now)
{
    corner_requests(); /* recorded on the small tasks, acted on here */
    gesture_step();
    if (atomic_load(&surf.ondemand)) log_gate_stacks();
    if (surf.dock || !surf.inmtx) return 0;
    int64_t next = 0;
    for (int slot = 0; slot < SURF_MAX_DECKS; slot++) {
        char id[48];
        lock();
        const surf_slot_t *sl = &surf.reg.slot[slot];
        const deck_model_t *m = sl->model;
        const usbdeck_t *d = sl->deck;
        const corner_cfg_t gesture = surf.settings.gesture;
        const bool used = sl->device_id[0] && m;
        const bool to_companion =
            used && !surf.dock &&
            surf_input_route(deck_model_has_display(m), surf_od_slot_locked(slot), surf.ui[slot], surf.live && sl->added,
                             surf.handback[slot], false) == SURF_TO_COMPANION;
        memcpy(id, sl->device_id, sizeof id);
        unlock();
        xSemaphoreTake(surf.inmtx, portMAX_DELAY);
        corner_t *c = corner_of(slot, d, &gesture);
        if (!to_companion) {
            corner_init(c, &gesture);
            xSemaphoreGive(surf.inmtx);
            continue;
        }
        uint8_t hw[2];
        const bool has = corner_keys(m, &c->cfg, hw);
        deck_event_t out[3];
        bool click = false, open = false;
        const int n = corner_sat_tick(c, has, hw, now / 1000, out, &open);
        for (int i = 0; i < n; i++) click |= sat_send(slot, id, m, &out[i]);
        const int64_t due = corner_due(c);
        xSemaphoreGive(surf.inmtx);
        if (click) feedback_event(FB_EV_KEY_CLICK);
        if (open) corner_open(slot); /* the surfaces task: fine here */
        if (due && (!next || due * 1000 < next)) next = due * 1000;
    }
    return next;
}

static void on_input(usbdeck_t *d, const deck_event_t *ev)
{
    char id[48];
    const deck_model_t *m = NULL;
    int slot = -1;
    uint32_t gen = 0;
    bool ui = false, companion = false, handback = false, od = false, loss = false;
    corner_cfg_t gesture;
    lock();
    gesture = surf.settings.gesture;
    slot = slot_of_locked(d);
    if (slot >= 0) {
        const surf_slot_t *sl = &surf.reg.slot[slot];
        gen = surf.gen[slot];
        m = sl->model;
        memcpy(id, sl->device_id, sizeof id);
        ui = surf.ui[slot];
        handback = surf.handback[slot];
        companion = surf.dock ? surf.owned[slot] : surf.live && sl->added;
        od = surf_od_slot_locked(slot);
        loss = surf.keeping[slot];
    }
    unlock();
    if (slot < 0 || !m) return; /* not registered (too many decks): nothing */
    switch (surf_input_route(deck_model_has_display(m), od, ui, companion, handback, loss)) {
    case SURF_TO_WAKE: { /* "Reconnecting" (or the images still up): the box's screens, now; the press does nothing else */
        bool first = false;
        lock();
        if (surf.gen[slot] == gen && !surf.woke[slot]) first = surf.woke[slot] = true;
        unlock();
        if (first) {
            ESP_LOGI(TAG, "slot %d: a key pressed while Companion is gone: the box's screens", slot);
            poke();
        }
        return;
    }
    case SURF_TO_BOX: {
        surf_ev_t e = {.kind = EV_INPUT, .slot = slot, .gen = gen, .deck = d, .in = *ev};
        if (ev->kind == DECK_EV_ENC_ROTATE || ev->kind == DECK_EV_TOUCH_SWIPE)
            post_event_lossy(&e);
        else
            post_event(&e);
        return;
    }
    case SURF_TO_DROP: return;
    case SURF_TO_COMPANION: break;
    }
    if (surf.dock) { /* the report has gone to Companion already when it drives the deck: only the click */
        /* no lock on the deck task: passthrough is set exactly while Companion drives the deck. A gesture key's
         * click comes when the corner gate passes its press on (a chord never clicks) */
        uint8_t hw[2];
        const bool corner = ev->kind == DECK_EV_KEY && corner_keys(m, &gesture, hw) && corner_which(hw, ev->index) >= 0;
        if (inputmap_key_down(ev) && usbdeck_passthrough(d) && !corner) feedback_event(FB_EV_KEY_CLICK);
        return;
    }
    bool click = false, open = false, due = false;
    xSemaphoreTake(surf.inmtx, portMAX_DELAY);
    lock(); /* inmtx before mtx: here only (sat_release_all takes them one after the other) */
    const bool opened = surf_od_slot_locked(slot) && deck_model_has_display(m);
    unlock();
    if (!opened) { /* opened on this deck meanwhile: the box takes it */
        /* the gesture's two keys (as configured) go through its gate (held back 120 ms only while armed);
         * others at once */
        corner_t *c = corner_of(slot, d, &gesture);
        uint8_t hw[2];
        const bool has = corner_keys(m, &c->cfg, hw);
        deck_event_t out[3];
        const int n = corner_sat_event(c, has, hw, ev, esp_timer_get_time() / 1000, out, &open);
        for (int i = 0; i < n; i++) click |= sat_send(slot, id, m, &out[i]);
        due = corner_due(c) != 0;
    }
    xSemaphoreGive(surf.inmtx);
    if (click) feedback_event(FB_EV_KEY_CLICK); /* after the press is queued for Companion */
    if (open) atomic_fetch_or(&surf.corner_open_slots, 1u << slot); /* opened by the surfaces task */
    if (open || due) poke(); /* the surfaces task's next pass opens, or ends the window on time */
}

static void on_problem(const char *msg) { problem_add(msg); }

/* The deck's quarantine for stalling USB is over (usbdeck took every image meanwhile with ESP_OK,
 * so none reached it): its quarantine problem is obsolete, and the surfaces task redraws it. */
static void on_restored(usbdeck_t *d)
{
    problem_drop_quarantine(usbdeck_serial(d)); /* in both modes, registered or not */
    /* Dock mode: Companion's images come back from dockmode's cache while Companion owns the deck
     * (the redraw below then only queues the message's frames again, if one is shown); otherwise the
     * redraw brings back the box's own screen. */
    if (surf.dock) dockmode_restored(d);
    surf_ev_t ev = {.kind = EV_RESTORED, .deck = d, .slot = -1};
    lock();
    ev.slot = slot_of_locked(d);
    if (ev.slot >= 0) {
        ev.gen = surf.gen[ev.slot];
        memcpy(ev.device_id, surf.reg.slot[ev.slot].device_id, sizeof ev.device_id);
    }
    unlock();
    if (ev.slot < 0) return; /* never registered (too many decks): nothing to redraw */
    ESP_LOGI(TAG, "%s: quarantine over, redrawing it", ev.device_id);
    post_event(&ev);
}

/* ---- image worker ---- */

static void store_last(int slot, const target_t *t, uint8_t *img)
{
    uint8_t *old = img;
    lock();
    if (surf.gen[slot] == t->gen) {
        old = surf.last[slot][t->idx];
        surf.last[slot][t->idx] = img;
    }
    unlock();
    heap_caps_free(old);
}

static void store_last_gen(int slot, int idx, uint32_t gen, uint8_t *img)
{
    const target_t t = {.idx = idx, .gen = gen};
    store_last(slot, &t, img);
}

static bool copy_last(int slot, const target_t *t, uint8_t *dst, size_t bytes)
{
    lock();
    bool ok = surf.gen[slot] == t->gen && surf.last[slot][t->idx];
    if (ok) memcpy(dst, surf.last[slot][t->idx], bytes);
    unlock();
    return ok;
}

static esp_err_t write_deck(int slot, const target_t *t, const uint8_t *img, size_t len)
{
    const deck_ctl_t *c = &t->ctl;
    bool owned = false;
    usbdeck_t *d = deck_acquire(USER_IMG, slot, t->gen, &owned);
    if (!d) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    if (owned) { /* dock mode, Companion drives it: only the box's frames get here, for dockmode to fit in */
        if (c->kind == CTL_KEY_LCD) {
            dockmode_box_key(d, c->hw, img, len);
        } else if (c->kind == CTL_LCD_CELL) {
            uint16_t x, y, w, h;
            deck_lcd_cell_region(t->m, c, &x, &y, &w, &h);
            dockmode_box_lcd(d, x, y, w, h, img, len);
        } else if (c->kind == CTL_INFO) {
            dockmode_box_info(d, img, len);
        }
    } else if (c->kind == CTL_KEY_LCD) {
        err = usbdeck_write_key(d, c->hw, img, len);
    } else if (c->kind == CTL_LCD_CELL) { /* the region unturned; the Plus XL's image is turned (lcd_xf) */
        uint16_t x, y, w, h;
        deck_lcd_cell_region(t->m, c, &x, &y, &w, &h);
        err = usbdeck_write_lcd(d, x, y, w, h, img, len);
    } else {
        err = usbdeck_write_info(d, img, len);
    }
    deck_release(USER_IMG);
    return err;
}

/* Copies the active message and its page on the slot's deck; false when there is none (or the screen on
 * demand outranks it). */
static bool msg_copy(int slot, char *text, size_t cap, int *page)
{
    lock();
    const bool on = surf.msg.on && !surf_od_slot_locked(slot); /* on demand outranks it on its own decks */
    if (on) snprintf(text, cap, "%s", surf.msg_text);
    *page = surf.msg_page[slot];
    unlock();
    return on;
}

/* The message's word for an LCD key (page `page`), laid out one word per key. */
static bool draw_message(const target_t *t, const char *text, int page, uint8_t *dst)
{
    const deck_model_t *m = t->m;
    if (t->ctl.w != m->key_px || t->ctl.h != m->key_px) return false;
    sui_content_t c;
    sui_screen_message(&c, text);
    sui_render_key(m->cols, m->rows, m->key_px, &c, page, t->ctl.col, t->ctl.row, dst);
    return true;
}

/* "Reconnecting" over this key's image in buf: the same on every LCD key. */
static void draw_reconnecting(const target_t *t, uint8_t *buf)
{
    const deck_model_t *m = t->m;
    if (t->ctl.kind != CTL_KEY_LCD || t->ctl.w != m->key_px || t->ctl.h != m->key_px) return;
    sui_content_t c;
    sui_screen_reconnecting(&c, NULL); /* the overlay draws only the word */
    sui_render_overlay_key(m->key_px, &c, buf);
}

static esp_err_t decode_b64(int slot, const target_t *t, const job_t *j, size_t bytes, uint8_t **out)
{
    uint8_t *img = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!img) return ESP_ERR_NO_MEM;
    if (!decode_bitmap((const char *)j->data, j->len, t->ctl.w, t->ctl.h, img, bytes)) {
        ESP_LOGD(TAG, "slot %d ctl %d: bad bitmap dropped", slot, t->idx);
        heap_caps_free(img);
        return ESP_ERR_INVALID_SIZE;
    }
    *out = img;
    return ESP_OK;
}

static esp_err_t render_job(int slot, const target_t *t, const job_t *j)
{
    static bool enc_warned;
    const deck_model_t *m = t->m;
    const deck_ctl_t *c = &t->ctl;

    if (c->kind == CTL_KEY_RGB) {
        if (j->kind != JOB_SOLID) return ESP_ERR_NOT_SUPPORTED;
        usbdeck_t *d = deck_acquire(USER_IMG, slot, t->gen, NULL);
        esp_err_t err = d ? usbdeck_fill_key(d, c->hw, j->r, j->g, j->b) : ESP_ERR_INVALID_STATE;
        deck_release(USER_IMG);
        return err;
    }
    if (!is_image_ctl(c->kind)) return ESP_ERR_NOT_SUPPORTED;
    const size_t bytes = (size_t)c->w * c->h * 3;
    if (bytes == 0 || bytes > IMG_MAX_BYTES) return ESP_ERR_INVALID_SIZE;

    char text[SURF_MSG_MAX + 1];
    int page = 0;
    const bool msg = c->kind == CTL_KEY_LCD && msg_copy(slot, text, sizeof text, &page);
    if (j->kind == JOB_MSG && !msg) return ESP_ERR_NOT_FOUND; /* message over: its end redraws the key */

    const uint8_t *src = surf.src;
    uint8_t *keep = NULL; /* decoded Companion image: becomes the control's last image */
    if (msg) { /* the message outranks everything on the LCD keys */
        if (!j->local) { /* Companion: update the cache only */
            if (j->kind == JOB_B64) {
                uint8_t *img = NULL;
                esp_err_t err = decode_b64(slot, t, j, bytes, &img);
                if (err != ESP_OK) return err;
                store_last(slot, t, img);
            } else if (j->kind == JOB_SOLID) {
                store_last(slot, t, NULL);
            }
            if (!j->msg) return ESP_OK; /* the key already shows the message */
        }
        if (!draw_message(t, text, page, surf.src)) return ESP_ERR_INVALID_SIZE;
    } else {
        switch (j->kind) {
        case JOB_B64: {
            esp_err_t err = decode_b64(slot, t, j, bytes, &keep);
            if (err != ESP_OK) return err;
            src = keep;
            break;
        }
        case JOB_RGB:
            if (j->len != bytes) return ESP_ERR_INVALID_SIZE;
            src = j->data;
            break;
        case JOB_SOLID:
            img_fill_rgb(surf.src, c->w, c->h, j->r, j->g, j->b);
            if (!j->local) store_last(slot, t, NULL); /* KEYS-CLEAR: nothing left to dim */
            break;
        case JOB_LAST:
            if (!copy_last(slot, t, surf.src, bytes)) img_fill_rgb(surf.src, c->w, c->h, 0, 0, 0);
            if (j->pct < 100) img_dim_rgb(surf.src, bytes, j->pct);
            if (j->overlay) draw_reconnecting(t, surf.src);
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    const img_xform_t xf = c->kind == CTL_KEY_LCD ? m->key_xf : m->lcd_xf;
    img_transform_rgb(src, c->w, c->h, xf, surf.xf);
    const bool swap = xf == IMG_XF_ROT90_CCW || xf == IMG_XF_MINI;
    const int ow = swap ? c->h : c->w, oh = swap ? c->w : c->h;
    if (keep) store_last(slot, t, keep);

    uint8_t *out = NULL;
    size_t len = 0;
    esp_err_t err = ESP_OK;
    if (m->fmt == DECK_IMG_JPEG) {
        err = img_jpeg_encode(surf.xf, ow, oh, &out, &len);
    } else if (m->fmt == DECK_IMG_BMP) {
        len = img_bmp_pack(surf.xf, ow, oh, surf.bmp, BMP_MAX_BYTES);
        out = surf.bmp;
        if (!len) err = ESP_ERR_INVALID_SIZE;
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        if (!enc_warned) ESP_LOGW(TAG, "image encode failed: %s", esp_err_to_name(err));
        enc_warned = true;
        return err == ESP_ERR_TIMEOUT ? ESP_FAIL : err; /* TIMEOUT means "deck queue full" here */
    }
    return write_deck(slot, t, out, len);
}

/* Draws and writes one job taken for the slot, or puts it back when the deck's queue is full. */
static void img_run(int slot, const target_t *tp, job_t *j, int64_t *retry_at)
{
    const target_t t = *tp;
    esp_err_t err = render_job(slot, &t, j);
    if (err == ESP_ERR_TIMEOUT) { /* deck queue full: keep the image and retry later */
        if (j->kind == JOB_B64) { /* already decoded and kept: redraw from the kept copy */
            job_t *r = job_new(JOB_LAST, 0, j->gen, false);
            if (r) r->msg = j->msg;
            job_free(j);
            j = r;
        }
        if (j && requeue(slot, t.idx, j)) *retry_at = esp_timer_get_time() + RETRY_US;
        return;
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_SIZE)
        ESP_LOGW(TAG, "slot %d ctl %d: image not written: %s", slot, t.idx, esp_err_to_name(err));
    job_free(j);
}

/* Takes and handles one job of the slot. Returns false when the slot had nothing pending. While at it,
 * surf.img_at[slot] says so: the job is out of the coalescer but not yet handed to usbdeck (the
 * hand-back waits for it, maintui.c). */
static bool img_one(int slot, int64_t *retry_at)
{
    target_t t = {0};
    lock();
    job_t *j = coalesce_take(&surf.co, slot, &t.idx);
    bool ok = j && j->gen == surf.gen[slot] && surf.reg.slot[slot].model && t.idx < surf.reg.slot[slot].nctl;
    if (ok) {
        t.gen = surf.gen[slot];
        t.m = surf.reg.slot[slot].model;
        t.ctl = surf.reg.slot[slot].ctls[t.idx];
    }
    if (j) surf.img_at[slot] = true;
    unlock();
    if (!j) return false;
    if (ok)
        img_run(slot, &t, j, retry_at);
    else
        job_free(j);
    lock();
    surf.img_at[slot] = false;
    unlock();
    return true;
}

/* On the task watchdog: it wakes at least every TICK_MS and feeds it once per round of jobs, so only
 * a job that hangs (decoder, JPEG engine) reboots the box. */
static void img_task(void *arg)
{
    (void)arg;
    int64_t retry_at[SURF_MAX_DECKS] = {0};
    int rr = 0;
    if (esp_task_wdt_add(NULL) != ESP_OK) ESP_LOGW(TAG, "imgwork: not on the task watchdog");
    for (;;) {
        esp_task_wdt_reset();
        TickType_t wait = pdMS_TO_TICKS(TICK_MS);
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < SURF_MAX_DECKS; i++) {
            if (!retry_at[i]) continue;
            int64_t left = retry_at[i] - now;
            TickType_t t = left <= 0 ? 0 : pdMS_TO_TICKS(left / 1000) + 1;
            if (t < wait) wait = t;
        }
        ulTaskNotifyTake(pdTRUE, wait);

        bool any;
        do { /* one job per deck per round, so a busy deck doesn't starve the others */
            esp_task_wdt_reset();
            any = false;
            for (int k = 0; k < SURF_MAX_DECKS; k++) {
                int slot = (rr + k) % SURF_MAX_DECKS;
                if (retry_at[slot] > esp_timer_get_time()) continue;
                retry_at[slot] = 0;
                if (img_one(slot, &retry_at[slot])) any = true;
            }
            rr = (rr + 1) % SURF_MAX_DECKS;
        } while (any);
    }
}

/* ---- surfaces task ---- */


static void add_device(int slot, const char *id, const deck_model_t *m)
{
    char *manifest = deck_manifest_json(m);
    if (!manifest) {
        lock();
        if (strcmp(surf.reg.slot[slot].device_id, id) == 0) surf.reg.slot[slot].added = false;
        unlock();
        problem_add("Out of memory building a Stream Deck layout");
        return;
    }
    char product[64];
    snprintf(product, sizeof product, "Elgato %s", m->name);
    const sat_device_desc_t d = {
        .device_id = id,
        .product_name = product,
        .manifest_json = manifest,
        .brightness = !m->pedal,
        .change_page_label = m->lcd_w ? PAGE_LABEL : NULL, /* Plus / Plus XL: swipe on the strip */
    };
    satclient_add_device(&d);
    free(manifest);
    ESP_LOGI(TAG, "ADD-DEVICE %s", id);
}

static void handle_attached(const surf_ev_t *ev)
{
    const int slot = ev->slot;
    uint8_t kinds[SURF_MAX_CTLS];
    bool has_last[SURF_MAX_CTLS], any_last = false;
    int n = 0;
    lock();
    const surf_slot_t *sl = &surf.reg.slot[slot];
    const bool valid = sl->deck == ev->deck && surf.gen[slot] == ev->gen;
    const deck_model_t *m = valid ? sl->model : NULL;
    const bool need_add = valid && !sl->added && surf.live && surf.handled_gen == surf.ready_gen;
    if (need_add) surf.reg.slot[slot].added = true; /* before the ADD, so a detach sends REMOVE */
    const bool live = surf.live;
    if (valid) {
        n = sl->nctl;
        for (int c = 0; c < n; c++) {
            kinds[c] = (uint8_t)sl->ctls[c].kind;
            has_last[c] = surf.last[slot][c] != NULL;
            any_last = any_last || has_last[c];
        }
    }
    unlock();
    if (!valid) return; /* gone again already */

    if (deck_model_has_display(m)) {
        /* Replug before the old detach: redraw what Companion last sent (it won't resend), in the
         * dim phase dimmed with "Reconnecting" over the keys. Strip cells and the info bar never
         * get status screens: start them black. An active message replaces all of it on the keys. */
        const bool dim = ev->reused && !live && surf.dimmed && any_last;
        for (int c = 0; c < n; c++) {
            job_t *j = NULL;
            if (dim && kinds[c] == CTL_KEY_LCD)
                j = last_job(ev->gen, DIM_PCT, true);
            else if (ev->reused && has_last[c])
                j = last_job(ev->gen, dim ? DIM_PCT : 100, false);
            else if (kinds[c] == CTL_LCD_CELL || kinds[c] == CTL_INFO)
                j = job_new(JOB_SOLID, 0, ev->gen, true);
            if (j) put_local(slot, c, j, false);
        }
        kick_imgwork();
    }
    maintui_redraw(slot);        /* the maintenance screen, if shown, is drawn again whole */
    surf.msg_seq_on[slot] = 0; /* nor the message, if one is active */
    if (need_add) add_device(slot, ev->device_id, m);
}

/* A deck's quarantine is over: draw everything it should show now, from what this module has. A deck
 * showing the maintenance screen: all of it again. Companion live, or the last page kept during a
 * short outage: Companion's last images (black where none is kept; dimmed with "Reconnecting" in the
 * dim phase). An active message is drawn again by update_message. RGB keys are set by feature
 * reports, which a quarantine doesn't stop: nothing to redraw there. */
static void handle_restored(const surf_ev_t *ev)
{
    const int slot = ev->slot;
    if (maintui_redraw(slot)) return; /* it shows the maintenance screen: all of it again */
    uint8_t kinds[SURF_MAX_CTLS];
    bool any_last = false;
    int n = 0;
    lock();
    const surf_slot_t *sl = &surf.reg.slot[slot];
    const bool valid = sl->deck == ev->deck && surf.gen[slot] == ev->gen && sl->model && deck_model_has_display(sl->model);
    const bool live = surf.live;
    if (valid) {
        n = sl->nctl;
        for (int c = 0; c < n; c++) {
            kinds[c] = (uint8_t)sl->ctls[c].kind;
            any_last = any_last || surf.last[slot][c] != NULL;
        }
    }
    unlock();
    if (!valid) return; /* gone again already, or nothing to draw on */

    const bool grace = surf.lost_at_us && esp_timer_get_time() - surf.lost_at_us < STATUS_AFTER_US;
    const bool last_page = live || (grace && any_last); /* as update_screens decides */
    const bool dim = !live && surf.dimmed && last_page;
    for (int c = 0; c < n; c++) {
        if (!is_image_ctl((deck_ctl_kind_t)kinds[c])) continue;
        job_t *j = NULL;
        if (last_page)
            j = last_job(ev->gen, dim ? DIM_PCT : 100, dim && kinds[c] == CTL_KEY_LCD);
        else if (kinds[c] != CTL_KEY_LCD)
            j = job_new(JOB_SOLID, 0, ev->gen, true); /* black */
        if (j) put_local(slot, c, j, false); /* a pending Companion image stays: it is newer */
    }
    kick_imgwork();
    surf.msg_seq_on[slot] = 0; /* the message sent meanwhile never arrived */
}

static void handle_detached(const surf_ev_t *ev)
{
    /* the same deck attached again and already added (its EV_ATTACHED handled first, on a fast re-attach): no
     * REMOVE-DEVICE after the new ADD-DEVICE. added is set on this task only, just before the ADD. */
    lock();
    const int again = surfreg_find(&surf.reg, ev->device_id);
    const bool readded = again >= 0 && surf.reg.slot[again].added;
    unlock();
    if (ev->was_added && !readded) satclient_remove_device(ev->device_id);
    ESP_LOGI(TAG, "%s gone from slot %d%s", ev->device_id, ev->slot, readded ? " (back already, added again)" : "");
}

/* Satellite: nothing pressed known to Companion any more, on every slot (a session ended or began). */
static void sat_down_clear(void)
{
    if (!surf.inmtx) return;
    xSemaphoreTake(surf.inmtx, portMAX_DELAY);
    memset(surf.nsat_down, 0, sizeof surf.nsat_down);
    xSemaphoreGive(surf.inmtx);
}

/* Companion READY (new session): re-add every attached deck. */
static void process_ready(void)
{
    int slots[SURF_MAX_DECKS];
    char ids[SURF_MAX_DECKS][48];
    const deck_model_t *models[SURF_MAX_DECKS];
    int n = 0;
    bool ready = false;
    lock();
    if (surf.live && surf.handled_gen != surf.ready_gen) {
        ready = true;
        surf.handled_gen = surf.ready_gen;
        surfreg_mark_all_unadded(&surf.reg);
        n = surf_pending_adds(&surf.reg, slots, SURF_MAX_DECKS);
        for (int k = 0; k < n; k++) {
            surf_slot_t *sl = &surf.reg.slot[slots[k]];
            sl->added = true;
            memcpy(ids[k], sl->device_id, sizeof ids[k]);
            models[k] = sl->model;
        }
    }
    unlock();
    if (!ready) return;
    surf.lost_at_us = 0;
    surf.dimmed = false;
    sat_down_clear(); /* a new session saw nothing pressed */
    for (int k = 0; k < n; k++) add_device(slots[k], ids[k], models[k]);
}

static void detect_loss(int64_t now)
{
    lock();
    bool live = surf.live;
    uint32_t g = surf.ready_gen;
    unlock();
    if (!live || satclient_state() == SATC_CONNECTED) return;
    lock();
    if (surf.ready_gen == g) surf.live = false; /* unless a new session became ready meanwhile */
    live = surf.live;
    if (!live) surf.sat_lost = true;
    unlock();
    if (!live) {
        ESP_LOGW(TAG, "Companion connection lost");
        surf.lost_at_us = now;
        surf.dimmed = false;
        sat_down_clear(); /* the session that saw them pressed is over */
    }
}

/* A screen shown has its next page due at due_us (0: none): the surfaces task wakes by then. */
static void page_due(int64_t due_us)
{
    if (due_us && (!surf.page_due_us || due_us < surf.page_due_us)) surf.page_due_us = due_us;
}

/* Companion lost: redraw every kept image dimmed, with "Reconnecting" across the LCD keys (black
 * under it where nothing is kept). Decks with nothing kept show the maintenance screens instead. */
static void dim_all(void)
{
    for (int slot = 0; slot < SURF_MAX_DECKS; slot++) {
        uint8_t kinds[SURF_MAX_CTLS];
        bool has[SURF_MAX_CTLS], any = false;
        int n = 0;
        lock();
        const uint32_t gen = surf.gen[slot];
        const surf_slot_t *sl = &surf.reg.slot[slot];
        if (sl->device_id[0] && sl->model && deck_model_has_display(sl->model) && !surf.ui[slot]) n = sl->nctl;
        for (int c = 0; c < n; c++) {
            kinds[c] = (uint8_t)sl->ctls[c].kind;
            has[c] = surf.last[slot][c] != NULL;
            any = any || has[c];
        }
        unlock();
        if (!any) continue;
        for (int c = 0; c < n; c++) {
            if (kinds[c] != CTL_KEY_LCD && !has[c]) continue;
            job_t *j = last_job(gen, DIM_PCT, kinds[c] == CTL_KEY_LCD);
            if (!j) break;
            put_local(slot, c, j, true);
        }
    }
    kick_imgwork();
}

/* Queues the active message (seq) on the slot's LCD keys: its first page once per message and deck,
 * then the next page every sui_page_us when the deck has too few keys for all of it. */
static void show_message(int slot, uint32_t seq, int64_t now)
{
    uint8_t idx[SURF_MAX_CTLS];
    int nk = 0;
    char text[SURF_MSG_MAX + 1];
    lock();
    const uint32_t gen = surf.gen[slot];
    const surf_slot_t *sl = &surf.reg.slot[slot];
    const deck_model_t *m = sl->model;
    const bool fresh = surf.msg_seq_on[slot] != seq || surf.msg_gen_on[slot] != gen;
    if (sl->device_id[0] && m && deck_model_has_display(m) && !surf_od_slot_locked(slot)) { /* on demand outranks it */
        for (int c = 0; c < sl->nctl; c++)
            if (sl->ctls[c].kind == CTL_KEY_LCD) idx[nk++] = (uint8_t)c;
    }
    snprintf(text, sizeof text, "%s", surf.msg_text);
    unlock();
    if (nk == 0) return;
    int64_t due = 0;
    const int page = sui_pager_next(&surf.msg_pg[slot], fresh, now, &due);
    page_due(due);
    if (page < 0) return; /* shown, and its page is still up */
    int pages = surf.msg_pg[slot].pages;
    if (fresh) {
        sui_content_t c;
        sui_screen_message(&c, text);
        pages = sui_page_count(m->cols, m->rows, m->key_px, &c);
    }
    lock();
    surf.msg_page[slot] = (uint8_t)page; /* imgwork draws this page from now on */
    unlock();
    bool all = true;
    for (int k = 0; k < nk; k++)
        if (!put_msg(slot, idx[k], gen)) all = false;
    kick_imgwork();
    if (all) { /* else retried on the next tick */
        surf.msg_seq_on[slot] = seq;
        surf.msg_gen_on[slot] = gen;
        page_due(sui_pager_shown(&surf.msg_pg[slot], page, pages, now));
    }
}

/* Message over: Companion's images come back from the cache (black where none is kept) while it is
 * live or in the first 2 s after a loss; otherwise update_screens redraws the dim phase, or the
 * maintenance screens come back (drawn again whole). */
static void end_message(int64_t now)
{
    lock();
    const bool live = surf.live;
    unlock();
    const bool before_dim = !live && surf.lost_at_us && now - surf.lost_at_us < DIM_AFTER_US;
    for (int slot = 0; slot < SURF_MAX_DECKS; slot++) {
        uint8_t idx[SURF_MAX_CTLS];
        int nk = 0;
        bool any_last = false;
        lock();
        const uint32_t gen = surf.gen[slot];
        const surf_slot_t *sl = &surf.reg.slot[slot];
        const bool ui = surf.ui[slot];
        if (sl->device_id[0] && sl->model && deck_model_has_display(sl->model) && !ui) {
            for (int c = 0; c < sl->nctl; c++) {
                any_last = any_last || surf.last[slot][c] != NULL;
                if (sl->ctls[c].kind == CTL_KEY_LCD) idx[nk++] = (uint8_t)c;
            }
        }
        unlock();
        if (ui) maintui_redraw(slot);
        if (!live && !(before_dim && any_last)) continue;
        for (int k = 0; k < nk; k++) {
            job_t *j = last_job(gen, 100, false);
            if (!j) break;
            put_local(slot, idx[k], j, !live);
        }
    }
    kick_imgwork();
    surf.dimmed = false;
    if (surf.dock) { /* each deck in its dim phase gets it drawn again over the message (update_dock_screens) */
        lock();
        for (int slot = 0; slot < SURF_MAX_DECKS; slot++) surf.ddimmed[slot] = false;
        unlock();
    }
}

/* Expires the message, draws it on every deck (also decks that come later) and redraws the decks
 * when it ends, also when it came and went before this task saw it: imgwork may have drawn it or
 * kept Companion images back meanwhile. Returns true while a message is active. */
static bool update_message(int64_t now)
{
    bool expired = false;
    lock();
    const surf_message_action_t act = surf_message_step(&surf.msg, now, &expired);
    const uint32_t seq = surf.msg.seq;
    unlock();
    if (expired) ESP_LOGI(TAG, "deck message expired");

    if (surf.dock) dockmode_message(act == SURF_MSG_DRAW); /* before the frames: they need the hold */
    if (act == SURF_MSG_DRAW) /* the screen on demand outranks it on its deck (show_message) */
        for (int i = 0; i < SURF_MAX_DECKS; i++) show_message(i, seq, now);
    else if (act == SURF_MSG_END)
        end_message(now);
    return act == SURF_MSG_DRAW;
}

/* Satellite, Companion lost: after DIM_AFTER_US the kept images dimmed with "Reconnecting" over them,
 * kept until STATUS_AFTER_US; then (or with nothing kept) the maintenance screens (maint_update). */
static void update_screens(int64_t now, bool msg)
{
    lock();
    const bool live = surf.live;
    unlock();
    if (live) { /* Companion draws everything */
        surf.lost_at_us = 0;
        surf.dimmed = false;
        return;
    }
    const bool grace = surf.lost_at_us && now - surf.lost_at_us < STATUS_AFTER_US;
    if (!grace) surf.lost_at_us = 0;
    if (msg) return; /* the message outranks the dim phase */
    if (grace && !surf.dimmed && now - surf.lost_at_us >= DIM_AFTER_US) {
        ESP_LOGI(TAG, "Companion still gone: dimming");
        dim_all();
        surf.dimmed = true;
    }
}

/* Dock mode: key hw's last image from the snapshot, upright (the deck's transform undone), key_px square RGB in
 * PSRAM; NULL: none kept or it doesn't decode (black under "Reconnecting"). */
static uint8_t *snap_key_rgb(const deck_model_t *m, const surf_dock_snap_t *s, uint8_t hw)
{
    if (!s || hw >= SURF_SNAP_KEYS || s->kind[hw] == DC_NONE) return NULL;
    const int px = m->key_px;
    const size_t bytes = (size_t)px * px * 3;
    uint8_t *out = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;
    if (s->kind[hw] == DC_FILL) {
        img_fill_rgb(out, px, px, s->rgb[hw][0], s->rgb[hw][1], s->rgb[hw][2]);
        return out;
    }
    uint8_t *dev = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    const bool ok = dev && (m->fmt == DECK_IMG_JPEG ? img_jpeg_decode(s->img[hw], s->len[hw], px, px, dev)
                            : m->fmt == DECK_IMG_BMP ? img_bmp_unpack(s->img[hw], s->len[hw], px, px, dev)
                                                     : false);
    if (ok) img_untransform_rgb(dev, px, px, m->key_xf, out);
    heap_caps_free(dev);
    if (ok) return out;
    heap_caps_free(out);
    return NULL;
}

/* Dock mode, Companion still gone from the slot's deck: its last key images (the snapshot, decoded the first time;
 * kept, when a message ended over them) dimmed with "Reconnecting" over the LCD keys, as Satellite mode's dim phase;
 * the strip and the info bar black. */
static void dock_dim(int slot)
{
    uint8_t kinds[SURF_MAX_CTLS], hws[SURF_MAX_CTLS];
    int n = 0;
    lock();
    const uint32_t gen = surf.gen[slot];
    const surf_slot_t *sl = &surf.reg.slot[slot];
    const deck_model_t *m = sl->device_id[0] ? sl->model : NULL;
    surf_dock_snap_t *snap = surf.dsnap[slot];
    surf.dsnap[slot] = NULL;
    surf.ddimmed[slot] = true;
    if (m && deck_model_has_display(m) && !surf.owned[slot] && !surf.ui[slot]) n = sl->nctl; /* the box's screen up: no */
    for (int c = 0; c < n; c++) {
        kinds[c] = (uint8_t)sl->ctls[c].kind;
        hws[c] = sl->ctls[c].hw;
    }
    unlock();
    int keys = 0, images = 0, fills = 0, drawn = 0; /* for the log: what came back under the word */
    for (int c = 0; c < n; c++) {
        job_t *j = NULL;
        if (kinds[c] == CTL_KEY_LCD) {
            keys++;
            if (snap) {
                const uint8_t k = hws[c] < SURF_SNAP_KEYS ? snap->kind[hws[c]] : DC_NONE;
                uint8_t *img = snap_key_rgb(m, snap, hws[c]); /* NULL: black under the word */
                images += k == DC_IMAGE;
                fills += k == DC_FILL;
                drawn += img != NULL;
                store_last_gen(slot, c, gen, img);
            }
            j = last_job(gen, DIM_PCT, true);
        } else if (kinds[c] == CTL_LCD_CELL || kinds[c] == CTL_INFO) {
            j = job_new(JOB_SOLID, 0, gen, true); /* black, as without images */
        }
        if (j) put_local(slot, c, j, false);
    }
    surf_dock_snap_free(snap);
    if (n)
        ESP_LOGI(TAG, "slot %d: Companion still gone: \"Reconnecting\" over %d of %d keys' images (kept: %d images, %d fills)",
                 slot, drawn, keys, images, fills);
    if (n) kick_imgwork();
}

/* Dock mode: the loss phase of the slot's deck is over (SURF_LOSS_OVER_US without Companion): the box's screens. */
static void dock_loss_end(int slot)
{
    lock();
    surf_dock_snap_t *snap = surf.dsnap[slot];
    surf.dsnap[slot] = NULL;
    surf.dlost_at_us[slot] = 0;
    surf.ddimmed[slot] = false;
    free_last_locked(slot);
    unlock();
    surf_dock_snap_free(snap);
    atomic_store(&surf.snap_dirty, true);
}

/* Network Dock mode, Companion gone from a deck it drove: each deck on its own (its own connection), as Satellite mode
 * for all of them: the images stay SURF_LOSS_DIM_US, then dimmed with "Reconnecting" until SURF_LOSS_OVER_US, then
 * the box's screens (maintui). The message outranks the dim phase, as in Satellite mode. */
static void update_dock_screens(int64_t now, bool msg)
{
    if (!surf.dock) return;
    for (int slot = 0; slot < SURF_MAX_DECKS; slot++) {
        lock();
        const int64_t at = surf.dlost_at_us[slot];
        const surf_loss_t ph = surf_loss_phase(at, surf.ddimmed[slot], now);
        const bool woke = surf.woke[slot];
        unlock();
        if (ph != SURF_LOSS_NONE && woke) { /* a key pressed: the box's screens now, the phase over */
            dock_loss_end(slot);
            continue;
        }
        switch (ph) {
        case SURF_LOSS_NONE: break;
        case SURF_LOSS_GRACE: page_due(at + SURF_LOSS_DIM_US); break;
        case SURF_LOSS_DIM_DUE:
            if (!msg) dock_dim(slot);
            page_due(at + SURF_LOSS_OVER_US);
            break;
        case SURF_LOSS_DIMMED: page_due(at + SURF_LOSS_OVER_US); break;
        case SURF_LOSS_OVER: dock_loss_end(slot); break;
        }
    }
}

static bool s_ui_any; /* a deck shows the maintenance screen, or is handing back after it */

/* Until the next pass: TICK_MS, UI_TICK_MS while a deck shows the maintenance screen or hands back, or
 * less when a message's next page is due sooner. */
static int64_t s_corner_due_us; /* the next gate window or hold due (surfaces task) */

static TickType_t task_wait(void)
{
    TickType_t wait = pdMS_TO_TICKS(s_ui_any ? UI_TICK_MS : TICK_MS);
    const int64_t due[2] = {surf.page_due_us, s_corner_due_us};
    for (int i = 0; i < 2; i++) {
        if (!due[i]) continue;
        const int64_t left = due[i] - esp_timer_get_time();
        const TickType_t t = left <= 0 ? 0 : pdMS_TO_TICKS(left / 1000) + 1;
        if (t < wait) wait = t;
    }
    return wait;
}

static void surf_task(void *arg)
{
    (void)arg;
    surf_ev_t ev;
    for (;;) {
        if (xQueueReceive(surf.evq, &ev, task_wait()) == pdTRUE) {
            switch (ev.kind) {
            case EV_ATTACHED: handle_attached(&ev); break;
            case EV_DETACHED: handle_detached(&ev); break;
            case EV_RESTORED: handle_restored(&ev); break;
            case EV_POKE: atomic_store(&surf.poke_pending, false); break;
            case EV_INPUT: maintui_input(ev.slot, ev.gen, &ev.in); break;
            case EV_PIN: maintui_pin_result(ev.slot, ev.gen, ev.pin_result, ev.wait_s); break;
            }
        }
        const int64_t now = esp_timer_get_time();
        s_corner_due_us = corner_step(now); /* before the screens: a gesture may open one */
        process_ready();
        detect_loss(now);
        surf.page_due_us = 0; /* the message's pages set it again */
        const bool msg = update_message(now);
        update_screens(now, msg);
        update_dock_screens(now, msg);
        s_ui_any = maintui_update(now, msg);
    }
}

/* ---- public API ---- */

/* Allocations, the JPEG encoder and the two tasks. */
static esp_err_t start_common(void)
{
    if (surf.surf_task) return ESP_ERR_INVALID_STATE;
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    /* the event queue's storage in PSRAM (internal when PSRAM is short): posted and taken by tasks only (deck,
     * USB client, Satellite and surfaces tasks), never an ISR; events are copied in and out. Never deleted. */
    surf.evq = xQueueCreateWithCaps(EVQ_DEPTH, sizeof(surf_ev_t), MALLOC_CAP_SPIRAM);
    if (!surf.evq) surf.evq = xQueueCreate(EVQ_DEPTH, sizeof(surf_ev_t));
    surf.inmtx = xSemaphoreCreateMutex();
    for (int i = 0; i < SURF_MAX_DECKS; i++) surf.comp_bright[i] = -1;
    maintui_init(); /* the maintenance screens' state: ~9 KB a deck, PSRAM */
    atomic_store(&surf.snap_dirty, true);
    surf.src = heap_caps_malloc(IMG_MAX_BYTES, MALLOC_CAP_SPIRAM);
    surf.xf = heap_caps_malloc(IMG_MAX_BYTES, MALLOC_CAP_SPIRAM);
    surf.bmp = heap_caps_malloc(BMP_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!surf.evq || !surf.inmtx || !surf.src || !surf.xf || !surf.bmp) {
        ESP_LOGE(TAG, "allocation failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = img_jpeg_init(); /* idempotent */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encoder init failed: %s", esp_err_to_name(err));
        problem_add("Hardware JPEG encoder failed to start: Stream Deck images unavailable");
    }
    if (xTaskCreate(img_task, "imgwork", IMG_STACK, NULL, IMG_PRIO, &surf.img_task) != pdPASS ||
        xTaskCreate(surf_task, "surfaces", SURF_STACK, NULL, SURF_PRIO, &surf.surf_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t start_usb(void)
{
    const usbdeck_cb_t ucb = {
        .on_attach = on_attach,
        .on_detach = on_detach,
        .on_input = on_input,
        .on_problem = on_problem,
        .on_restored = on_restored,
    };
    esp_err_t err = usbdeck_start(&ucb);
    if (err != ESP_OK) ESP_LOGE(TAG, "usbdeck start failed: %s", esp_err_to_name(err));
    return err;
}

/* the deck limit (2 or 4; anything else: 2), set before the tasks start. */
static void set_limit(int deck_limit)
{
    surf.limit = deck_limit == 4 ? 4 : 2; /* 2 or 4 as the RPC takes them, anything else 2 */
    surf.reg.cap = (uint8_t)surf.limit;
    ESP_LOGI(TAG, "up to %d Stream Decks at once (deckLimit)", surf.limit);
}

int surfaces_decks_ignored(void)
{
    if (!ensure_mutex()) return 0;
    lock();
    const int n = surf.nignored;
    unlock();
    return n;
}

bool surfaces_deck_ignored(const char *serial)
{
    bool hit = false;
    if (!serial || !serial[0] || !ensure_mutex()) return false;
    lock();
    for (int i = 0; i < surf.nignored && !hit; i++) hit = !strcmp(surf.ignored_sn[i], serial);
    unlock();
    return hit;
}

esp_err_t surfaces_start(int deck_limit)
{
    if (surf.surf_task) return ESP_ERR_INVALID_STATE;
    set_limit(deck_limit);
    esp_err_t err = start_common();
    if (err != ESP_OK) return err;

    const sat_session_cb_t scb = {
        .on_ready = on_ready,
        .on_too_old = on_too_old,
        .on_key_state = on_key_state,
        .on_clear = on_clear,
        .on_brightness = on_brightness,
        .on_add_result = on_add_result,
    };
    err = satclient_start(&scb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "satclient start failed: %s", esp_err_to_name(err));
        return err;
    }
    return start_usb();
}

esp_err_t surfaces_start_dock(const char *box_name, const uint8_t mac[6], bool announce, int deck_limit)
{
    if (surf.surf_task) return ESP_ERR_INVALID_STATE;
    surf.dock = true;
    set_limit(deck_limit);
    esp_err_t err = start_common();
    if (err != ESP_OK) return err;
    err = dockmode_start(box_name, mac, announce, surf.limit); /* a dock per deck taken */
    if (err == ESP_OK) poke(); /* the surfaces task gives the docks' gates the gesture (gesture_step) */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network Dock servers failed to start: %s", esp_err_to_name(err));
        problem_add("Network Dock mode failed to start: Companion can't reach the decks");
    }
    const esp_err_t uerr = start_usb(); /* the decks still show their status screens */
    return err != ESP_OK ? err : uerr;
}

void surfaces_dock_apply(const char *box_name, bool announce)
{
    if (surf.dock) dockmode_apply(box_name, announce);
}

void surf_dock_snap_free(surf_dock_snap_t *s)
{
    if (!s) return;
    for (int k = 0; k < SURF_SNAP_KEYS; k++) heap_caps_free(s->img[k]);
    heap_caps_free(s);
}

void surfaces_dock_owner(usbdeck_t *d, bool owned, const char *peer_ip, surf_dock_snap_t *snap)
{
    int slot = -1, n = 0;
    uint8_t kinds[SURF_MAX_CTLS];
    bool released = false, keep = false;
    surf_dock_snap_t *old = NULL;
    lock();
    slot = slot_of_locked(d);
    if (slot >= 0 && owned && peer_ip) /* a take-over or a hand-over: the Companion that drives it now */
        snprintf(surf.peer[slot], sizeof surf.peer[slot], "%s", peer_ip);
    if (slot >= 0 && surf.owned[slot] != owned) {
        released = !owned;
        surf.owned[slot] = owned;
        surf.lost[slot] = !owned; /* the deck is still there (a deck pulled out never gets here) */
        /* Frames queued or being made for it are void: when taken over, none may follow Companion's;
         * when let go, the box's frames meant for dockmode go to the deck itself, made anew (a new gen
         * has update_message and maint_update queue the message or the maintenance screen again). */
        coalesce_drop_slot(&surf.co, slot, job_free);
        surf.gen[slot]++;
    }
    if (slot >= 0 && (owned || released)) { /* a new owner ends the loss phase; a release starts one, with images */
        old = surf.dsnap[slot];
        surf.dsnap[slot] = NULL;
        surf.dlost_at_us[slot] = 0;
        surf.ddimmed[slot] = false;
        free_last_locked(slot); /* dock mode keeps last images only for the dim phase */
        keep = released && snap && surf.reg.slot[slot].model && deck_model_has_display(surf.reg.slot[slot].model);
        if (keep) {
            surf.dsnap[slot] = snap;
            snap = NULL;
            surf.dlost_at_us[slot] = esp_timer_get_time();
        }
    }
    while (owned && inuse_locked(d)) { /* imgwork is handing it one more frame */
        unlock();
        vTaskDelay(1);
        lock();
    }
    const uint32_t gen = slot >= 0 ? surf.gen[slot] : 0;
    if (released && !keep && surf.reg.slot[slot].model && deck_model_has_display(surf.reg.slot[slot].model)) {
        n = surf.reg.slot[slot].nctl;
        for (int c = 0; c < n; c++) kinds[c] = (uint8_t)surf.reg.slot[slot].ctls[c].kind;
    }
    unlock();
    surf_dock_snap_free(old);
    surf_dock_snap_free(snap); /* not taken: nothing to dim */
    if (keep) ESP_LOGI(TAG, "slot %d: Companion let go: its images stay, \"Reconnecting\" in %d s", slot,
                       (int)(SURF_LOSS_DIM_US / 1000000));
    for (int c = 0; c < n; c++) { /* Companion's last strip and info bar images go at once (black), until the
                                    * maintenance screen's next pass draws its status bar there */
        if (kinds[c] != CTL_LCD_CELL && kinds[c] != CTL_INFO) continue;
        job_t *j = job_new(JOB_SOLID, 0, gen, true); /* black */
        if (j) put_local(slot, c, j, false);
    }
    if (n) kick_imgwork();
    if (slot >= 0) poke(); /* the maintenance screen comes back (or not yet: the loss phase), or "Connected" on demand */
}

void surfaces_net_changed(void) { poke(); }

void surfaces_set_hooks(const surf_hooks_t *h)
{
    if (!h || !ensure_mutex()) return;
    lock();
    surf.hooks = *h;
    unlock();
}

/* mtx held: the slot of the attached deck with a screen named by serial (or device_id); -1 none. */
static int od_slot_of_locked(const char *serial)
{
    char id[48];
    snprintf(id, sizeof id, "%s%s", strncmp(serial, DEVICE_ID_PREFIX, strlen(DEVICE_ID_PREFIX)) ? DEVICE_ID_PREFIX : "", serial);
    surf_od_cand_t c[SURF_MAX_DECKS];
    surf_od_cands_locked(c);
    return surf_od_find(c, SURF_MAX_DECKS, id);
}

/* The box menu opened (on) or closed on the deck named by serial; none named (NULL or ""): closing, on every
 * deck; opening, on the biggest with a screen (the pick). The deck is found, changed and named under one lock, so
 * a deck unplugged meanwhile can't leave a stale slot. Logged; the dock hold taken at once on an open; the task
 * poked. ESP_ERR_NOT_FOUND: no such deck with a screen attached. */
static esp_err_t od_change(const char *serial, bool on)
{
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    const bool named = serial && serial[0], all = !on && !named;
    char id[48] = "", model[33] = ""; /* the 32-character model names whole */
    bool changed = false;
    int k = -1;
    lock();
    if (all) {
        changed = surf.od.n > 0;
        surf.od.n = 0;
        atomic_store(&surf.ondemand, false);
    } else {
        if (named) {
            k = od_slot_of_locked(serial);
        } else {
            surf_od_cand_t c[SURF_MAX_DECKS];
            surf_od_cands_locked(c);
            k = surf_od_pick(c, SURF_MAX_DECKS);
        }
        if (k >= 0) { /* find and pick only give a slot with a deck that has a screen (a model) */
            snprintf(id, sizeof id, "%s", surf.reg.slot[k].device_id);
            snprintf(model, sizeof model, "%s", surf.reg.slot[k].model->name);
            changed = od_set_locked(id, on);
        }
    }
    unlock();
    if (!all && k < 0) {
        if (!named) ESP_LOGI(TAG, "box menu not opened: no deck with a screen attached");
        return ESP_ERR_NOT_FOUND;
    }
    if (changed) {
        if (all) ESP_LOGI(TAG, "box menu closed on every deck");
        else ESP_LOGI(TAG, "box menu %s on %s (%s)", on ? "opened" : "closed", id, model);
    }
    maintui_maint(on); /* dock mode: the deck's input held from Companion at once */
    poke();
    return ESP_OK;
}

esp_err_t surfaces_maint(bool on) { return od_change(NULL, on); }

esp_err_t surfaces_maint_deck_set(const char *serial, bool on) { return od_change(serial, on); }

int surfaces_maint_list(char serials[][33], int max)
{
    if (!ensure_mutex()) return 0;
    lock();
    int n = 0;
    for (int i = 0; i < surf.od.n && n < max; i++) snprintf(serials[n++], 33, "%s", surf_serial_of(surf.od.id[i]));
    unlock();
    return n;
}

bool surfaces_deck_menu(const char *serial, bool *screen)
{
    bool open = false, scr = false;
    if (serial && serial[0] && ensure_mutex()) {
        lock();
        const int k = od_slot_of_locked(serial);
        scr = k >= 0;
        open = scr && surf_od_slot_locked(k);
        unlock();
    }
    if (screen) *screen = scr;
    return open;
}

void surfaces_maint_toggle(void)
{
    atomic_fetch_add(&surf.maint_toggles, 1);
    poke(); /* never blocks: a full queue drops the poke, and the 1 s pass picks the request up */
}

static bool maint_deck(char serial[33], char model[33], bool first_open)
{
    serial[0] = model[0] = '\0';
    if (!ensure_mutex()) return false;
    lock();
    int k = -1;
    surf_od_cand_t c[SURF_MAX_DECKS];
    surf_od_cands_locked(c);
    for (int i = 0; first_open && i < surf.od.n && k < 0; i++) k = surf_od_find(c, SURF_MAX_DECKS, surf.od.id[i]);
    if (k < 0) k = surf_od_pick(c, SURF_MAX_DECKS); /* the one surfaces_maint(true) opens on */
    if (k >= 0) {
        snprintf(serial, 33, "%s", surf_serial_of(surf.reg.slot[k].device_id));
        snprintf(model, 33, "%s", surf.reg.slot[k].model->name);
    }
    unlock();
    return k >= 0;
}

bool surfaces_maint_deck(char serial[33], char model[33]) { return maint_deck(serial, model, true); }
bool surfaces_maint_pick(char serial[33], char model[33]) { return maint_deck(serial, model, false); }

bool surfaces_maint_on(void) { return atomic_load(&surf.ondemand); }

void surfaces_pin_result(int slot, uint32_t ticket, int result, int wait_s)
{
    if (slot < 0 || slot >= SURF_MAX_DECKS) return;
    const surf_ev_t ev = {.kind = EV_PIN, .slot = slot, .gen = ticket, .pin_result = result, .wait_s = wait_s};
    post_event(&ev);
}

void surfaces_toast(int outcome, const char *text)
{
    if (!ensure_mutex()) return;
    lock();
    snprintf(surf.toast, sizeof surf.toast, "%s", text ? text : "");
    surf.toast_state = outcome == SURF_TOAST_REFUSED ? DUI_STATE_BAD : outcome == SURF_TOAST_BUSY ? DUI_STATE_WAIT : DUI_STATE_OK;
    surf.toast_until_us = esp_timer_get_time() + TOAST_US;
    unlock();
    poke();
}

void surfaces_set_settings(const surf_settings_t *st)
{
    if (!st || !ensure_mutex()) return;
    lock();
    const bool moved = !surf.gesture_given || !corner_cfg_eq(&surf.settings.gesture, &st->gesture);
    surf.settings = *st;
    surf.gesture_given = true;
    unlock();
    if (moved) { /* the Satellite gates take it up at their next event (corner_of); the docks' from the surfaces
                  * task (gesture_step), the one task that gives it them: always the latest, in order */
        char what[80];
        corner_cfg_describe(&st->gesture, what, sizeof what);
        ESP_LOGI(TAG, "box menu gesture: %s", what);
        atomic_store(&surf.gesture_dirty, true);
    }
    atomic_store(&surf.snap_dirty, true); /* a new brightness shows (and applies) at once, not up to 2 s on */
    poke();
}

void surfaces_companion(bool *driving, bool *lost)
{
    *driving = *lost = false;
    if (!surf.mtx) return;
    lock();
    if (surf.dock) {
        for (int i = 0; i < SURF_MAX_DECKS; i++) {
            *driving |= surf.owned[i];
            *lost |= surf.lost[i];
        }
    } else {
        *driving = surf.live;
        *lost = surf.sat_lost;
    }
    unlock();
}

void surfaces_set_selection(sel_result_t sel)
{
    if (!ensure_mutex()) return;
    lock();
    const bool changed = surf.sel != sel;
    surf.sel = sel;
    unlock();
    if (changed) poke();
}

void surfaces_set_ap_info(const char *ssid_or_null, const char *pass_or_null)
{
    if (!ensure_mutex()) return;
    lock();
    snprintf(surf.ap_ssid, sizeof surf.ap_ssid, "%s", ssid_or_null ? ssid_or_null : "");
    snprintf(surf.ap_pass, sizeof surf.ap_pass, "%s", pass_or_null ? pass_or_null : "");
    surf.ap_shown = ssid_or_null != NULL;
    surf.ap_open = pass_or_null == NULL;
    unlock();
    poke();
}

esp_err_t surfaces_show_message(const char *text, uint32_t seconds)
{
    if (!surf_message_valid(text, seconds)) return ESP_ERR_INVALID_ARG;
    if (!ensure_mutex()) return ESP_ERR_NO_MEM;
    const int64_t until = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    lock();
    snprintf(surf.msg_text, sizeof surf.msg_text, "%s", text);
    memset(surf.msg_page, 0, sizeof surf.msg_page); /* a new message starts on its first page everywhere */
    surf_message_show(&surf.msg, until);
    unlock();
    if (seconds)
        ESP_LOGI(TAG, "deck message shown for %u s", (unsigned)seconds);
    else
        ESP_LOGI(TAG, "deck message shown until cleared");
    poke();
    return ESP_OK;
}

void surfaces_clear_message(void)
{
    if (!surf.mtx) return;
    lock();
    const bool was_on = surf_message_clear(&surf.msg);
    unlock();
    if (!was_on) return;
    ESP_LOGI(TAG, "deck message cleared");
    poke();
}

bool surfaces_message_active(char *text, size_t cap, uint32_t *remaining_s)
{
    bool on = false;
    int64_t until = 0;
    if (text && cap) text[0] = '\0';
    if (remaining_s) *remaining_s = 0;
    if (!surf.mtx) return false;
    lock();
    on = surf.msg.on;
    until = surf.msg.until_us;
    if (on && text && cap) snprintf(text, cap, "%s", surf.msg_text);
    unlock();
    const int64_t now = esp_timer_get_time();
    if (on && until && now >= until) { /* expired: the surfaces task ends it within a second */
        if (text && cap) text[0] = '\0';
        return false;
    }
    if (on && until && remaining_s) *remaining_s = (uint32_t)((until - now + 999999) / 1000000);
    return on;
}

int surfaces_problem_list(char out[][PROBLEM_LEN], int max)
{
    if (!out || max <= 0 || !surf.mtx) return 0;
    lock();
    int n = surf.problems.n < max ? surf.problems.n : max;
    for (int i = 0; i < n; i++) memcpy(out[i], surf.problems.text[i], PROBLEM_LEN);
    unlock();
    return n;
}
