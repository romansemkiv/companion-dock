#pragma once
/* Sys.GetPerf's arithmetic (perf.c), pure: a task's share of one core between two samples of FreeRTOS's
 * run-time counters. Host-tested.
 *
 * Rules:
 * - Share = the task's run time since the earlier sample over the wall time since it, in tenths of a
 *   percent of one core, at most 1000.
 * - A task missing from the earlier sample started since: all its run time is in the window.
 * - A counter that went back (a handle reused by a newer task) counts from 0, never as a negative share.
 * - No earlier sample, or no time between them: unknown (-1). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uintptr_t id; /* the task's handle */
    uint64_t run; /* its run-time counter (us) */
} perf_sample_t;

int perf_share_permille(const perf_sample_t *prev, size_t n_prev, bool have_prev, uintptr_t id, uint64_t run,
                        uint64_t dt_us);
