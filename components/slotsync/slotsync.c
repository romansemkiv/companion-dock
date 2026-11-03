/* The recovery slot (slotsync.h): the sync task, the status, the flash claim and Boot
 * other slot. The decisions are slotsync_logic.c's.
 *
 * Every flash access runs on the sync task (internal stack, as the firmware's tasks that write flash
 * have; priority 1, below every other task), so the public calls never touch flash and work from any
 * task. The task checks the running image once (its length, esp_image_get_metadata, and its SHA-256,
 * esp_partition_get_sha256, which checks the appended SHA-256 against the content), then the other
 * slot, and then every SS_TICK_MS asks ss_decide whether to copy. It checks the other slot again after
 * a failed copy and after a failed upload (slotsync_flash_release), never while the flash is claimed.
 * A running image that can't be read whole means no copy this run (no_source), said as such.
 *
 * Checking the other slot: no image header (erased) -> empty; otadata marks it invalid or aborted, or
 * it fails esp_image_verify (header and chip, segments, checksum, appended SHA-256) -> invalid; an
 * image of another project, or one without an app descriptor -> foreign (shown as "invalid",
 * replaced, never booted); else valid: the same image when the SHA-256s match (verify_other: one pass),
 * newer when its build stamp is the same as or later than the running image's (ss_counts_newer).
 *
 * The copy: esp_ota_begin on the other slot with sequential writes (each sector is erased just before
 * it is written), the running image's bytes read in SS_CHUNK chunks and written in SS_PIECE pieces, a
 * pause of SS_PAUSE_MS between chunks, esp_ota_end (which runs esp_image_verify on it), then the
 * SHA-256s compared. A piece is one 4 KB sector: one sector erase with the cache off at a time (~45 ms
 * typical), not a 64 KB block's, the same pattern as the web upload's 8 KB writes; and at priority 1
 * every other ready task runs between two pieces. The duration, the longest piece and the task's
 * stack high-water mark are logged, for the hardware checks. The boot slot is never changed (no
 * esp_ota_set_boot_partition). esp_ota_begin erases otadata's inactive entry (IDF does that for every
 * OTA write with rollback on): the active entry, which picks the boot slot, stays.
 *
 * Boot other slot: esp_ota_set_boot_partition on the sync task, then the restart on a task of its own
 * (SS_RESTART_STACK, as webui's restart task: esp_restart's shutdown handlers and log lines run on it).
 *
 * The flash owner (s_owner) keeps a copy, the web upload and a restart into the other slot apart: a
 * compare-and-swap from OWN_NONE claims it. */
#include "slotsync/slotsync.h"
#include "rpc/refusals.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "breadcrumb/breadcrumb.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "slotsync_logic.h"

static const char *TAG = "slotsync";

#define SS_STACK 5120 /* internal: it writes flash (high-water mark logged after each copy) */
#define SS_PRIO 1     /* below every other task: they all run between two pieces */
#define SS_TICK_MS 2000
#define SS_CHUNK (64 * 1024) /* read at a time (PSRAM buffer) */
#define SS_PIECE 4096        /* written at a time: one sector erase */
#define SS_PAUSE_MS 100      /* between chunks: the constant to tune (design Part 3) */
#define SS_BOOT_WAIT_MS 10000
#define SS_RESTART_STACK 8192 /* internal: esp_restart and its log lines run on it (webui's restart task) */
#define SS_RESTART_PRIO 6
#define SS_RESTART_DELAY_MS 1000

enum { OWN_NONE, OWN_SYNC, OWN_UPLOAD, OWN_BOOT };

typedef struct {
    bool known;
    ss_other_t other;
    bool same, newer;
    char project[32], version[32], date[16], time[16];
} facts_t;

static const esp_partition_t *s_run, *s_oth;
static uint32_t s_run_len;    /* the running image's length, the appended SHA-256 included */
static uint8_t s_run_sha[32];
static atomic_bool s_run_ok;  /* the running image was read whole (length and SHA-256) */

static SemaphoreHandle_t s_mtx; /* s_facts, s_done, s_fail_us, s_pct */
static StaticSemaphore_t s_mtx_buf;
static facts_t s_facts;
static bool s_done;
static int64_t s_fail_us = -1;
static int s_pct;

static TaskHandle_t s_task;
static atomic_int s_owner;
static atomic_bool s_pending; /* the running image is pending verify (the task reads otadata) */
static atomic_bool s_go;      /* Sync now claimed the flash: the task copies */
static atomic_bool s_rescan;  /* check the other slot again */
static atomic_uint s_gen;     /* bumped by a release: a check that began before it is stale */
static atomic_bool s_boot_req;
static SemaphoreHandle_t s_boot_done;
static StaticSemaphore_t s_boot_done_buf;
static esp_err_t s_boot_err;

static void lock(void) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

static void wake(void)
{
    TaskHandle_t t = s_task;
    if (t) xTaskNotifyGive(t);
}

static bool claim(int who, int *holder)
{
    int expect = OWN_NONE;
    const bool ok = atomic_compare_exchange_strong(&s_owner, &expect, who);
    if (holder) *holder = expect;
    return ok;
}

/* The decision's inputs as they are now (no flash access). */
static void inputs(ss_in_t *in)
{
    const int own = atomic_load(&s_owner);
    lock();
    *in = (ss_in_t){
        .other = s_facts.known ? s_facts.other : SS_OTHER_UNKNOWN,
        .same = s_facts.same,
        .newer = s_facts.newer,
        .pending = atomic_load(&s_pending),
        .flash_busy = own == OWN_UPLOAD || own == OWN_BOOT,
        .copying = own == OWN_SYNC,
        .done = s_done,
        .no_source = !atomic_load(&s_run_ok),
        .uptime_us = esp_timer_get_time(),
        .fail_us = s_fail_us,
    };
    unlock();
}

static esp_err_t why_err(ss_why_t why)
{
    switch (why) {
    case SS_GO: return ESP_OK;
    case SS_E_NOT_READY: return ESP_ERR_NOT_FINISHED;
    case SS_E_SYNCING: return ESP_ERR_INVALID_STATE;
    case SS_E_FLASH_BUSY: return ESP_ERR_NOT_ALLOWED;
    case SS_E_PENDING: return ESP_ERR_OTA_ROLLBACK_INVALID_STATE;
    case SS_E_NEWER: return ESP_ERR_INVALID_VERSION;
    case SS_E_EMPTY: return ESP_ERR_NOT_FOUND;
    case SS_E_INVALID: return ESP_ERR_OTA_VALIDATE_FAILED;
    case SS_E_FOREIGN: return ESP_ERR_NOT_SUPPORTED;
    case SS_E_NO_SOURCE: return ESP_ERR_INVALID_SIZE;
    default: return ESP_FAIL;
    }
}

const char *slotsync_err_text(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ok";
    case ESP_ERR_NOT_FINISHED: return RPC_MSG_CHECKING; /* rpc/refusals.h: the decks read these too */
    case ESP_ERR_INVALID_STATE: return RPC_MSG_SYNCING;
    case ESP_ERR_NOT_ALLOWED: return RPC_MSG_UPLOAD;
    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE: return RPC_MSG_SELF_CHECK;
    case ESP_ERR_INVALID_VERSION: return RPC_MSG_NEWER;
    case ESP_ERR_NOT_FOUND: return RPC_MSG_EMPTY;
    case ESP_ERR_OTA_VALIDATE_FAILED: return RPC_MSG_INVALID;
    case ESP_ERR_NOT_SUPPORTED: return RPC_MSG_FOREIGN;
    case ESP_ERR_INVALID_SIZE: return "this firmware's image can't be read whole: no recovery copy this run (see the log)";
    case ESP_FAIL: return "failed (see the log)";
    case ESP_ERR_NO_MEM: return "no memory for the restart: the other slot starts at the next restart";
    case ESP_ERR_TIMEOUT: return "the sync task did not answer";
    default: return esp_err_to_name(e);
    }
}

/* ---- the sync task's work ---- */

static bool pending_verify(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(s_run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

static unsigned stack_left(void) { return (unsigned)uxTaskGetStackHighWaterMark(NULL); }

static bool check_running(void)
{
    const esp_partition_pos_t pos = {.offset = s_run->address, .size = s_run->size};
    esp_image_metadata_t md;
    esp_err_t e = esp_image_get_metadata(&pos, &md);
    if (e == ESP_OK) e = esp_partition_get_sha256(s_run, s_run_sha);
    if (e != ESP_OK || md.image_len == 0 || md.image_len > s_run->size) {
        ESP_LOGE(TAG, "the running image (%s) can't be read whole (%s): no recovery copy this run", s_run->label,
                 esp_err_to_name(e));
        return false;
    }
    s_run_len = md.image_len;
    return true;
}

static const char *kind_name(const facts_t *f)
{
    if (f->other == SS_OTHER_EMPTY) return "no firmware";
    if (f->other == SS_OTHER_INVALID) return "an invalid firmware";
    if (f->other == SS_OTHER_FOREIGN) return "another project's firmware";
    if (f->same) return "a copy of this firmware";
    return f->newer ? "a newer firmware" : "an older firmware";
}

/* Validates the other slot's image (esp_image_verify: header and chip, segments, checksum, appended
 * SHA-256) and gives its SHA-256, in one pass over the image: esp_image_verify checks the appended
 * SHA-256 against the content, so md.image_digest is it. Only an image without one is hashed again. */
static bool verify_other(uint8_t sha[32])
{
    const esp_partition_pos_t pos = {.offset = s_oth->address, .size = s_oth->size};
    esp_image_metadata_t md;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK) return false;
    if (md.image.hash_appended) {
        memcpy(sha, md.image_digest, 32);
        return true;
    }
    return esp_partition_get_sha256(s_oth, sha) == ESP_OK;
}

static void check_other(void)
{
    const unsigned gen = atomic_load(&s_gen);
    facts_t f = {.known = true, .other = SS_OTHER_EMPTY};
    esp_image_header_t hdr;
    if (esp_partition_read(s_oth, 0, &hdr, sizeof hdr) != ESP_OK) {
        f.other = SS_OTHER_INVALID;
    } else if (hdr.magic == ESP_IMAGE_HEADER_MAGIC) {
        const esp_app_desc_t *run = esp_app_get_description();
        esp_app_desc_t d;
        const bool desc = esp_ota_get_partition_description(s_oth, &d) == ESP_OK;
        if (desc) {
            strlcpy(f.project, d.project_name, sizeof f.project);
            strlcpy(f.version, d.version, sizeof f.version);
            strlcpy(f.date, d.date, sizeof f.date);
            strlcpy(f.time, d.time, sizeof f.time);
        }
        esp_ota_img_states_t st;
        const bool marked = esp_ota_get_state_partition(s_oth, &st) == ESP_OK &&
                            (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED);
        uint8_t sha[32];
        if (marked || !verify_other(sha)) {
            f.other = SS_OTHER_INVALID;
        } else if (!desc || strncmp(d.project_name, run->project_name, sizeof d.project_name) != 0) {
            f.other = SS_OTHER_FOREIGN;
        } else {
            f.other = SS_OTHER_VALID;
            f.same = atomic_load(&s_run_ok) && memcmp(sha, s_run_sha, sizeof sha) == 0;
            f.newer = !f.same && ss_counts_newer(f.date, f.time, run->date, run->time);
        }
    }
    lock();
    const bool fresh = atomic_load(&s_gen) == gen; /* no upload came and went meanwhile */
    if (fresh) s_facts = f;
    unlock();
    if (!fresh) {
        atomic_store(&s_rescan, true);
        return;
    }
    if (f.version[0])
        ESP_LOGI(TAG, "the other slot (%s) holds %s: %s %s, built %s %s", s_oth->label, kind_name(&f), f.project,
                 f.version, f.date, f.time);
    else ESP_LOGI(TAG, "the other slot (%s) holds %s", s_oth->label, kind_name(&f));
}

static void set_pct(int pct)
{
    lock();
    s_pct = pct;
    unlock();
}

static bool facts_known(void)
{
    lock();
    const bool k = s_facts.known;
    unlock();
    return k;
}

/* Copies the running image into the other slot. The caller holds the flash (OWN_SYNC); released here. */
static void copy(void)
{
    if (!facts_known() || !atomic_load(&s_run_ok)) {
        /* an upload came and went between the decision and the claim: decided again at the next tick */
        ESP_LOGW(TAG, "copy dropped: the other slot is being checked again after an upload");
        atomic_store(&s_owner, OWN_NONE);
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    int64_t worst = 0;
    const char *step = "";
    esp_ota_handle_t h = 0;
    bool open = false;
    esp_err_t e = ESP_OK;
    set_pct(0);
    ESP_LOGI(TAG, "copying this firmware (%s, %lu bytes) into the other slot (%s)", s_run->label,
             (unsigned long)s_run_len, s_oth->label);
    uint8_t *buf = heap_caps_malloc(SS_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        e = ESP_ERR_NO_MEM;
        step = "buffer allocation";
        goto out;
    }
    e = esp_ota_begin(s_oth, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        step = "start (esp_ota_begin)";
        goto out;
    }
    open = true;
    for (uint32_t off = 0; off < s_run_len; off += SS_CHUNK) {
        const uint32_t n = s_run_len - off < SS_CHUNK ? s_run_len - off : SS_CHUNK;
        e = esp_partition_read(s_run, off, buf, n);
        if (e != ESP_OK) {
            step = "read";
            goto out;
        }
        for (uint32_t p = 0; p < n; p += SS_PIECE) {
            const int64_t w0 = esp_timer_get_time();
            e = esp_ota_write(h, buf + p, n - p < SS_PIECE ? n - p : SS_PIECE);
            const int64_t w = esp_timer_get_time() - w0;
            if (w > worst) worst = w;
            if (e != ESP_OK) {
                step = "write";
                goto out;
            }
        }
        set_pct((int)((uint64_t)(off + n) * 99 / s_run_len)); /* 100 once verified */
        vTaskDelay(pdMS_TO_TICKS(SS_PAUSE_MS));
    }
    open = false;
    e = esp_ota_end(h); /* validates the image */
    if (e != ESP_OK) {
        step = "validation";
        goto out;
    }
    uint8_t sha[32];
    e = esp_partition_get_sha256(s_oth, sha);
    if (e == ESP_OK && memcmp(sha, s_run_sha, sizeof sha) != 0) e = ESP_ERR_INVALID_CRC;
    if (e != ESP_OK) step = "SHA-256 comparison";
out:
    if (open) esp_ota_abort(h);
    free(buf);
    const int64_t now = esp_timer_get_time();
    if (e == ESP_OK) {
        const esp_app_desc_t *run = esp_app_get_description();
        facts_t f = {.known = true, .other = SS_OTHER_VALID, .same = true};
        strlcpy(f.project, run->project_name, sizeof f.project);
        strlcpy(f.version, run->version, sizeof f.version);
        strlcpy(f.date, run->date, sizeof f.date);
        strlcpy(f.time, run->time, sizeof f.time);
        lock();
        s_facts = f;
        s_done = true;
        s_pct = 100;
        unlock();
        ESP_LOGI(TAG,
                 "the other slot (%s) now holds a copy of this firmware (%s): %.1f s, longest flash write %lld ms, "
                 "sync task stack left %u bytes",
                 s_oth->label, run->version, (double)(now - t0) / 1e6, (long long)(worst / 1000), stack_left());
    } else {
        lock();
        s_fail_us = now;
        s_pct = 0;
        unlock();
        ESP_LOGE(TAG, "copying into the other slot (%s) failed at the %s: %s; next try in 1 h (stack left %u bytes)",
                 s_oth->label, step, esp_err_to_name(e), stack_left());
        check_other(); /* it may hold anything now */
    }
    atomic_store(&s_owner, OWN_NONE);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(SS_RESTART_DELAY_MS)); /* the reply or the screen goes out first */
    breadcrumb_restart(BC_WHY_BOOT_OTHER);
}

/* Boot other slot, for slotsync_boot_other (the flash is held: OWN_BOOT). */
static void boot_other(void)
{
    esp_err_t e = esp_ota_set_boot_partition(s_oth); /* verifies the image again */
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "making the other slot (%s) the boot slot failed: %s (stack left %u bytes)", s_oth->label,
                 esp_err_to_name(e), stack_left());
        atomic_store(&s_owner, OWN_NONE);
    } else {
        ESP_LOGW(TAG, "the other slot (%s) is the boot slot now: restarting into it (stack left %u bytes)",
                 s_oth->label, stack_left());
        breadcrumb_scheduled(BC_WHY_BOOT_OTHER);
        if (xTaskCreate(restart_task, "restart_other", SS_RESTART_STACK, NULL, SS_RESTART_PRIO, NULL) != pdPASS) {
            breadcrumb_clear();
            e = ESP_ERR_NO_MEM; /* the owner stays OWN_BOOT: the boot slot is set, the next restart takes it */
            ESP_LOGE(TAG, "no memory for the restart task: the other slot starts at the next restart");
        }
    }
    s_boot_err = e;
    xSemaphoreGive(s_boot_done);
}

static void sync_task(void *arg)
{
    (void)arg;
    atomic_store(&s_pending, pending_verify());
    atomic_store(&s_run_ok, check_running());
    for (;;) {
        /* pending verify only ever ends within a run: no otadata reads once it reads false */
        if (atomic_load(&s_pending)) atomic_store(&s_pending, pending_verify());
        if (atomic_exchange(&s_boot_req, false)) boot_other();
        if (atomic_load(&s_owner) == OWN_NONE) {
            const bool again = atomic_exchange(&s_rescan, false);
            if (!facts_known() || again) check_other();
        }
        if (atomic_exchange(&s_go, false)) {
            copy(); /* Sync now: slotsync_start claimed the flash */
        } else {
            ss_in_t in;
            inputs(&in);
            if (ss_decide(&in, SS_AUTO).copy && claim(OWN_SYNC, NULL)) copy();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SS_TICK_MS));
    }
}

/* ---- the public calls ---- */

esp_err_t slotsync_init(void)
{
    if (s_task) return ESP_OK;
    s_run = esp_ota_get_running_partition();
    s_oth = esp_ota_get_next_update_partition(NULL);
    if (!s_mtx) s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    if (!s_boot_done) s_boot_done = xSemaphoreCreateBinaryStatic(&s_boot_done_buf);
    if (!s_run || !s_oth) {
        ESP_LOGE(TAG, "no other app slot: no recovery copy");
        return ESP_ERR_NOT_FOUND;
    }
    if (xTaskCreate(sync_task, "slotsync", SS_STACK, NULL, SS_PRIO, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void slotsync_get(slotsync_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->state = SLOTSYNC_WAITING;
    if (!s_mtx || !s_run) return;
    strlcpy(out->running, s_run->label, sizeof out->running);
    if (!s_oth) return;
    strlcpy(out->other, s_oth->label, sizeof out->other);
    ss_in_t in;
    inputs(&in);
    out->state = ss_decide(&in, SS_AUTO).state;
    lock();
    strlcpy(out->other_project, s_facts.project, sizeof out->other_project);
    strlcpy(out->other_version, s_facts.version, sizeof out->other_version);
    strlcpy(out->other_date, s_facts.date, sizeof out->other_date);
    strlcpy(out->other_time, s_facts.time, sizeof out->other_time);
    out->pct = out->state == SLOTSYNC_SYNCING ? s_pct : out->state == SLOTSYNC_IN_SYNC ? 100 : 0;
    unlock();
    out->other_foreign = in.other == SS_OTHER_FOREIGN;
    out->other_valid = in.other == SS_OTHER_VALID;
    out->flash_busy = in.flash_busy;
}

esp_err_t slotsync_start(bool force)
{
    if (!s_task) return ESP_ERR_NOT_FOUND;
    ss_in_t in;
    inputs(&in);
    const ss_out_t o = ss_decide(&in, force ? SS_FORCE : SS_NOW);
    if (!o.copy) return why_err(o.why);
    int holder;
    if (!claim(OWN_SYNC, &holder)) return holder == OWN_SYNC ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_ALLOWED;
    ESP_LOGI(TAG, "Sync now%s", force ? " (force)" : "");
    atomic_store(&s_go, true);
    wake();
    return ESP_OK;
}

esp_err_t slotsync_boot_other(void)
{
    if (!s_task) return ESP_ERR_NOT_FOUND;
    ss_in_t in;
    inputs(&in);
    const ss_why_t why = ss_boot_other(&in);
    if (why != SS_GO) return why_err(why);
    int holder;
    if (!claim(OWN_BOOT, &holder)) return holder == OWN_SYNC ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_ALLOWED;
    xSemaphoreTake(s_boot_done, 0); /* no stale result */
    atomic_store(&s_boot_req, true);
    wake();
    if (xSemaphoreTake(s_boot_done, pdMS_TO_TICKS(SS_BOOT_WAIT_MS)) != pdTRUE) {
        if (atomic_exchange(&s_boot_req, false)) { /* not taken up: nothing changed */
            atomic_store(&s_owner, OWN_NONE);
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreTake(s_boot_done, portMAX_DELAY); /* taken up: it ends soon */
    }
    return s_boot_err;
}

esp_err_t slotsync_flash_claim(void)
{
    int holder;
    if (claim(OWN_UPLOAD, &holder)) return ESP_OK;
    return holder == OWN_SYNC ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_ALLOWED;
}

void slotsync_flash_release(void)
{
    int expect = OWN_UPLOAD;
    if (!atomic_compare_exchange_strong(&s_owner, &expect, OWN_NONE)) return;
    if (s_mtx) {
        lock();
        atomic_fetch_add(&s_gen, 1);
        s_facts.known = false; /* the upload may have left a partial image */
        s_done = false;        /* it destroyed the copy: restoring it is not a second copy */
        unlock();
    }
    atomic_store(&s_rescan, true);
    wake();
}
