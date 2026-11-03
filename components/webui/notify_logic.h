/* NotifyStatus's comparison of two statuses (notify.c), pure (host-tested). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The comparison key of status st: st printed (unformatted) without the fields that change by themselves,
 * uptime_s, heap and message.remaining_s, as a copy of st with them deleted prints (they are taken out of st
 * for the print and put back at their places, so st is as it was and no copy is made). NULL when it can't be
 * printed; cJSON_free it. */
char *notify_status_key(cJSON *st);

/* 32-bit FNV-1a of the n bytes at p, chained from h (NOTIFY_FNV_SEED to start): Sys.GetStatus.rev's fingerprints of
 * the Companions found and the firmware slots. p may be NULL when n is 0. */
#define NOTIFY_FNV_SEED 2166136261u
uint32_t notify_fnv1a(uint32_t h, const void *p, size_t n);

#ifdef __cplusplus
}
#endif
