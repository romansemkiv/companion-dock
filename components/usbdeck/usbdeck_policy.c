#include "usbdeck_policy.h"
#include <stdio.h>
#include <string.h>

/* ---- timing ---- */

uint32_t upol_interval_us(bool high_speed, uint8_t binterval)
{
    if (binterval == 0) binterval = 1;
    if (high_speed) return 125u << (binterval > 16 ? 15 : binterval - 1);
    return 1000u * binterval;
}

uint16_t upol_chunk_pkts(uint32_t interval_us)
{
    uint32_t pkts = interval_us ? UPOL_OUT_CHUNK_MAX_US / interval_us : UPOL_INTR_MAX_PACKETS;
    if (pkts < 1) pkts = 1;
    if (pkts > UPOL_INTR_MAX_PACKETS) pkts = UPOL_INTR_MAX_PACKETS;
    return (uint16_t)pkts;
}

uint32_t upol_out_timeout_ms(uint32_t pkts, uint32_t interval_us)
{
    return UPOL_OUT_TIMEOUT_MS + (uint32_t)((uint64_t)pkts * interval_us * 2 / 1000);
}

uint32_t upol_backoff_next_ms(uint32_t cur_ms)
{
    uint32_t b = cur_ms ? 2 * cur_ms : UPOL_BACKOFF_MIN_MS;
    return b > UPOL_BACKOFF_MAX_MS ? UPOL_BACKOFF_MAX_MS : b;
}

bool upol_sweep_due(int64_t now_us, int64_t last_sweep_us, int64_t *delay_us)
{
    const int64_t since = now_us - last_sweep_us, window = UPOL_IN_WINDOW_MS * 1000LL;
    if (since >= window) return true;
    *delay_us = window - since;
    return false;
}

/* ---- the OUT/IN gate ---- */

upol_in_next_t upol_in_next(bool completed, bool stopping, bool hold)
{
    if (!completed || stopping) return UPOL_IN_STOP;
    return hold ? UPOL_IN_PARK : UPOL_IN_RESUBMIT;
}

bool upol_out_may_start(unsigned queued, unsigned submitting)
{
    (void)queued; /* an idle deck's IN stays queued: never waited for */
    return submitting == 0;
}

upol_in_close_t upol_in_close(bool gone, int64_t waited_us, int64_t last_halt_us)
{
    if (waited_us >= UPOL_IN_CLOSE_US) return UPOL_CLOSE_GIVE_UP;
    if (!gone || waited_us < UPOL_IN_GONE_SETTLE_US) return UPOL_CLOSE_WAIT;
    return last_halt_us < 0 || waited_us - last_halt_us >= UPOL_IN_REHALT_US ? UPOL_CLOSE_HALT : UPOL_CLOSE_WAIT;
}

/* ---- escalation ---- */

void upol_key(char *out, size_t cap, const char *serial, uint16_t vid, uint16_t pid, const char *port_path)
{
    if (!cap) return;
    if (serial && serial[0])
        snprintf(out, cap, "%s", serial);
    else
        snprintf(out, cap, "?%04x:%04x@%s", vid, pid, port_path ? port_path : "");
}

void upol_init(upol_t *p) { memset(p, 0, sizeof *p); }

static const upol_record_t *find_c(const upol_t *p, const char *key)
{
    if (!key || !key[0]) return NULL;
    for (int i = 0; i < UPOL_RECORDS; i++)
        if (strcmp(p->rec[i].key, key) == 0) return &p->rec[i];
    return NULL;
}

static upol_record_t *find(upol_t *p, const char *key) { return (upol_record_t *)find_c(p, key); }

/* Existing record, else a free one, else the least recently used one that isn't quarantined,
 * else (all quarantined) the least recently used one. */
static upol_record_t *get(upol_t *p, const char *key, int64_t now_us)
{
    upol_record_t *r = find(p, key);
    if (r || !key || !key[0]) return r;
    upol_record_t *free_slot = NULL, *lru = NULL, *lru_q = NULL;
    for (int i = 0; i < UPOL_RECORDS && !free_slot; i++) {
        upol_record_t *c = &p->rec[i];
        if (!c->key[0])
            free_slot = c;
        else if (!c->quarantined && (!lru || c->used_us < lru->used_us))
            lru = c;
        else if (c->quarantined && (!lru_q || c->used_us < lru_q->used_us))
            lru_q = c;
    }
    r = free_slot ? free_slot : lru ? lru : lru_q;
    memset(r, 0, sizeof *r);
    snprintf(r->key, sizeof r->key, "%s", key);
    r->used_us = now_us;
    return r;
}

/* Resets this record caused within the window. */
static int live_resets(const upol_record_t *r, int64_t now_us)
{
    int n = 0;
    for (int i = 0; i < r->nresets; i++)
        if (now_us - r->reset_us[i] < UPOL_RESET_WINDOW_US) n++;
    return n;
}

static bool would_quarantine(const upol_record_t *r, int64_t now_us)
{
    if (!r || r->quarantined) return false;
    const bool on_probation = r->probation_until_us && now_us < r->probation_until_us;
    return on_probation || live_resets(r, now_us) + 1 >= UPOL_RESETS_QUARANTINE;
}

bool upol_would_quarantine(const upol_t *p, const char *key, int64_t now_us)
{
    return would_quarantine(find_c(p, key), now_us);
}

bool upol_reset_allowed(const upol_t *p, const char *key, int64_t now_us)
{
    if (!p->any_reset) return true;
    const int64_t since = now_us - p->last_reset_us;
    if (since >= UPOL_RESET_MIN_INTERVAL_US) return true;
    return since >= UPOL_RESET_QUARANTINE_GAP_US && upol_would_quarantine(p, key, now_us);
}

bool upol_root_settled(int64_t now_us, int64_t power_on_us, int64_t last_enum_us)
{
    return last_enum_us > power_on_us || now_us - power_on_us >= UPOL_ROOT_SETTLE_US;
}

/* What a quarantined record switches off: images, and input too after an IN strike. */
static upol_quarantine_t level(const upol_record_t *r)
{
    return (r->kinds & UPOL_STRIKE_IN) ? UPOL_Q_ALL : UPOL_Q_WRITES;
}

int64_t upol_quarantine_len_us(uint8_t n)
{
    const int shift = n <= 1 ? 0 : (n - 1 > 3 ? 3 : n - 1); /* clamped before shifting */
    const int64_t len = UPOL_QUARANTINE_MIN_US << shift;
    return len > UPOL_QUARANTINE_MAX_US ? UPOL_QUARANTINE_MAX_US : len;
}

static int64_t quarantine_end(const upol_record_t *r)
{
    return r->quarantined_us + upol_quarantine_len_us(r->nquar);
}

int64_t upol_quarantine_end_us(const upol_t *p, const char *key)
{
    const upol_record_t *r = find_c(p, key);
    return r && r->quarantined ? quarantine_end(r) : 0;
}

/* One retry at a time, box-wide: is a deck on probation, or about to be (the re-attach after our
 * reset is pending), or was there a bus reset just now? */
static bool retry_running(const upol_t *p, int64_t now_us)
{
    if (p->any_reset && now_us - p->last_reset_us < UPOL_LIFT_QUIET_US) return true;
    for (int i = 0; i < UPOL_RECORDS; i++) {
        const upol_record_t *c = &p->rec[i];
        if (!c->key[0]) continue;
        if (c->probation_until_us && now_us < c->probation_until_us) return true;
        if (c->reattach_by_us && now_us < c->reattach_by_us) return true;
    }
    return false;
}

/* The one way a quarantine ends while the deck is plugged in: if it has run its period and no other
 * retry is running, lift it for one retry on probation. Resets and streak stay. */
static bool lift_if_due(const upol_t *p, upol_record_t *r, int64_t now_us)
{
    if (now_us < quarantine_end(r) || retry_running(p, now_us)) return false;
    r->quarantined = false;
    r->quarantined_us = 0;
    r->streak_us = now_us; /* quiet time counts from here */
    r->probation_until_us = now_us + UPOL_PROBATION_US;
    r->used_us = now_us;
    return true;
}

upol_quarantine_t upol_note_reset(upol_t *p, const char *key, upol_strike_t kind, int64_t now_us)
{
    p->any_reset = true;
    p->last_reset_us = now_us;
    upol_record_t *r = get(p, key, now_us);
    if (!r) return UPOL_Q_NONE;
    const bool quarantine = would_quarantine(r, now_us);
    r->used_us = now_us;
    if (!r->quarantined && now_us - r->streak_us >= UPOL_STREAK_RESET_US) r->nquar = 0; /* quiet hour */
    r->streak_us = now_us;
    /* Keep the resets still in the window, then add this one (dropping the oldest if full). */
    int keep = 0;
    for (int i = 0; i < r->nresets; i++)
        if (now_us - r->reset_us[i] < UPOL_RESET_WINDOW_US) r->reset_us[keep++] = r->reset_us[i];
    if (keep == 0) r->kinds = 0;
    if (keep == UPOL_RESETS_QUARANTINE) {
        memmove(r->reset_us, r->reset_us + 1, (keep - 1) * sizeof r->reset_us[0]);
        keep--;
    }
    r->reset_us[keep++] = now_us;
    r->nresets = (uint8_t)keep;
    r->kinds |= (uint8_t)kind;
    r->reattach_by_us = now_us + UPOL_REATTACH_US;
    r->probation_until_us = 0;
    if (!quarantine) return UPOL_Q_NONE;
    r->quarantined = true;
    r->quarantined_us = now_us;
    if (r->nquar < UINT8_MAX) r->nquar++;
    return level(r);
}

upol_quarantine_t upol_note_attach(upol_t *p, const char *key, int64_t now_us)
{
    upol_record_t *r = find(p, key);
    if (!r) return UPOL_Q_NONE;
    r->used_us = now_us;
    const bool after_our_reset = r->reattach_by_us && now_us < r->reattach_by_us; /* in time */
    r->reattach_by_us = 0;
    if (r->quarantined && !lift_if_due(p, r, now_us)) return level(r); /* nothing to test yet */
    if (after_our_reset) r->probation_until_us = now_us + UPOL_PROBATION_US;
    return UPOL_Q_NONE;
}

bool upol_note_lift(upol_t *p, const char *key, int64_t now_us)
{
    upol_record_t *r = find(p, key);
    if (!r || !r->quarantined) return true;
    return lift_if_due(p, r, now_us);
}

void upol_note_detach(upol_t *p, const char *key, bool ours, int64_t now_us)
{
    upol_record_t *r = find(p, key);
    if (!r || ours) return;
    if (r->key[0] == '?') { /* a socket, not a deck (upol_key): whoever comes next starts afresh */
        memset(r, 0, sizeof *r);
        return;
    }
    r->used_us = now_us;
    if (r->quarantined) r->streak_us = now_us; /* a lift: quiet time counts from here */
    r->quarantined = false; /* replugged: lift (the streak stays) */
    r->quarantined_us = 0;
    r->kinds = 0;           /* what to switch off is decided afresh; the reset count carries on */
    r->reattach_by_us = 0;  /* its next attach doesn't follow our reset */
    r->probation_until_us = 0;
}

void upol_after_restart(upol_t *p)
{
    if (p->any_reset) p->last_reset_us = 0;
    for (int i = 0; i < UPOL_RECORDS; i++) {
        upol_record_t *r = &p->rec[i];
        r->key[UPOL_KEY_MAX - 1] = 0;
        if (r->nresets > UPOL_RESETS_QUARANTINE) r->nresets = UPOL_RESETS_QUARANTINE;
        for (int k = 0; k < r->nresets; k++) r->reset_us[k] = 0;
        if (r->reattach_by_us) r->reattach_by_us = UPOL_REATTACH_US;
        r->probation_until_us = 0;
        if (r->quarantined) r->quarantined_us = 0; /* its period starts again */
        r->streak_us = 0;
        r->used_us = 0;
    }
}

uint32_t upol_sum(const upol_t *p)
{
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < sizeof *p; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

int upol_quarantine_msg(char *out, size_t cap, const char *serial, upol_quarantine_t q, int64_t left_us)
{
    if (q == UPOL_Q_NONE) {
        if (cap) out[0] = 0;
        return 0;
    }
    const int64_t minute = 60 * 1000000LL, max_min = UPOL_QUARANTINE_MAX_US / minute;
    int64_t min = left_us <= 0 ? 1 : (left_us + minute - 1) / minute;
    if (min > max_min) min = max_min;
    return snprintf(out, cap, USBDECK_QUARANTINE_PREFIX "%s, retrying in %d min", serial,
                    q == UPOL_Q_ALL ? "off" : "images off", (int)min);
}

void upol_summary(const upol_t *p, int *decks, int *resets, int *quarantined)
{
    *decks = *resets = *quarantined = 0;
    for (int i = 0; i < UPOL_RECORDS; i++) {
        const upol_record_t *r = &p->rec[i];
        if (!r->key[0]) continue;
        (*decks)++;
        *resets += r->nresets;
        if (r->quarantined) (*quarantined)++;
    }
}

/* ---- deck trouble for the status light ---- */

bool upol_failing(bool reported, uint32_t last_fail_s, uint32_t now_s)
{
    return reported && (uint32_t)(now_s - last_fail_s) < UPOL_FAILING_HOLD_S;
}
