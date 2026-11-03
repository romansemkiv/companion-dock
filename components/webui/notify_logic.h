/* NotifyStatus's comparison of two statuses (notify.c), pure (host-tested). */
#pragma once
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The comparison key of status st: st printed (unformatted) without the fields that change by themselves,
 * uptime_s, heap and message.remaining_s, as a copy of st with them deleted prints (they are taken out of st
 * for the print and put back at their places, so st is as it was and no copy is made). NULL when it can't be
 * printed; cJSON_free it. */
char *notify_status_key(cJSON *st);

#ifdef __cplusplus
}
#endif
