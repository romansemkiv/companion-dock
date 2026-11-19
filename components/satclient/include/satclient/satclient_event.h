#pragma once
/* What satclient_set_observer reports (no ESP-IDF header: the pure outcome rule uses it too). */
typedef enum { SATC_EV_ATTEMPT_FAILED, SATC_EV_READY, SATC_EV_DROPPED } satc_event_t;
