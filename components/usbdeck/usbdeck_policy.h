#pragma once
/* Pure decision logic of the USB deck layer (no ESP-IDF, host-tested in test/host):
 * - OUT/IN timing: packet spacing, gated chunk size, OUT timeout, write backoff, IN restart window;
 * - escalation: a box-wide floor between bus resets, and a per-deck record (keyed by serial, or by
 *   VID/PID and port path when the serial can't be read) that survives detach and software
 *   restarts, and quarantines a deck that the bus reset doesn't cure;
 * - recovery: a quarantine is never permanent. It ends by itself after 10 min, doubling at
 *   every quarantine in a row up to 60 min, for one retry on probation, one deck at a time.
 * Times are microseconds of one monotonic clock (esp_timer_get_time on the target). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "usbdeck/problem.h"

/* ---- timing ---- */

#define UPOL_OUT_TIMEOUT_MS 500    /* a deck holds off OUT data for up to ~220 ms when healthy */
/* after the timeout: a deck that still hasn't taken it is wedged. 500 ms was too short: an XL (fw 1.01.001) took an
 * image report 1.0 s late, right after a full page, and was reset while healthy (the report went through 1 ms
 * after the reset started); 2 s more gives 2.5 x that, and a wedged deck is still reset 2.5 s after the report */
#define UPOL_OUT_STUCK_MS 2000
#define UPOL_OUT_CHUNK_MAX_US 8000 /* bus time of one gated OUT transfer */
#define UPOL_INTR_MAX_PACKETS 32   /* IDF 5.5 hcd_dwc.c XFER_LIST_LEN_INTR */
#define UPOL_BACKOFF_MIN_MS 1000
#define UPOL_BACKOFF_MAX_MS 8000
#define UPOL_IN_WINDOW_MS 10       /* parked IN transfers are restarted at least this often */

/* ---- deck trouble for the status light (usbdeck_trouble) ----
 * A deck's input or image writes count as failing now while the problem was reported for the streak
 * and the last failure is less than UPOL_FAILING_HOLD_S old (the IN streak's own quiet window, 10 s):
 * a deck that recovered stops counting then, without waiting for its next key press or image. Seconds
 * of a 32-bit clock, compared across its wrap. */
#define UPOL_FAILING_HOLD_S 10
bool upol_failing(bool reported, uint32_t last_fail_s, uint32_t now_s);

/* Interrupt endpoint (micro)frame spacing, USB 2.0 table 9-13. bInterval 0 is treated as 1. */
uint32_t upol_interval_us(bool high_speed, uint8_t binterval);
/* Packets one gated OUT transfer may carry: OUT_CHUNK_MAX_US of bus time, 1..32. */
uint16_t upol_chunk_pkts(uint32_t interval_us);
/* When a gated OUT transfer of `pkts` packets counts as late. */
uint32_t upol_out_timeout_ms(uint32_t pkts, uint32_t interval_us);
/* Next write backoff after a late transfer (0: first one). */
uint32_t upol_backoff_next_ms(uint32_t cur_ms);
/* At the end of a gated transfer: restart parked IN transfers now (true), or *delay_us later. */
bool upol_sweep_due(int64_t now_us, int64_t last_sweep_us, int64_t *delay_us);

/* ---- the OUT/IN gate ----
 * With the RX FIFO sized for a 512-byte HS IN packet (usb_host_task.c), an idle IN transfer stays
 * queued: the deck's NAKs are retried inside it and it completes only with data. The gate therefore
 * never waits for IN transfers before an OUT, and never halts or cancels a queued one to make room
 * (IDF 5.5 can't halt a periodic channel whose device keeps NAKing). What it prevents is the one
 * thing hwdebug-1 tied IDF's skipped-OUT-packet assert to: an IN resubmission racing an OUT. */

typedef enum { UPOL_IN_RESUBMIT, UPOL_IN_PARK, UPOL_IN_STOP } upol_in_next_t;
/* An IN transfer came back from the USB stack. completed: status COMPLETED (with data; at idle it
 * doesn't come back); stopping: its handle is closing; hold: an OUT transfer holds the gate. PARK:
 * the gate re-arms it after the OUT (upol_sweep_due); STOP: it leaves the stack (an error goes to
 * the owner, which restarts it). */
upol_in_next_t upol_in_next(bool completed, bool stopping, bool hold);

/* May the OUT that just took the gate (s_hold set) start? queued: IN transfers queued in the USB
 * stack; submitting: IN submits decided before the hold and not yet returned from the HCD.
 * Only the latter hold it back: one submit call, microseconds. A queued IN never does. */
bool upol_out_may_start(unsigned queued, unsigned submitting);

#define UPOL_IN_SUBMIT_WAIT_US 20000LL /* out_begin's bound on that wait; past it the OUT isn't sent */

#define UPOL_IN_GONE_SETTLE_US 100000LL /* a gone deck's IN: time to retire by itself before a halt */
#define UPOL_IN_REHALT_US 100000LL
#define UPOL_IN_CLOSE_US 1000000LL      /* then the close gives up (the interface leaks) */
typedef enum { UPOL_CLOSE_WAIT, UPOL_CLOSE_HALT, UPOL_CLOSE_GIVE_UP } upol_in_close_t;
/* Closing a handle whose IN transfer is still queued, waited_us after the close began; last_halt_us:
 * when (on the same clock) it last halted it, -1 for never. gone: the deck left (its port is invalid,
 * or it errors out behind a hub). A gone deck's IN is halted once it had UPOL_IN_GONE_SETTLE_US to
 * retire by itself (a halt racing that retirement never returns), then every UPOL_IN_REHALT_US; a
 * present deck's queued IN is never halted. GIVE_UP at UPOL_IN_CLOSE_US. */
upol_in_close_t upol_in_close(bool gone, int64_t waited_us, int64_t last_halt_us);

/* ---- escalation ---- */

#define UPOL_KEY_MAX 33
#define UPOL_RECORDS 8                                   /* decks remembered (> USBDECK_MAX) */
#define UPOL_RESET_MIN_INTERVAL_US (10 * 1000000LL)      /* box-wide floor between bus resets */
#define UPOL_RESET_QUARANTINE_GAP_US (1 * 1000000LL)     /* ...except a quarantining one: this */
#define UPOL_ROOT_SETTLE_US (3 * 1000000LL)              /* after our root-port power-on, at most */
#define UPOL_REATTACH_US (30 * 1000000LL)                /* our reset -> that deck's re-attach, at most */
#define UPOL_PROBATION_US (30 * 1000000LL)               /* after the re-attach that follows our
                                                          * reset, and after a quarantine's lift */
#define UPOL_RESET_WINDOW_US (10 * 60 * 1000000LL)
#define UPOL_RESETS_QUARANTINE 4                         /* resets caused within the window */
#define UPOL_QUARANTINE_MIN_US (10 * 60 * 1000000LL)     /* a quarantine lasts this, then the deck */
#define UPOL_QUARANTINE_MAX_US (60 * 60 * 1000000LL)     /* is retried; doubling up to this */
#define UPOL_STREAK_RESET_US (60 * 60 * 1000000LL)       /* no strike this long: back to the minimum */
#define UPOL_LIFT_QUIET_US (10 * 1000000LL)              /* no retry this soon after a bus reset */
#define UPOL_LIFT_RETRY_US (5 * 1000000LL)               /* a due retry held back: ask again then */
#define UPOL_LAYOUT_VERSION 5                            /* bump when upol_t (or what it means) changes;
                                                          * 5: IN strikes no longer raised, so a
                                                          * kept record with one reads as absent */

typedef enum { UPOL_STRIKE_OUT = 1, UPOL_STRIKE_IN = 2 } upol_strike_t;
typedef enum { UPOL_Q_NONE = 0, UPOL_Q_WRITES, UPOL_Q_ALL } upol_quarantine_t;

typedef struct {
    char key[UPOL_KEY_MAX];            /* "" = free */
    int64_t reset_us[UPOL_RESETS_QUARANTINE]; /* resets this deck caused, oldest first */
    uint8_t nresets;
    uint8_t kinds;                     /* upol_strike_t bits of those resets since it was plugged in */
    int64_t reattach_by_us;            /* our reset took it off: an attach before this starts its
                                        * probation (0: none) */
    int64_t probation_until_us;        /* 0: none */
    bool quarantined;
    uint8_t nquar;                     /* quarantines in the current streak (saturating) */
    int64_t quarantined_us;            /* when the current quarantine started; it ends after
                                        * upol_quarantine_len_us(nquar) */
    int64_t streak_us;                 /* last strike or lift: the streak's quiet time counts from
                                        * here (only meaningful with nquar > 0) */
    int64_t used_us;
} upol_record_t;

typedef struct {
    upol_record_t rec[UPOL_RECORDS];
    bool any_reset;
    int64_t last_reset_us;
} upol_t;

/* Record key: the serial when one was read, else VID/PID plus the hub port path, which (unlike
 * the USB address, handed out round-robin) stays the same across re-enumeration. */
void upol_key(char *out, size_t cap, const char *serial, uint16_t vid, uint16_t pid, const char *port_path);

void upol_init(upol_t *p);
/* Would a bus reset caused by `key`'s deck quarantine it? (Same rule as upol_note_reset.) Yes when
 * it stalled again during its probation (the reset, or the quarantine, didn't cure it), or this
 * would be its UPOL_RESETS_QUARANTINE-th reset within the window. Never while quarantined. */
bool upol_would_quarantine(const upol_t *p, const char *key, int64_t now_us);
/* May a bus reset for `key`'s deck start now, as far as the record goes? Yes if there was no reset
 * yet, or the floor has passed, or it is UPOL_RESET_QUARANTINE_GAP_US past the last one and this
 * reset quarantines the deck. The port must also be settled (upol_root_settled). */
bool upol_reset_allowed(const upol_t *p, const char *key, int64_t now_us);
/* May the root port be powered off now, as far as IDF's hub driver goes? After a power-on its
 * root-port state is POWERED until it has handled the root device's connection; a power-off in
 * that window makes it look for a device-tree node that doesn't exist yet and abort (IDF 5.5
 * hub.c root_port_handle_events, ESP_ERROR_CHECK(dev_tree_node_dev_gone(NULL, 0))). A device that
 * finished enumerating after our power-on (last_enum_us, the client's NEW_DEV) shows that the
 * connection was handled. Without one, wait at most UPOL_ROOT_SETTLE_US after power_on_us (0: we
 * never powered it). Nothing else delays a reset: enumeration elsewhere on the bus doesn't. */
bool upol_root_settled(int64_t now_us, int64_t power_on_us, int64_t last_enum_us);
/* A bus reset started because of `key`'s deck (a strike). UPOL_Q_NONE, or the level this reset
 * quarantines it at (report once; the next attach returns the same level). A quarantine starts a
 * period of upol_quarantine_len_us(n), n counting the quarantines in a row: the streak is
 * forgotten at a strike that comes UPOL_STREAK_RESET_US after the last strike or lift (time spent
 * quarantined isn't quiet time: such a deck can't strike). An empty key still counts for the floor. */
upol_quarantine_t upol_note_reset(upol_t *p, const char *key, upol_strike_t kind, int64_t now_us);
/* The deck attached: what it may do. Starts its probation if this is the re-attach after a reset
 * it caused (within UPOL_REATTACH_US of it; a later attach starts nothing). A quarantine that has
 * ended is lifted here if upol_note_lift would lift it; else it holds until the deck task's lift. */
upol_quarantine_t upol_note_attach(upol_t *p, const char *key, int64_t now_us);
/* Quarantine length for the n-th quarantine in a row: UPOL_QUARANTINE_MIN_US doubled n - 1 times
 * (n 0 counts as 1), at most UPOL_QUARANTINE_MAX_US: 10, 20, 40, 60, 60... min. */
int64_t upol_quarantine_len_us(uint8_t n);
/* When `key`'s quarantine ends (0: not quarantined), for the deck task's lift. */
int64_t upol_quarantine_end_us(const upol_t *p, const char *key);
/* The deck task of a quarantined, attached deck asks whether it may take images again. True when
 * the quarantine has ended and no other retry is in progress: none while another deck is on
 * probation or about to be (the re-attach after our reset is pending), nor within
 * UPOL_LIFT_QUIET_US of a bus reset (else ask again UPOL_LIFT_RETRY_US later). Then it is lifted:
 * resets and streak kept, on probation for UPOL_PROBATION_US (one retry: a stall now quarantines it
 * again, twice as long). Also true, changing nothing, when the record doesn't quarantine it (any
 * more, or was evicted): nothing holds the deck back. */
bool upol_note_lift(upol_t *p, const char *key, int64_t now_us);
/* The deck detached. ours: during our bus reset (keeps everything). Otherwise (a replug, or the
 * deck dropping off or restarting by itself) a quarantine is lifted and a probation ends, and:
 * - a serial key keeps its resets, which count until they leave the window, so the
 *   UPOL_RESETS_QUARANTINE backstop holds across quarantine episodes, and its streak;
 * - a port-path key (no serial) is dropped: it names a socket, not a deck, and the next deck
 *   there must not inherit it. */
void upol_note_detach(upol_t *p, const char *key, bool ours, int64_t now_us);
/* After a software restart, with the record kept in RAM: the clock started again at 0, so every
 * time counts as "at boot" (counts stay, windows restart); a running probation ends, a pending
 * re-attach (the reset hadn't seen the deck come back) is due within UPOL_REATTACH_US of boot,
 * however long ago the reset was (the restart doesn't say). A quarantine starts its period again
 * at boot: never shorter than what was left, longer by at most one period (60 min); a box that
 * restarts more often than that (for another reason) never retries a deck that stays plugged in.
 * The streak's quiet hour also counts from boot. Also repairs what can't be valid (unterminated
 * keys, counts out of range). A serial-less deck swapped for another of the same model at the same
 * socket during a restart inherits the record (no detach is seen); accepted, since the record must
 * survive the restart that a USB that won't come back triggers. */
void upol_after_restart(upol_t *p);
/* Checksum of the record, to validate it after a restart (FNV-1a). */
uint32_t upol_sum(const upol_t *p);
/* The problem text for a quarantined deck, starting with USBDECK_QUARANTINE_PREFIX: what is off,
 * and in how many minutes it is retried (left_us until upol_quarantine_end_us, rounded up to whole
 * minutes, 1..60). The same when the reset quarantines the deck and at the re-attach after it
 * (snprintf result; Q_NONE: empty). Fits USBDECK_PROBLEM_MAX with any serial usbdeck reports (31
 * characters at most). */
int upol_quarantine_msg(char *out, size_t cap, const char *serial, upol_quarantine_t q, int64_t left_us);
/* For logs: decks remembered, resets they caused, decks quarantined. */
void upol_summary(const upol_t *p, int *decks, int *resets, int *quarantined);
