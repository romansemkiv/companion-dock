/* ESP: Network Dock mode glue (7.3). Each deck gets a dock (elgdock), whose port presents
 * the deck itself; while Companion has a connection to it, it drives the deck, otherwise the
 * deck shows the box's maintenance screens (surfaces.c).
 *
 * Everything Companion sends to a deck with a display goes through that deck's image cache and gate
 * (dockcache.h), on the dock's own task (elgdock's on_traffic): it keeps the last complete image of
 * every target in PSRAM and, while a custom message is shown, holds Companion's key images back and
 * writes the box's message frames (rendered by surfaces' imgwork, posted here) in between Companion's
 * images: on a deck with too few keys for the message, the frames of its next page every 3 s (the
 * surfaces task turns the pages of the dock screen and the message, in both modes). When the message
 * ends, or a quarantine of the deck ends, the cache is written back; a page's frame that comes after
 * that is dropped (dc_box_key keeps nothing without the hold), so it never overwrites the replay.
 * The maintenance screen on demand works the same way, with the strip and the info bar held too
 * (dc_set_hold_all) and the box's frames for them; its input stays with the box meanwhile
 * (elgdock_hold_input).
 *
 * Locks: dm.mtx guards the table and what other tasks post (message frames, a replay request), held
 * briefly. A deck's `work` mutex is held by its dock's task over every dockcache call (which may
 * write to the deck and wait on it) and by attach / detach / on_owner around (re)setting the cache;
 * it is taken after elgdock's usb_mtx in on_owner and before it in on_traffic, on the same task, so
 * the two never meet across tasks (detach takes it after elgdock_release, holding nothing).
 *
 * A new Companion connection to a deck nobody drove starts with an empty cache; one that replaces the
 * connection being served (a hand-over: on_owner(true) again, no on_owner(false) between) keeps the
 * cache and the message frames, as the deck still shows them, and only drops the images the old
 * connection had under way. */
#include <stdatomic.h>
#include <string.h>
#include "dockcache.h"
#include "elgdock/elgdock.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "surfaces/surfaces.h"
#include "surfaces_priv.h"

#define TAG "dockmode"

/* The box's frames posted for a dock's task (images as usbdeck_write_key / _lcd / _info take them). */
typedef struct {
    uint8_t *key[DC_KEYS_MAX];
    size_t key_len[DC_KEYS_MAX];
    uint8_t *lcd[DC_LCD_MAX]; /* strip regions */
    size_t lcd_len[DC_LCD_MAX];
    uint16_t lcd_r[DC_LCD_MAX][4]; /* x, y, w, h */
    uint8_t *info;
    size_t info_len;
    bool bright_set; /* the box's brightness for the deck on demand covers */
    uint8_t bright;
    bool any; /* something above is posted (else on_traffic has nothing to take, apply or free) */
} posts_t;

typedef struct {
    usbdeck_t *d; /* dm.mtx; NULL: free */
    bool ready;   /* dm.mtx; the cache is set up (else the entry is only reserved) */
    bool owned;   /* dm.mtx; a Companion connection drives the deck */
    int dock;     /* dm.mtx; d's dock (elgdock_assign at dockmode_attach: d keeps it until elgdock_release) */
    SemaphoreHandle_t work;
    dc_t *dc; /* PSRAM; work */
    /* dm.mtx: posted for the dock's task */
    posts_t post; /* the box's frames */
    bool replay;  /* a quarantine ended */
    bool settled; /* dm.mtx: nothing of the box's is on the deck or waits (Companion's images all back) */
    /* work */
    bool log_drawn, log_end, log_replay;
} dm_deck_t;

static struct {
    SemaphoreHandle_t mtx;
    atomic_bool msg;   /* a custom message is shown */
    _Atomic(const usbdeck_t *) maint[SURF_MAX_DECKS]; /* the decks the box menu is open on, NULL none */
    dm_deck_t *deck; /* SURF_MAX_DECKS entries in PSRAM (dockmode_start): plain fields under dm.mtx or work, the
                      * dock tasks' and the surfaces task's (PSRAM stacks already), never an ISR or DMA */
} dm;

static void *mem_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void *mem_resize(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
static const dc_mem_t MEM = {.alloc = mem_alloc, .resize = mem_resize, .release = heap_caps_free};

static void lock(void) { xSemaphoreTake(dm.mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(dm.mtx); }

/* d's entry (ready: only once its cache is set up). */
static dm_deck_t *entry_locked(const usbdeck_t *d, bool ready)
{
    for (int i = 0; i < SURF_MAX_DECKS; i++)
        if (d && dm.deck[i].d == d && (dm.deck[i].ready || !ready)) return &dm.deck[i];
    return NULL;
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool deck_write(void *ctx, bool feature, const uint8_t *report, size_t len)
{
    return elgdock_write(ctx, feature, report, len);
}

/* Takes what was posted for e (dm.mtx held) into out (its buffers are the caller's to free). */
static void take_posts_locked(dm_deck_t *e, posts_t *out, bool *replay)
{
    *out = e->post;
    memset(&e->post, 0, sizeof e->post);
    if (replay) *replay = e->replay;
    e->replay = false;
}

static void free_posts(posts_t *p)
{
    for (int k = 0; k < DC_KEYS_MAX; k++) heap_caps_free(p->key[k]);
    for (int i = 0; i < DC_LCD_MAX; i++) heap_caps_free(p->lcd[i]);
    heap_caps_free(p->info);
    memset(p, 0, sizeof *p);
}

/* Logs what a message or a replay did, once the deck has all of it (work held). */
static void log_progress(dm_deck_t *e, int k)
{
    dc_t *c = e->dc;
    if (!dc_settled(c)) return;
    if (e->log_drawn && c->hold && c->stats.box_frames >= (unsigned)c->m->cols * c->m->rows) {
        ESP_LOGI(TAG, "dock %d: message drawn (%u frames)", k, (unsigned)c->stats.box_frames);
        e->log_drawn = false;
    }
    if (e->log_end && !c->hold) {
        ESP_LOGI(TAG, "dock %d: message over: %u Companion reports were held (%u complete images, %u not kept); "
                      "%u targets written back from the cache, each its latest; %u bytes cached",
                 k, (unsigned)c->stats.held_reports, (unsigned)c->stats.held_images, (unsigned)c->stats.uncached,
                 (unsigned)c->stats.replayed, (unsigned)dc_bytes(c));
        memset(&c->stats, 0, sizeof c->stats);
        e->log_end = false;
    }
    if (e->log_replay) {
        ESP_LOGI(TAG, "dock %d: quarantine over: images written back from the cache", k);
        e->log_replay = false;
    }
}

/* elgdock, on the dock's task: Companion's report for d, or a quiet moment (report NULL). */
static bool on_traffic(usbdeck_t *d, bool feature, const uint8_t *report, size_t len)
{
    lock();
    dm_deck_t *e = entry_locked(d, true);
    unlock();
    if (!e) return true; /* no cache (no display, or no memory): passed on as it is */
    xSemaphoreTake(e->work, portMAX_DELAY);
    posts_t post;
    post.any = false; /* the rest only when there are posts (take_posts_locked copies them all) */
    bool replay = false;
    int k = -1;
    lock();
    const bool mine = e->d == d && e->ready; /* not detached meanwhile */
    if (mine) {
        k = e->dock; /* kept at attach, not looked up in elgdock (its lock) for every report */
        if (e->post.any) {
            take_posts_locked(e, &post, &replay);
        } else {
            replay = e->replay;
            e->replay = false;
        }
    }
    unlock();
    if (!mine) {
        xSemaphoreGive(e->work);
        return true;
    }
    dc_t *c = e->dc;

    bool maint = false;
    for (int i = 0; i < SURF_MAX_DECKS && !maint; i++) maint = atomic_load(&dm.maint[i]) == d;
    const bool hold = atomic_load(&dm.msg) || maint;
    if (!maint && c->hold_all) { /* on demand closed: the strip and the info bar first */
        ESP_LOGI(TAG, "dock %d: maintenance screen closed: Companion's images come back", k);
        dc_set_hold_all(c, false);
    }
    if (hold != c->hold) {
        if (hold) {
            ESP_LOGI(TAG, "dock %d: %s: Companion's images held back meanwhile", k,
                     maint ? "maintenance screen on demand" : "message shown");
            e->log_drawn = true;
        } else {
            ESP_LOGI(TAG, "dock %d: box frames over: writing the cache back", k);
            e->log_end = true;
        }
        dc_set_hold(c, hold);
    }
    if (maint && !c->hold_all) dc_set_hold_all(c, true);
    if (post.any) {
        for (int i = 0; i < DC_KEYS_MAX; i++)
            if (post.key[i] && !dc_box_key(c, (uint8_t)i, post.key[i], post.key_len[i]) && c->hold)
                ESP_LOGW(TAG, "dock %d: box frame for key %d not kept", k, i);
        for (int i = 0; i < DC_LCD_MAX; i++)
            if (post.lcd[i] && !dc_box_lcd(c, post.lcd_r[i][0], post.lcd_r[i][1], post.lcd_r[i][2], post.lcd_r[i][3],
                                           post.lcd[i], post.lcd_len[i]) && c->hold_all)
                ESP_LOGW(TAG, "dock %d: box frame for the strip at x %u not kept", k, (unsigned)post.lcd_r[i][0]);
        if (post.info && !dc_box_info(c, post.info, post.info_len) && c->hold_all)
            ESP_LOGW(TAG, "dock %d: box frame for the info bar not kept", k);
        if (post.bright_set && !dc_box_brightness(c, post.bright) && c->hold_all)
            ESP_LOGW(TAG, "dock %d: the box's brightness not kept", k);
        free_posts(&post);
    }
    const bool was_settled = e->settled;
    if (replay) {
        dc_replay_all(c);
        e->log_replay = true;
    }

    const dc_io_t io = {.write = deck_write, .ctx = d, .now_ms = now_ms()};
    if (!report)
        dc_pump(c, &io);
    else if (feature)
        dc_feature(c, &io, report, len);
    else
        dc_output(c, &io, report, len);
    log_progress(e, k);
    const bool settled = !c->hold && dc_settled(c);
    xSemaphoreGive(e->work);
    if (settled != was_settled) {
        lock();
        e->settled = settled;
        unlock();
    }
    return false; /* taken care of (elgdock checks whether the connection goes on) */
}

/* A fresh cache for a new Companion connection or for none: the deck shows the box's screens, not
 * Companion's images, so nothing kept is the deck's any more. */
static void reset_cache(usbdeck_t *d)
{
    lock();
    dm_deck_t *e = entry_locked(d, true);
    posts_t post;
    memset(&post, 0, sizeof post);
    if (e) take_posts_locked(e, &post, NULL);
    unlock();
    free_posts(&post);
    if (!e) return;
    xSemaphoreTake(e->work, portMAX_DELAY);
    dc_reset(e->dc);
    dc_set_hold_all(e->dc, false);
    dc_set_hold(e->dc, false); /* the next on_traffic takes up a message or the maintenance screen shown */
    lock();
    e->settled = true; /* nothing kept to write back */
    unlock();
    e->log_drawn = e->log_end = e->log_replay = false;
    xSemaphoreGive(e->work);
}

/* Companion's last key images, taken before its cache is reset (it let go of the deck): NULL when nothing is kept.
 * On the dock's task, under the deck's work mutex. */
static surf_dock_snap_t *snap_keys(dm_deck_t *e)
{
    surf_dock_snap_t *s = heap_caps_calloc(1, sizeof *s, MALLOC_CAP_SPIRAM);
    if (!s) return NULL;
    bool any = false;
    xSemaphoreTake(e->work, portMAX_DELAY);
    const deck_model_t *m = e->dc->m;
    const unsigned n = m ? (unsigned)m->cols * m->rows : 0;
    for (unsigned k = 0; k < n && k < SURF_SNAP_KEYS; k++) {
        const size_t cap = dc_key_image_cap(e->dc, (uint8_t)k);
        uint8_t *buf = cap ? heap_caps_malloc(cap, MALLOC_CAP_SPIRAM) : NULL;
        size_t len = 0;
        const dc_kind_t kind = dc_key_image(e->dc, (uint8_t)k, buf, buf ? cap : 0, &len, s->rgb[k]);
        s->kind[k] = (uint8_t)kind;
        if (kind == DC_IMAGE) {
            s->img[k] = buf;
            s->len[k] = len;
        } else {
            heap_caps_free(buf);
        }
        any = any || kind != DC_NONE;
    }
    xSemaphoreGive(e->work);
    if (any) return s;
    surf_dock_snap_free(s);
    return NULL;
}

/* On the dock's task, under its usb_mtx: d stays (elgdock_release waits for it). */
static void on_owner(usbdeck_t *d, bool companion, const char *peer_ip)
{
    const int k = elgdock_dock_of(d);
    lock();
    dm_deck_t *e = entry_locked(d, true); /* elgdock runs only once dm.mtx is there */
    const bool handover = e && companion && e->owned;
    if (e) e->owned = companion;
    unlock();
    surf_dock_snap_t *snap = NULL;
    if (handover) { /* the deck still shows what the cache and the frames say */
        ESP_LOGI(TAG, "dock %d: Companion at %s takes the deck over from the connection before (cache kept)", k,
                 peer_ip ? peer_ip : "?");
        xSemaphoreTake(e->work, portMAX_DELAY);
        dc_end_sequences(e->dc);
        xSemaphoreGive(e->work);
    } else {
        if (companion)
            ESP_LOGI(TAG, "dock %d: Companion at %s drives the deck", k, peer_ip ? peer_ip : "?");
        else
            ESP_LOGI(TAG, "dock %d: Companion let go of the deck", k);
        if (!companion && e) snap = snap_keys(e); /* what the deck shows, for "Reconnecting" over it */
        reset_cache(d);
    }
    surfaces_dock_owner(d, companion, peer_ip, snap); /* a no-op on a hand-over: the slot stays owned */
}

esp_err_t dockmode_start(const char *box_name, const uint8_t mac[6], bool announce, int ndocks)
{
    if (!dm.deck) dm.deck = heap_caps_calloc(SURF_MAX_DECKS, sizeof *dm.deck, MALLOC_CAP_SPIRAM);
    if (!dm.deck) dm.deck = heap_caps_calloc(SURF_MAX_DECKS, sizeof *dm.deck, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dm.deck) return ESP_ERR_NO_MEM; /* dm.mtx stays NULL: every entry point does nothing */
    for (int i = 0; i < SURF_MAX_DECKS; i++)
        if (!(dm.deck[i].work = xSemaphoreCreateMutex())) return ESP_ERR_NO_MEM;
    SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
    if (!mtx) return ESP_ERR_NO_MEM;
    dm.mtx = mtx; /* last: set means every mutex is there (attach / detach check it) */
    const elgdock_cb_t cb = {.on_owner = on_owner, .on_traffic = on_traffic, .on_corner = surfaces_dock_corner};
    esp_err_t e = elgdock_start(box_name, mac, &cb, ndocks);
    if (e == ESP_OK && !announce) elgdock_set_announce(box_name, false);
    return e;
}

void dockmode_attach(usbdeck_t *d, const deck_model_t *m, const char *serial)
{
    const int k = elgdock_assign(d, m, serial);
    if (k < 0 || !dm.mtx || !deck_model_has_display(m)) return;
    lock();
    dm_deck_t *e = entry_locked(d, false);
    for (int i = 0; i < SURF_MAX_DECKS && !e; i++)
        if (!dm.deck[i].d) e = &dm.deck[i];
    if (e) { /* reserved: no other deck takes it, on_traffic doesn't use it yet */
        e->d = d;
        e->ready = false;
        e->owned = false;
        e->settled = true;
        e->dock = k;
    }
    unlock();
    if (!e) return;
    xSemaphoreTake(e->work, portMAX_DELAY);
    if (e->dc)
        dc_reset(e->dc); /* whatever an earlier deck left */
    else
        e->dc = heap_caps_calloc(1, sizeof *e->dc, MALLOC_CAP_SPIRAM);
    if (e->dc) dc_init(e->dc, m, &MEM);
    xSemaphoreGive(e->work);
    lock();
    if (e->dc)
        e->ready = true;
    else
        e->d = NULL;
    unlock();
    if (!e->dc) {
        ESP_LOGW(TAG, "dock %d: no memory for an image cache: no messages over Companion", k);
        return;
    }
    ESP_LOGI(TAG, "dock %d: image cache for %s: up to %u KB of PSRAM", k, m->name,
             (unsigned)(dc_bound_bytes(m) / 1024));
}

void dockmode_detach(usbdeck_t *d)
{
    elgdock_release(d); /* first: the dock no longer uses d (on_traffic's writes now fail) */
    if (!dm.mtx) return; /* dock mode never got going: no caches */
    posts_t post;
    memset(&post, 0, sizeof post);
    lock();
    dm_deck_t *e = entry_locked(d, false);
    if (e) {
        take_posts_locked(e, &post, NULL);
        e->d = NULL;
        e->ready = false;
        e->owned = false;
    }
    unlock();
    free_posts(&post);
    if (!e) return;
    xSemaphoreTake(e->work, portMAX_DELAY); /* on_traffic has left it */
    lock();
    const bool unused = e->d == NULL; /* else another deck took it meanwhile, and set it up anew */
    unlock();
    if (unused && e->dc) dc_reset(e->dc); /* the PSRAM back */
    xSemaphoreGive(e->work);
}

/* Images sent to it while it was quarantined were dropped: what Companion drew comes back from the
 * cache (images and fills; the message's frames if one is shown). surfaces redraws the box's own
 * screen when Companion doesn't own the deck. */
void dockmode_restored(usbdeck_t *d)
{
    if (!dm.mtx) return;
    lock();
    dm_deck_t *e = entry_locked(d, true);
    if (e) e->replay = true;
    unlock();
    ESP_LOGI(TAG, "dock %d: the deck's quarantine is over%s", elgdock_dock_of(d),
             e ? ": Companion's images come back from the cache" : "");
}

void dockmode_message(bool on) { atomic_store(&dm.msg, on); }

void dockmode_maint(const usbdeck_t *const *d, int n)
{
    for (int i = 0; i < SURF_MAX_DECKS; i++) atomic_store(&dm.maint[i], i < n ? d[i] : NULL);
}

/* A copy of img, or NULL (logged). */
static uint8_t *copy_frame(const uint8_t *img, size_t len)
{
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) ESP_LOGW(TAG, "no memory for a box frame");
    else memcpy(copy, img, len);
    return copy;
}

void dockmode_box_key(usbdeck_t *d, uint8_t hw, const uint8_t *img, size_t len)
{
    if (!dm.mtx || hw >= DC_KEYS_MAX || !img || !len) return;
    uint8_t *copy = copy_frame(img, len);
    if (!copy) return;
    lock();
    dm_deck_t *e = entry_locked(d, true);
    uint8_t *old = copy;
    if (e) {
        old = e->post.key[hw];
        e->post.key[hw] = copy;
        e->post.key_len[hw] = len;
        e->post.any = true;
    }
    unlock();
    heap_caps_free(old);
}

void dockmode_box_lcd(usbdeck_t *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *img, size_t len)
{
    if (!dm.mtx || !img || !len) return;
    uint8_t *copy = copy_frame(img, len);
    if (!copy) return;
    lock();
    dm_deck_t *e = entry_locked(d, true);
    uint8_t *old = copy;
    if (e) {
        int at = -1; /* the region's post, else a free one */
        for (int i = 0; i < DC_LCD_MAX && at < 0; i++)
            if (e->post.lcd[i] && e->post.lcd_r[i][0] == x && e->post.lcd_r[i][1] == y && e->post.lcd_r[i][2] == w &&
                e->post.lcd_r[i][3] == h)
                at = i;
        for (int i = 0; i < DC_LCD_MAX && at < 0; i++)
            if (!e->post.lcd[i]) at = i;
        if (at >= 0) {
            old = e->post.lcd[at];
            e->post.lcd[at] = copy;
            e->post.lcd_len[at] = len;
            e->post.lcd_r[at][0] = x;
            e->post.lcd_r[at][1] = y;
            e->post.lcd_r[at][2] = w;
            e->post.lcd_r[at][3] = h;
            e->post.any = true;
        }
    }
    unlock();
    heap_caps_free(old);
}

void dockmode_box_info(usbdeck_t *d, const uint8_t *img, size_t len)
{
    if (!dm.mtx || !img || !len) return;
    uint8_t *copy = copy_frame(img, len);
    if (!copy) return;
    lock();
    dm_deck_t *e = entry_locked(d, true);
    uint8_t *old = copy;
    if (e) {
        old = e->post.info;
        e->post.info = copy;
        e->post.info_len = len;
        e->post.any = true;
    }
    unlock();
    heap_caps_free(old);
}

void dockmode_box_brightness(usbdeck_t *d, uint8_t pct)
{
    if (!dm.mtx) return;
    lock();
    dm_deck_t *e = entry_locked(d, true);
    if (e) {
        e->post.bright_set = true;
        e->post.bright = pct;
        e->post.any = true;
    }
    unlock();
}

uint16_t dockmode_port(const usbdeck_t *d)
{
    const int k = elgdock_dock_of(d);
    return k < 0 ? 0 : (uint16_t)ELGDOCK_PRIMARY_PORT(k);
}

void dockmode_hold_input(const char *const *serials, int n) { elgdock_hold_input(serials, n); }

bool dockmode_settled(const usbdeck_t *d)
{
    if (!dm.mtx) return true;
    lock();
    const dm_deck_t *e = entry_locked(d, true);
    const bool done = !e || !e->owned || e->settled; /* not Companion's: nothing of its to bring back */
    unlock();
    return done;
}

void dockmode_apply(const char *box_name, bool announce) { elgdock_set_announce(box_name, announce); }
