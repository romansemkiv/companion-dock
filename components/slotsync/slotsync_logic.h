#pragma once
/* The recovery slot's decisions (pure: no ESP-IDF header; host-tested in
 * test/host/test_slotsync_logic.c). slotsync.c feeds them what the partitions, the clock and the OTA
 * upload say, and acts on the answer. */
#include <stdbool.h>
#include <stdint.h>

#include "slotsync/slotsync.h"

#define SS_DELAY_US (10 * 60 * 1000000LL) /* the automatic sync waits for this much uptime */
#define SS_RETRY_US (60 * 60 * 1000000LL) /* and this long after a failed copy */

/* What the other slot holds. */
typedef enum {
    SS_OTHER_UNKNOWN, /* not checked yet (the first second after the start, or after an upload failed) */
    SS_OTHER_EMPTY,   /* no image header: erased */
    SS_OTHER_INVALID, /* an image that fails verification, or one otadata marks invalid or aborted */
    SS_OTHER_FOREIGN, /* a valid image of another project, or one without an app descriptor:
                       * reported as "invalid", replaceable, never booted */
    SS_OTHER_VALID,
} ss_other_t;

typedef struct {
    ss_other_t other;
    bool same;          /* VALID: its SHA-256 is the running image's */
    bool newer;         /* VALID: not built earlier than the running image (ss_counts_newer) */
    bool pending;       /* the running image is pending verify */
    bool flash_busy;    /* a firmware upload holds the flash, or a restart into the other slot is on
                         * its way */
    bool copying;       /* a copy runs */
    bool done;          /* a copy succeeded since this start (cleared when a failed upload destroyed it) */
    bool no_source;     /* the running image can't be read whole: no copy this run */
    int64_t uptime_us;
    int64_t fail_us;    /* the uptime of the last failed copy; -1: none since this start */
} ss_in_t;

typedef enum {
    SS_AUTO,  /* the sync task's own check */
    SS_NOW,   /* Sync now */
    SS_FORCE, /* Sync now, also over a newer image */
} ss_req_t;

typedef enum {
    SS_GO,           /* copy now */
    SS_NOT_DUE,      /* SS_AUTO: no copy (nothing to do, or held: the state says which) */
    SS_E_NOT_READY,  /* the other slot isn't checked yet */
    SS_E_SYNCING,    /* a copy runs */
    SS_E_FLASH_BUSY, /* an upload holds the flash, or a restart into the other slot is on its way */
    SS_E_PENDING,    /* the running image is pending verify */
    SS_E_NEWER,      /* Sync now over a newer image, without force */
    SS_E_EMPTY,      /* Boot other: no image there */
    SS_E_INVALID,    /* Boot other: an invalid or aborted image */
    SS_E_FOREIGN,    /* Boot other: another project's firmware */
    SS_E_NO_SOURCE,  /* Sync now: the running image can't be read whole */
} ss_why_t;

typedef struct {
    slotsync_state_t state;
    bool copy; /* start a copy now (why == SS_GO) */
    ss_why_t why;
} ss_out_t;

/* The state, and whether to copy now.
 * State: a copy running is "syncing"; the same image "in_sync"; a newer valid one "other_newer".
 * Otherwise a copy is due, and while it is held (not checked yet, pending verify, the flash busy,
 * uptime under SS_DELAY_US, or under SS_RETRY_US since the last failure) the state is "waiting";
 * when not, it says what the slot holds ("empty", "invalid" (a foreign image too), "differs"). With
 * no_source nothing is held, as no copy will come: the state says what the slot holds.
 * SS_AUTO copies when a copy is due, not held, none succeeded since the start (once per boot) and the
 * running image can be read. SS_NOW / SS_FORCE ignore the timer, the back-off and "once per boot"
 * (and copy over the same image too), but are refused while a copy runs, the flash is busy, the
 * running image is pending verify, the slot isn't checked yet or the running image can't be read,
 * and SS_NOW over a newer image. */
ss_out_t ss_decide(const ss_in_t *in, ss_req_t req);

/* Boot other slot: SS_GO when the other slot holds a valid image of this project and no copy, upload
 * or restart runs and the running image isn't pending verify; else why not. */
ss_why_t ss_boot_other(const ss_in_t *in);

/* An app descriptor's build date ("Nov  1 2026", __DATE__) and time ("14:02:11", __TIME__) as one
 * sortable number, yyyymmddhhmmss; -1 when either can't be read. */
int64_t ss_build_stamp(const char *date, const char *time);

/* The other slot's image counts as newer than the running one: built later, or in the same second
 * (two different images with one stamp mean a stale stamp, e.g. a rebuild of the same commit,
 * which doesn't recompile esp_app_desc.c), or undatable (either stamp -1). Such an image is never
 * overwritten without force. Only an earlier stamp makes it older. */
bool ss_counts_newer(const char *other_date, const char *other_time, const char *run_date, const char *run_time);
