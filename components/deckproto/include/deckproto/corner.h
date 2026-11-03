#pragma once
/* Pure: the gesture that opens the maintenance screen on demand on a deck Companion drives (which replaces the two
 * top corners; configurable, corner_cfg_t), in both modes. Two corner keys: FIRST, the one tapped first (by default
 * top-right), and SECOND, the one held with it (by default bottom-right):
 *
 * - Tap FIRST taps times in a row (by default once; each tap within CORNER_ARM_MS of the one before, SECOND not pressed
 *   between them), then within CORNER_ARM_MS press FIRST and SECOND together and hold both hold_ms (by default 2 s). The
 *   taps reach Companion as any press does (they can't be told apart then).
 * - Only in the armed window after the last tap is a FIRST or SECOND key-down held back from Companion for
 *   CORNER_WINDOW_MS. If the other one goes down within that window, neither key-down is sent: the chord is
 *   consumed. Outside the armed window no key is ever delayed.
 * - A lone FIRST or SECOND press in the armed window goes on once the window ends (at most CORNER_WINDOW_MS late),
 *   its release after it; a release inside the window sends the press and the release together. A FIRST tap
 *   re-arms.
 * - A chord let go before hold_ms sends nothing and opens nothing; held hold_ms it opens, and the two keys'
 *   releases go to nobody. Until both are up, either pressed again is swallowed.
 * - Every other key is never delayed: Satellite mode delays only these two keys' events; Network Dock mode
 *   rewrites the deck's input reports with the held-back bits cleared (corner_gate_*).
 * - Off: no key is the gesture's; nothing is ever held back and nothing opens.
 *
 * The corners are logical places, (0 or cols - 1, 0 or rows - 1): the Original Stream Deck reports each row right
 * to left, so its right column is report index 0 of the row (deck_model_controls). Every model with a screen and 2
 * rows or more has them; never the Pedal. Time is in ms, monotonic, passed in. Host-tested
 * (test/host/test_corner.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deckmodel/model.h"
#include "deckproto/deckproto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CORNER_WINDOW_MS 120       /* in the armed window, a FIRST / SECOND key-down held back this long, for the other */
#define CORNER_HOLD_DEFAULT_MS 2000 /* both held this long: the maintenance screen opens on the deck */
#define CORNER_ARM_MS 3000          /* after a FIRST tap, the window in which the next tap or the chord counts */
#define CORNER_TAPS_MAX 3           /* FIRST tapped 1 to 3 times */

/* A deck's four corner keys (boxconfig's gestureKeys names them in this order). */
enum { CORNER_TOP_LEFT, CORNER_TOP_RIGHT, CORNER_BOTTOM_LEFT, CORNER_BOTTOM_RIGHT };

/* The gesture's two keys, as hw[] and corner_which have them. */
enum { CORNER_FIRST, CORNER_SECOND };

/* The gesture as set on the web page (gestureEnabled, gestureKeys, gestureTaps, gestureHoldSeconds). */
typedef struct {
    bool on;          /* the gesture opens the box menu; off, no key is ever held back */
    uint8_t first;    /* the corner tapped first, then held (CORNER_TOP_LEFT ...) */
    uint8_t second;   /* the corner held with it, not first */
    uint8_t taps;     /* FIRST's taps that arm the chord, 1 .. CORNER_TAPS_MAX */
    uint16_t hold_ms; /* both held this long open the box menu, > 0 */
} corner_cfg_t;

/* The default gesture: on, tap top-right once, then top-right + bottom-right held 2 s. */
corner_cfg_t corner_cfg_default(void);
/* cfg is a gesture the gates can run: two different corners, taps 1 .. CORNER_TAPS_MAX, a hold (on or off). */
bool corner_cfg_ok(const corner_cfg_t *cfg);
/* The gesture the gates run for cfg: cfg itself when corner_cfg_ok, else (NULL, all zero: none given yet, or not
 * valid) the default. */
corner_cfg_t corner_cfg_use(const corner_cfg_t *cfg);
/* a and b run the same gesture (as corner_cfg_use takes them). */
bool corner_cfg_eq(const corner_cfg_t *a, const corner_cfg_t *b);
/* For the log, as corner_cfg_use takes it: "tap top-right, then hold top-right + bottom-right 2 s",
 * "tap top-left twice, ...", "... 3 times, ...", or "off"; cut to n - 1 characters. */
void corner_cfg_describe(const corner_cfg_t *cfg, char *out, size_t n);

/* The gesture's two keys' report indexes (deckproto_parse_input's key index: hw) into hw[CORNER_FIRST],
 * hw[CORNER_SECOND], for gesture cfg (corner_cfg_use: NULL is the default); false when the gesture is off, or for a
 * model without it (no screen: the Pedal; one row). */
bool corner_keys(const deck_model_t *m, const corner_cfg_t *cfg, uint8_t hw[2]);
/* CORNER_FIRST / CORNER_SECOND for key report index idx, -1 for any other key. */
int corner_which(const uint8_t hw[2], uint8_t idx);

/* A corner's state. */
enum {
    CORNER_UP,   /* up (as far as the gate knows) */
    CORNER_WAIT, /* down, held back: the window runs */
    CORNER_PASS, /* down, passed on to Companion */
    CORNER_EATEN /* down, consumed by a chord: its release goes to nobody */
};

typedef struct {
    corner_cfg_t cfg; /* the gesture it runs (corner_cfg_use of corner_init's) */
    uint8_t st[2];
    int64_t t[2];   /* WAIT: when it went down */
    bool chord;     /* both went down within the window: consumed until both are up */
    bool broken;    /* ... one let go before the hold: it opens nothing */
    bool fired;     /* ... the hold made: opened */
    int64_t chord_t; /* when the chord was made */
    int64_t armed_until; /* the last FIRST tap's window ends then (0: none) */
    uint8_t ntaps;       /* FIRST's taps in a row so far; armed while >= cfg.taps, in the window */
} corner_t;

/* What to pass on to Companion, in order, and whether the gesture opened. */
typedef struct {
    uint8_t n;
    struct {
        uint8_t which; /* CORNER_FIRST / CORNER_SECOND */
        bool down;
    } ev[4];
    bool open; /* the chord held cfg.hold_ms: open the maintenance screen on this deck */
} corner_out_t;

/* Nothing held back, nothing armed, running gesture cfg (corner_cfg_use: NULL is the default). */
void corner_init(corner_t *c, const corner_cfg_t *cfg);
/* FIRST or SECOND went down or up at now (the window and the hold are looked at first, as corner_tick). */
void corner_key(corner_t *c, int which, bool down, int64_t now, corner_out_t *o);
/* Time passes: a window that ended passes its press on; a chord held cfg.hold_ms opens. */
void corner_tick(corner_t *c, int64_t now, corner_out_t *o);
/* When corner_tick next has something to do (0: nothing pending). */
int64_t corner_due(const corner_t *c);
/* The key's down state is kept from Companion now (held back, or consumed). */
bool corner_held(const corner_t *c, int which);

/* ---- Satellite mode: the gate on the deck's events ---- */
/* Deck event ev on its way to Companion: the events to send instead, in order, into out (up to 3); returns
 * how many. Any event but a gesture key's (has, hw: corner_keys for the model and c->cfg) goes on as it is; a
 * gesture key's goes through the gate (corner_key). *open: the gesture made. */
int corner_sat_event(corner_t *c, bool has, const uint8_t hw[2], const deck_event_t *ev, int64_t now,
                     deck_event_t out[3], bool *open);
/* Time passes (corner_tick): the corner presses to send now, into out; returns how many. */
int corner_sat_tick(corner_t *c, bool has, const uint8_t hw[2], int64_t now, deck_event_t out[3], bool *open);

/* ---- Network Dock mode: the gate on the raw input reports ---- */
#define CORNER_REPORT_MAX 64 /* bytes of a report handled: all input data of every model */

typedef struct {
    bool on;        /* the model has the gesture, and it is on */
    bool gen2;      /* key reports 01 00 len16 + keys; gen1: 01 + keys */
    uint8_t hw[2];  /* the gesture keys' indexes */
    corner_t c;     /* c.cfg: the gesture it runs */
    uint8_t last[CORNER_REPORT_MAX]; /* the last key report given, as given */
    uint8_t last_len;                /* 0: none yet */
} corner_gate_t;

/* Up to 3 reports to send Companion instead of the one given, in order. */
typedef struct {
    int n;
    uint8_t r[3][CORNER_REPORT_MAX];
    uint8_t len[3];
    bool open;
    bool pressed; /* a corner's press reached Companion now (the box clicks then, never for a chord) */
} corner_gate_out_t;

/* For model m, running gesture cfg (corner_cfg_use: NULL is the default). */
void corner_gate_init(corner_gate_t *g, const deck_model_t *m, const corner_cfg_t *cfg);
/* Everything held back forgotten (the model and the gesture kept): the gate starts again (the box's hold on the
 * deck turned on or off). */
void corner_gate_reset(corner_gate_t *g);
/* A report from the deck on its way to Companion (its first CORNER_REPORT_MAX bytes): what to send instead.
 * A key report goes on at once with the held-back corners' bytes cleared (every other key's as given);
 * before it, the last key report with a corner released by the gate (a press passed on late, or a press
 * let go inside the window: its press, then this report's release). Any other report goes on as it is. */
void corner_gate_report(corner_gate_t *g, const uint8_t *r, size_t len, int64_t now, corner_gate_out_t *o);
/* Time passes: a window that ended sends the last key report again with that corner's press. */
void corner_gate_tick(corner_gate_t *g, int64_t now, corner_gate_out_t *o);
int64_t corner_gate_due(const corner_gate_t *g);

#ifdef __cplusplus
}
#endif
