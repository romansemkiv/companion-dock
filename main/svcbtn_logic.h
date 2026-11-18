/* The service key (pure: no ESP-IDF, unit-tested on the host, test/host).
 * svcbtn.c polls it every 10 ms: a short press (released after SVC_SHORT_MIN_US and
 * before SVC_SHORT_MAX_US) does keyShort; held SVC_LONG_US (= SVC_SHORT_MAX_US) and released before
 * SVC_SETUP_US, a long press, keyLong; held SVC_SETUP_US then released turns the setup hotspot on for 15
 * minutes, held SVC_RESET_US then released resets the settings. Each hold mark is an event of its own
 * (*_STAGE) as it is reached, for the key's feedback. The action fires on
 * release, so holding on to the reset never opens the hotspot on the way. A press counts only once the
 * line has read released since start-up: one that reads pressed from the start (held, miswired, a
 * pin tied low) never counts as a short press, opens the hotspot or resets. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SVC_SETUP_US (5 * 1000000LL)
#define SVC_RESET_US (10 * 1000000LL)
/* A shorter press is a bounce: at svcbtn.c's 10 ms poll, five samples or more. */
#define SVC_SHORT_MIN_US (50 * 1000LL)
#define SVC_SHORT_MAX_US (3 * 1000000LL)  /* a short press is released before this */
#define SVC_LONG_US SVC_SHORT_MAX_US       /* ... a long press after it (and before SVC_SETUP_US) */

typedef enum {
    SVC_NONE,
    SVC_PRESSED,     /* a press began */
    SVC_LONG_STAGE,  /* held SVC_LONG_US: released now, the long press */
    SVC_SETUP_STAGE, /* held SVC_SETUP_US: released now, the hotspot */
    SVC_RESET_STAGE, /* held SVC_RESET_US: released now, the factory reset */
    SVC_SETUP,       /* released after the setup stage */
    SVC_RESET,       /* released after the reset stage */
    SVC_SHORT,       /* released after a short press: keyShort */
    SVC_LONG,        /* released after the long stage, before the setup stage: keyLong */
} svc_event_t;

typedef struct {
    bool armed, down;
    int stage;
    int64_t pressed_at;
} svcbtn_t;

void svcbtn_init(svcbtn_t *b);
/* One poll: down = the line reads pressed, now_us = a monotonic time. */
svc_event_t svcbtn_step(svcbtn_t *b, bool down, int64_t now_us);
