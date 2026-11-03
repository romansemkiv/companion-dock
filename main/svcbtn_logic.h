/* The service button on GPIO32 (pure: no ESP-IDF, unit-tested on the host, test/host).
 * svcbtn.c polls it every 10 ms: a short press (released after SVC_SHORT_MIN_US and
 * before SVC_SHORT_MAX_US) opens or closes the maintenance screen on the decks, held SVC_SETUP_US
 * then released turns the setup hotspot on for 15 minutes, held SVC_RESET_US then released resets the
 * settings; released between SVC_SHORT_MAX_US and SVC_SETUP_US it does nothing. The action fires on
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
#define SVC_SHORT_MAX_US (2 * 1000000LL)  /* a short press is released before this */

typedef enum {
    SVC_NONE,
    SVC_PRESSED,     /* a press began */
    SVC_SETUP_STAGE, /* held SVC_SETUP_US: released now, the hotspot */
    SVC_RESET_STAGE, /* held SVC_RESET_US: released now, the factory reset */
    SVC_SETUP,       /* released after the setup stage */
    SVC_RESET,       /* released after the reset stage */
    SVC_SHORT,       /* released after a short press: the maintenance screen on demand */
} svc_event_t;

typedef struct {
    bool armed, down;
    int stage;
    int64_t pressed_at;
} svcbtn_t;

void svcbtn_init(svcbtn_t *b);
/* One poll: down = the line reads pressed, now_us = a monotonic time. */
svc_event_t svcbtn_step(svcbtn_t *b, bool down, int64_t now_us);
