/* The restart breadcrumb's record and the boot summary (pure: no ESP-IDF header; host-tested in
 * test/host/test_breadcrumb_logic.c).
 *
 * Before a deliberate restart the box writes a short reason into a record in RTC no-init memory
 * (breadcrumb.c). The record carries a magic (with the layout in it, so an image with another layout
 * reads it as absent) and a checksum, so random memory after a power-on, a torn write or another
 * image's record never passes as one. At the next start the record is read, summarised with the
 * reset reason, logged and cleared:
 *   "software restart: mode switch"                          the restart went as planned
 *   "panic, last deliberate restart: mode switch pending"    it crashed after esp_restart was
 *                                                            called: in the shutdown it runs, or
 *                                                            early in the next start-up, before
 *                                                            app_main read the record
 *   "panic, last deliberate restart: mode switch pending (scheduled)"
 *                                                            it crashed after the restart was
 *                                                            scheduled, before esp_restart was called
 *   "panic"                                                  no deliberate restart was under way
 * The core dump tells which. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BC_REASON_MAX 24 /* bytes, with the NUL */

typedef enum {
    BC_STAGE_SCHEDULED = 1,  /* a restart is on its way (e.g. after a reply goes out) */
    BC_STAGE_RESTARTING = 2, /* esp_restart is being called */
} bc_stage_t;

typedef struct {
    uint32_t magic;
    uint32_t stage;
    char reason[BC_REASON_MAX];
    uint32_t sum;
} bc_record_t;

/* What a reset reason says about the breadcrumb (breadcrumb.c maps esp_reset_reason to this). */
typedef enum {
    BC_RESET_POWERON,    /* power-on: no-init memory holds nothing; the record is ignored */
    BC_RESET_DELIBERATE, /* esp_restart */
    BC_RESET_CRASH,      /* panic, watchdogs, brownout, CPU lockup */
    BC_RESET_OTHER,      /* RESET pin, deep sleep, JTAG, USB, ... */
} bc_reset_kind_t;

/* Fills r with reason (printable ASCII kept, anything else dropped; cut to BC_REASON_MAX - 1;
 * NULL is ""), stage, the magic and the checksum. */
void bc_encode(bc_record_t *r, const char *reason, bc_stage_t stage);

/* r holds a record bc_encode wrote: magic, a known stage, a NUL inside reason, checksum. */
bool bc_valid(const bc_record_t *r);

/* Leaves r invalid. */
void bc_clear(bc_record_t *r);

/* The boot summary (header comment) into out (always NUL-terminated when n > 0). reset: the reset
 * reason's name ("panic", "software restart", ...); r: the record found at boot (NULL or invalid:
 * none). A power-on ignores the record. Returns strlen(out). */
size_t bc_summary(char *out, size_t n, const char *reset, bc_reset_kind_t kind, const bc_record_t *r);
