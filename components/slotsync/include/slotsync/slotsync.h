/* The recovery slot: the box keeps a working copy of the running firmware in the other
 * app slot (ota_0 / ota_1).
 *
 * Automatic: once the running image has been up SS_DELAY_US (10 min), is not pending verify, no
 * firmware upload holds the flash, and the other slot holds no image, an invalid or aborted one,
 * another project's or an older one, a low-priority task copies the running image into it
 * through esp_ota_begin / write / end (which validates it) and compares both images' SHA-256. It never
 * changes the boot slot (no esp_ota_set_boot_partition). Once per boot after a success; a failure is
 * logged and retried after SS_RETRY_US (1 h). A valid image of this project whose build stamp (app
 * descriptor date and time) is the same as or later than the running one's, e.g. one written
 * over USB, is never overwritten automatically: "other_newer", and Boot other slot offers it. Another
 * project's image is never booted from here. slotsync.c has the details (chunks, pauses, what the
 * bootloader does with the copy).
 *
 * The part above ESP_PLATFORM is pure (no ESP-IDF header): slotsync_logic.c and the host tests use
 * it. */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLOTSYNC_IN_SYNC,     /* "in_sync": the other slot holds this very image (same SHA-256) */
    SLOTSYNC_SYNCING,     /* "syncing": a copy runs (pct) */
    SLOTSYNC_DIFFERS,     /* "differs": a valid image built earlier (a sync replaces it) */
    SLOTSYNC_OTHER_NEWER, /* "other_newer": a valid image built in the same second or later, or undatable:
                           * never overwritten without force */
    SLOTSYNC_EMPTY,       /* "empty": no image (erased) */
    SLOTSYNC_INVALID,     /* "invalid": an image that fails verification, one marked invalid/aborted (a
                           * failed update after a rollback), or another project's (other_foreign):
                           * replaceable, never booted */
    SLOTSYNC_WAITING,     /* "waiting": a copy is due but held: the first 10 minutes after the start,
                           * pending verify, a firmware upload, the 1 h back-off after a failure, or
                           * the slots not checked yet (the first seconds after the start) */
} slotsync_state_t;

/* "in_sync", "syncing", "differs", "other_newer", "empty", "invalid", "waiting" (Ota.GetStatus). */
const char *slotsync_state_name(slotsync_state_t s);

typedef struct {
    slotsync_state_t state;
    int pct;                  /* the copy's progress, 0-100, while syncing; 100 in sync; else 0 */
    char running[17];         /* the running slot's label ("ota_1") */
    char other[17];           /* the other slot's label ("ota_0"); "" when the table has none */
    char other_project[32];   /* the other slot's app descriptor: project name, "" when none can be read */
    char other_version[32];   /* its version */
    char other_date[16];      /* its build date ("Nov  1 2026") */
    char other_time[16];      /* its build time ("14:02:11") */
    bool other_foreign;       /* another project's image (or one without an app descriptor): "invalid" */
    bool other_valid;         /* a valid image of this project: slotsync_boot_other may take it (it
                               * still refuses while a sync, an upload or pending verify runs) */
    bool flash_busy;          /* a firmware upload holds the flash, or a restart into the other slot is on its
                               * way: a sync now is refused (the decks' Sync says "wait") */
} slotsync_status_t;

#ifdef ESP_PLATFORM
#include "esp_err.h"

/* Starts the sync task (internal stack, priority 1). Call once, at the end of the boot. Until it has
 * checked both slots (a second or so), slotsync_get says "waiting". If the running image can't be read
 * whole, no copy is made this run (logged; slotsync_start says so) and the state says what the other
 * slot holds. */
esp_err_t slotsync_init(void);

/* The state now (cheap: no flash access; any task). */
void slotsync_get(slotsync_status_t *out);

/* Sync now (Ota.Sync, the deck's Sync now): starts a copy whatever the 10-minute timer, the 1 h
 * back-off and "once per boot" say, also when the slots are in sync. Returns at once; the copy runs
 * on the sync task (slotsync_get: "syncing", pct). force: also over a newer image.
 *   ESP_OK                              started
 *   ESP_ERR_NOT_FINISHED                the slots are still being checked (a second after the
 *                                       start, or right after a failed upload)
 *   ESP_ERR_INVALID_STATE               a sync runs already
 *   ESP_ERR_NOT_ALLOWED                 a firmware upload holds the flash, or the box is restarting
 *                                       into the other slot
 *   ESP_ERR_OTA_ROLLBACK_INVALID_STATE  the running image is pending verify (esp_ota_ops.h)
 *   ESP_ERR_INVALID_VERSION             the other slot holds a newer image (force overrides)
 *   ESP_ERR_INVALID_SIZE                the running image can't be read whole: no copy this run
 *   ESP_ERR_NOT_FOUND                   no other app slot in the partition table
 * slotsync_err_text says each in words. */
esp_err_t slotsync_start(bool force);

/* Boot other slot (Ota.BootOther, the deck): makes the other slot the boot slot (on the sync task:
 * esp_ota_set_boot_partition, which verifies the image again), then a restart task of its own (8 KB,
 * internal, priority 6, as webui's) restarts the box about a second later, so a reply or a screen can
 * go out first (breadcrumb "boot other slot"). Blocks for that, normally a few hundred ms. The image
 * then starts like an update: pending verify, the self-check, and a rollback to this slot if it fails.
 * Refused (nothing changed) with ESP_ERR_NOT_FINISHED, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_ALLOWED and
 * ESP_ERR_OTA_ROLLBACK_INVALID_STATE as above, ESP_ERR_NOT_FOUND (no image in the other slot, or no
 * other slot), ESP_ERR_OTA_VALIDATE_FAILED (an invalid or aborted image), ESP_ERR_NOT_SUPPORTED
 * (another project's image), ESP_ERR_TIMEOUT (the sync task didn't take the request up within
 * 10 s), or another error of esp_ota_set_boot_partition (e.g. ESP_FAIL: writing otadata failed).
 * ESP_ERR_NO_MEM: no task for the restart; the boot slot is set, so the next restart takes it
 * (uploads and syncs stay refused until then). */
esp_err_t slotsync_boot_other(void);

/* The web OTA upload (POST /ota) and a sync exclude each other. claim before writing the other slot:
 * ESP_OK, or ESP_ERR_INVALID_STATE while a sync runs (the upload answers 409), ESP_ERR_NOT_ALLOWED
 * while a restart into the other slot is on its way. release when the upload failed or was refused
 * after the claim: the slots are checked again (the other one may hold a partial image now) and a
 * held sync goes on. An upload that succeeded keeps the claim until the restart. */
esp_err_t slotsync_flash_claim(void);
void slotsync_flash_release(void);

/* A short sentence for the codes above ("a firmware upload is running", ...), for RPC errors and
 * screens. */
const char *slotsync_err_text(esp_err_t e);
#endif

#ifdef __cplusplus
}
#endif
