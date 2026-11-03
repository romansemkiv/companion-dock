/* The restart breadcrumb: why the box last restarted on purpose, kept across that restart.
 *
 * Every deliberate restart records a short reason first (RTC no-init memory, with a magic and a
 * checksum: breadcrumb_logic.h). breadcrumb_boot, early in app_main, reads it with esp_reset_reason,
 * logs one line and clears it. A crash after a restart was requested and before the next start
 * reached breadcrumb_boot (in the second before esp_restart, in the shutdown it runs, or early in the
 * next start-up) shows as "panic, last deliberate restart: <reason> pending" instead of a bare panic;
 * the core dump tells which. Device.GetInfo reports {reason, breadcrumb} as lastReset and
 * Sys.GetStatus the one-line summary. A crash also leaves a core dump in the "coredump" partition
 * (docs/manual.md: idf.py coredump-info). */
#pragma once

#include <stddef.h>

/* Restart reasons, as the breadcrumb records them. */
#define BC_WHY_API_RESTART "Device.Restart"
#define BC_WHY_FACTORY_RESET "factory reset"
#define BC_WHY_MODE_SWITCH "mode switch"
#define BC_WHY_DECK_LIMIT "deck limit" /* Device.SetConfig deckLimit, applied at the next start */
#define BC_WHY_OTA "OTA"
#define BC_WHY_OTA_ROLLBACK "OTA self-check failed"
#define BC_WHY_BUTTON_RESET "service button reset"
#define BC_WHY_USB_POWER "USB power-on failed"
#define BC_WHY_C6_UPDATE "C6 update"
#define BC_WHY_C6_LOST "C6 lost" /* the supervisor: no C6 and no Ethernet for 3 min */
#define BC_WHY_BOOT_OTHER "boot other slot" /* slotsync_boot_other: Ota.BootOther or the deck */

/* A restart for `why` is on its way (it is made later, by breadcrumb_restart). */
void breadcrumb_scheduled(const char *why);

/* Records `why` and restarts. */
void breadcrumb_restart(const char *why) __attribute__((noreturn));

/* Records `why` for a restart that a library call makes itself (it calls esp_restart inside, e.g.
 * esp_ota_mark_app_invalid_rollback_and_reboot): call it right before that call. */
void breadcrumb_note(const char *why);

/* Drops the record: a restart that was recorded did not happen (the call returned, or the restart
 * couldn't be scheduled). */
void breadcrumb_clear(void);

/* Once, first thing in app_main: reads and clears the record, logs the summary. */
void breadcrumb_boot(void);

/* This start's reset reason ("power-on", "software restart", "panic", "task watchdog", ...). */
const char *breadcrumb_reset_reason(void);

/* The reason the last deliberate restart recorded ("" when none was under way or after a power-on). */
const char *breadcrumb_last(void);

/* The one-line summary breadcrumb_boot logged. */
const char *breadcrumb_summary(void);
