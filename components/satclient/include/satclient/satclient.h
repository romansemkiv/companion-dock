#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "satclient/satclient_event.h"
#include "satproto/session.h"

typedef enum { SATC_IDLE, SATC_CONNECTING, SATC_CONNECTED, SATC_TOO_OLD } satc_state_t;

/* Starts the client task. cb->send is ignored (the client writes to its socket);
 * all other callbacks run on the client task with `ctx`. */
esp_err_t satclient_start(const sat_session_cb_t *cb, void *ctx);

/* Empty/NULL host: disconnect and go idle. Same target again: no-op. */
void satclient_set_target(const char *host, uint16_t port);
/* Retry now (the deck's "Retry now"): with a target and no ready session, the connection under way
 * or the wait before the next try (the back-off, the 30 s after a too-old Companion) ends and the
 * client connects again at once. No-op while connected, or with no target. */
void satclient_retry(void);

/* One observer of the connection's outcomes; NULL clears. It is called on the client task (keep it short, and
 * never wait on a lock a task that calls satclient_* holds), with no lock of the client's held: SATC_EV_ATTEMPT_FAILED
 * for an attempt that failed (the lookup or connect failed, or the connection ended before the session was ready),
 * SATC_EV_READY for a session that became ready (after the on_ready callback), SATC_EV_DROPPED for a ready session
 * that ended. Never for a deliberate change of target, and never for a Companion that is too old (the attempt then
 * reports nothing; the client waits 30 s and tries again). May be set before or after satclient_start, from any task. */
void satclient_set_observer(void (*fn)(satc_event_t ev, void *ctx), void *ctx);

satc_state_t satclient_state(void);
/* Caps of the last session that reached READY, or of the last one refused as too old
 * (companion_version is then the refused version). */
void satclient_caps(sat_caps_t *out);
/* Current target host ("" when idle; NUL-terminated within cap) and port. Either may be NULL. */
void satclient_get_target(char *host, size_t cap, uint16_t *port);

/* Thread-safe, never block; strings are copied. Items are dropped unless the
 * session is READY (add/remove are re-issued by the caller on on_ready). */
void satclient_add_device(const sat_device_desc_t *d);
void satclient_remove_device(const char *device_id);
void satclient_key(const char *dev, const char *ctl, bool pressed);
void satclient_rotate(const char *dev, const char *ctl, int delta);
void satclient_change_page(const char *dev, bool forward);
