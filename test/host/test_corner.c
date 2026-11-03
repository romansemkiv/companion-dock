/* The gesture that opens the maintenance screen on demand (components/deckproto/corner.c): by
 * default a tap of the top-right key arms it for 3 s; then the top-right and bottom-right keys pressed
 * together and held 2 s. The keys per model (the Original's right-to-left rows), unarmed (nothing delayed), armed
 * (the chord window, a lone press late and complete, releases in order), the armed window's end, the chord consumed
 * or let go early, the dock-mode report gate (held-back bits cleared, every other key at once, the click flag) and
 * the Satellite order of events through the helpers surfaces.c runs. Then the gesture as configured (off, any two
 * corners, the one tapped first, 1-3 taps, the hold), an invalid configuration taken as the default, and its
 * description for the log. FIRST is the key tapped first (by default top-right), SECOND the one held with it. */
#include <string.h>

#include "deckmodel/model.h"
#include "deckproto/corner.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const deck_model_t *M(uint16_t pid) { return deck_model_find(DECK_VID_ELGATO, pid); }

/* ---- the keys per model ---- */

static void test_corner_keys_per_model(void)
{
    uint8_t hw[2];
    TEST_ASSERT_TRUE(corner_keys(M(0x00BA), NULL, hw)); /* Module 32: (7,0) + (7,3) */
    TEST_ASSERT_EQUAL(7, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(31, hw[CORNER_SECOND]);
    TEST_ASSERT_TRUE(corner_keys(M(0x00C6), NULL, hw)); /* + XL: (8,0) + (8,3) */
    TEST_ASSERT_EQUAL(8, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(35, hw[CORNER_SECOND]);
    TEST_ASSERT_TRUE(corner_keys(M(0x0080), NULL, hw)); /* MK.2: (4,0) + (4,2) */
    TEST_ASSERT_EQUAL(4, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(14, hw[CORNER_SECOND]);
    TEST_ASSERT_TRUE(corner_keys(M(0x0084), NULL, hw)); /* +: (3,0) + (3,1) */
    TEST_ASSERT_EQUAL(3, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(7, hw[CORNER_SECOND]);
    TEST_ASSERT_TRUE(corner_keys(M(0x009A), NULL, hw)); /* Neo */
    TEST_ASSERT_EQUAL(3, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(7, hw[CORNER_SECOND]);
    TEST_ASSERT_TRUE(corner_keys(M(0x0090), NULL, hw)); /* Mini (gen1): (2,0) + (2,1) */
    TEST_ASSERT_EQUAL(2, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(5, hw[CORNER_SECOND]);
    TEST_ASSERT_TRUE(corner_keys(M(0x0060), NULL, hw)); /* the Original reports each row right to left */
    TEST_ASSERT_EQUAL(0, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(10, hw[CORNER_SECOND]);
    TEST_ASSERT_FALSE(corner_keys(M(0x0086), NULL, hw)); /* never the Pedal */
    /* the logical places agree with deck_model_controls (the Original's mirrored rows) */
    const uint16_t pids[] = {0x0060, 0x0090, 0x0080, 0x00BA, 0x0084, 0x009A, 0x00C6, 0x006C, 0x00B9, 0x00B8};
    for (size_t k = 0; k < sizeof pids / sizeof pids[0]; k++) {
        const deck_model_t *m = M(pids[k]);
        deck_ctl_t ctl[64];
        const int n = deck_model_controls(m, ctl, 64);
        TEST_ASSERT_TRUE(corner_keys(m, NULL, hw));
        for (int i = 0; i < n; i++) {
            if (ctl[i].kind != CTL_KEY_LCD) continue;
            const int want = ctl[i].col != m->cols - 1 ? -1 : ctl[i].row == 0 ? CORNER_FIRST : ctl[i].row == m->rows - 1 ? CORNER_SECOND : -1;
            TEST_ASSERT_EQUAL(want, corner_which(hw, ctl[i].hw));
        }
    }
}

/* ---- the state machine ---- */

/* A TR tap at t (down, up 80 ms later): it reaches Companion at once, and arms the gate. */
static void tap_tr(corner_t *c, int64_t t)
{
    corner_out_t o;
    corner_key(c, CORNER_FIRST, true, t, &o);
    corner_key(c, CORNER_FIRST, false, t + 80, &o);
}

static void test_unarmed_nothing_delayed(void)
{
    corner_t c;
    corner_out_t o;
    corner_init(&c, NULL);
    corner_key(&c, CORNER_SECOND, true, 1000, &o); /* not armed: at once */
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(CORNER_SECOND, o.ev[0].which);
    TEST_ASSERT_TRUE(o.ev[0].down);
    TEST_ASSERT_FALSE(corner_held(&c, CORNER_SECOND));
    TEST_ASSERT_EQUAL(0, corner_due(&c));
    corner_key(&c, CORNER_FIRST, true, 1020, &o); /* TR and BR together, unarmed: two presses, no gesture */
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_TRUE(o.ev[0].down);
    corner_tick(&c, 9000, &o);
    TEST_ASSERT_FALSE(o.open);
    corner_key(&c, CORNER_SECOND, false, 9100, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_FALSE(o.ev[0].down);
    /* the first tap: forwarded at once, both its press and its release */
    corner_init(&c, NULL);
    corner_key(&c, CORNER_FIRST, true, 0, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    corner_key(&c, CORNER_FIRST, false, 80, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_FALSE(o.ev[0].down);
}

static void test_armed_chord_held_opens(void)
{
    corner_t c;
    corner_out_t o;
    corner_init(&c, NULL);
    tap_tr(&c, 0); /* armed until 80 + 3000 */
    corner_key(&c, CORNER_FIRST, true, 1000, &o);
    TEST_ASSERT_EQUAL(0, o.n); /* held back */
    corner_key(&c, CORNER_SECOND, true, 1000 + CORNER_WINDOW_MS - 1, &o); /* the chord */
    TEST_ASSERT_EQUAL(0, o.n);
    TEST_ASSERT_EQUAL(1000 + CORNER_WINDOW_MS - 1 + CORNER_HOLD_DEFAULT_MS, corner_due(&c));
    corner_tick(&c, 1000 + CORNER_WINDOW_MS - 2 + CORNER_HOLD_DEFAULT_MS, &o);
    TEST_ASSERT_FALSE(o.open);
    corner_tick(&c, 1000 + CORNER_WINDOW_MS - 1 + CORNER_HOLD_DEFAULT_MS, &o);
    TEST_ASSERT_TRUE(o.open);
    TEST_ASSERT_EQUAL(0, o.n);
    corner_tick(&c, 9000, &o);
    TEST_ASSERT_FALSE(o.open); /* once */
    corner_key(&c, CORNER_FIRST, false, 9100, &o);
    corner_key(&c, CORNER_SECOND, false, 9200, &o);
    TEST_ASSERT_EQUAL(0, o.n); /* their releases go to nobody */
    TEST_ASSERT_FALSE(c.chord);
    /* used: the next TR / BR press isn't held back */
    corner_key(&c, CORNER_SECOND, true, 9300, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    /* BR first, then TR: the same */
    corner_init(&c, NULL);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_SECOND, true, 500, &o);
    corner_key(&c, CORNER_FIRST, true, 560, &o);
    TEST_ASSERT_EQUAL(0, o.n);
    corner_tick(&c, 560 + CORNER_HOLD_DEFAULT_MS, &o);
    TEST_ASSERT_TRUE(o.open);
}

static void test_armed_window_ends(void)
{
    corner_t c;
    corner_out_t o;
    corner_init(&c, NULL);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_FIRST, true, 80 + CORNER_ARM_MS - 1, &o); /* just inside: held back */
    TEST_ASSERT_EQUAL(0, o.n);
    corner_init(&c, NULL);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_FIRST, true, 80 + CORNER_ARM_MS, &o); /* over: at once, and no chord */
    TEST_ASSERT_EQUAL(1, o.n);
    corner_key(&c, CORNER_SECOND, true, 80 + CORNER_ARM_MS + 20, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    corner_tick(&c, 10000, &o);
    TEST_ASSERT_FALSE(o.open);
}

static void test_armed_lone_press_late_and_complete(void)
{
    corner_t c;
    corner_out_t o;
    corner_init(&c, NULL);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_SECOND, true, 1000, &o); /* armed: held back */
    TEST_ASSERT_EQUAL(0, o.n);
    TEST_ASSERT_TRUE(corner_held(&c, CORNER_SECOND));
    corner_tick(&c, 1000 + CORNER_WINDOW_MS, &o); /* the window over: the press, at most 120 ms late */
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(CORNER_SECOND, o.ev[0].which);
    TEST_ASSERT_TRUE(o.ev[0].down);
    corner_key(&c, CORNER_SECOND, false, 1500, &o); /* its release after it */
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_FALSE(o.ev[0].down);
    /* let go inside the window: the press and the release together */
    corner_key(&c, CORNER_SECOND, true, 2000, &o);
    corner_key(&c, CORNER_SECOND, false, 2050, &o);
    TEST_ASSERT_EQUAL(2, o.n);
    TEST_ASSERT_TRUE(o.ev[0].down);
    TEST_ASSERT_FALSE(o.ev[1].down);
    /* a TR tap in the window (forwarded, late and complete) arms again */
    corner_key(&c, CORNER_FIRST, true, 3000, &o);
    corner_key(&c, CORNER_FIRST, false, 3050, &o);
    TEST_ASSERT_EQUAL(2, o.n);
    corner_key(&c, CORNER_FIRST, true, 3050 + CORNER_ARM_MS - 10, &o); /* past the first tap's window: still armed */
    TEST_ASSERT_EQUAL(0, o.n);
}

static void test_armed_chord_released_early(void)
{
    corner_t c;
    corner_out_t o;
    corner_init(&c, NULL);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_FIRST, true, 1000, &o);
    corner_key(&c, CORNER_SECOND, true, 1050, &o);
    corner_key(&c, CORNER_FIRST, false, 2000, &o); /* before 2 s */
    TEST_ASSERT_EQUAL(0, o.n);
    corner_tick(&c, 9000, &o);
    TEST_ASSERT_FALSE(o.open); /* nothing opens */
    corner_key(&c, CORNER_FIRST, true, 9100, &o); /* pressed again while BR is down: swallowed */
    TEST_ASSERT_EQUAL(0, o.n);
    corner_key(&c, CORNER_SECOND, false, 9200, &o);
    corner_key(&c, CORNER_FIRST, false, 9300, &o);
    TEST_ASSERT_EQUAL(0, o.n); /* releases to nobody */
    corner_tick(&c, 20000, &o);
    TEST_ASSERT_FALSE(o.open);
}

static void test_late_tick_keeps_order(void)
{
    corner_t c;
    corner_out_t o;
    corner_init(&c, NULL);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_FIRST, true, 1000, &o);
    corner_key(&c, CORNER_SECOND, true, 1500, &o); /* TR's window long over, no tick ran: TR first, then BR waits */
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(CORNER_FIRST, o.ev[0].which);
    TEST_ASSERT_FALSE(c.chord);
    TEST_ASSERT_TRUE(corner_held(&c, CORNER_SECOND));
    /* a chord held past 2 s with no tick: the next event opens it */
    corner_init(&c, NULL);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_FIRST, true, 1000, &o);
    corner_key(&c, CORNER_SECOND, true, 1010, &o);
    corner_key(&c, CORNER_FIRST, false, 4000, &o);
    TEST_ASSERT_TRUE(o.open);
}

/* ---- Network Dock mode: the report gate ---- */

static size_t gen2_keys(uint8_t *r, int nkeys, const int *down, int ndown)
{
    memset(r, 0, CORNER_REPORT_MAX);
    r[0] = 0x01;
    r[2] = (uint8_t)nkeys;
    for (int i = 0; i < ndown; i++) r[4 + down[i]] = 1;
    return (size_t)(4 + nkeys);
}

static size_t gen1_keys(uint8_t *r, int nkeys, const int *down, int ndown)
{
    memset(r, 0, CORNER_REPORT_MAX);
    r[0] = 0x01;
    for (int i = 0; i < ndown; i++) r[1 + down[i]] = 1;
    return (size_t)(1 + nkeys);
}

/* A report sequence for the TR tap on an MK.2 (TR 4, BR 14). */
static void gate_tap_tr(corner_gate_t *g, int64_t t)
{
    corner_gate_out_t o;
    uint8_t r[CORNER_REPORT_MAX];
    size_t n = gen2_keys(r, 15, (const int[]){4}, 1);
    corner_gate_report(g, r, n, t, &o);
    n = gen2_keys(r, 15, NULL, 0);
    corner_gate_report(g, r, n, t + 80, &o);
}

static void test_gate_unarmed_passes(void)
{
    corner_gate_t g;
    corner_gate_out_t o;
    corner_gate_init(&g, M(0x0080), NULL);
    uint8_t r[CORNER_REPORT_MAX];
    size_t n = gen2_keys(r, 15, (const int[]){4, 14}, 2); /* both, unarmed: as given, at once */
    corner_gate_report(&g, r, n, 0, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(1, o.r[0][4 + 4]);
    TEST_ASSERT_EQUAL(1, o.r[0][4 + 14]);
    TEST_ASSERT_TRUE(o.pressed); /* a press reached Companion: the click */
    TEST_ASSERT_EQUAL(0, corner_gate_due(&g));
    corner_gate_tick(&g, 5000, &o);
    TEST_ASSERT_FALSE(o.open);
}

static void test_gate_armed_masks_and_passes_late(void)
{
    corner_gate_t g;
    corner_gate_out_t o;
    corner_gate_init(&g, M(0x0080), NULL);
    gate_tap_tr(&g, 0);
    uint8_t r[CORNER_REPORT_MAX];
    size_t n = gen2_keys(r, 15, (const int[]){14}, 1); /* BR, armed: held back */
    corner_gate_report(&g, r, n, 500, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(0, o.r[0][4 + 14]);
    TEST_ASSERT_FALSE(o.pressed);
    n = gen2_keys(r, 15, (const int[]){14, 7}, 2); /* another key: at once, BR still masked */
    corner_gate_report(&g, r, n, 530, &o);
    TEST_ASSERT_EQUAL(1, o.r[o.n - 1][4 + 7]);
    TEST_ASSERT_EQUAL(0, o.r[o.n - 1][4 + 14]);
    corner_gate_tick(&g, 500 + CORNER_WINDOW_MS, &o); /* the window over: the last report again, BR in */
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(1, o.r[0][4 + 14]);
    TEST_ASSERT_TRUE(o.pressed);
    /* let go inside the window: press, then release */
    corner_gate_init(&g, M(0x0090), NULL); /* Mini, gen1: TR 2, BR 5 */
    n = gen1_keys(r, 6, (const int[]){2}, 1);
    corner_gate_report(&g, r, n, 0, &o);
    n = gen1_keys(r, 6, NULL, 0);
    corner_gate_report(&g, r, n, 80, &o); /* the tap: armed */
    n = gen1_keys(r, 6, (const int[]){5}, 1);
    corner_gate_report(&g, r, n, 300, &o);
    TEST_ASSERT_EQUAL(0, o.r[o.n - 1][1 + 5]);
    n = gen1_keys(r, 6, NULL, 0);
    corner_gate_report(&g, r, n, 350, &o);
    TEST_ASSERT_EQUAL(2, o.n);
    TEST_ASSERT_EQUAL(1, o.r[0][1 + 5]);
    TEST_ASSERT_EQUAL(0, o.r[1][1 + 5]);
}

static void test_gate_chord_never_reaches_companion(void)
{
    corner_gate_t g;
    corner_gate_out_t o;
    corner_gate_init(&g, M(0x0060), NULL); /* the Original: TR index 0, BR index 10 */
    uint8_t r[CORNER_REPORT_MAX];
    size_t n = gen1_keys(r, 15, (const int[]){0}, 1);
    corner_gate_report(&g, r, n, 0, &o);
    n = gen1_keys(r, 15, NULL, 0);
    corner_gate_report(&g, r, n, 80, &o); /* the tap */
    n = gen1_keys(r, 15, (const int[]){0}, 1);
    corner_gate_report(&g, r, n, 1000, &o);
    n = gen1_keys(r, 15, (const int[]){0, 10}, 2);
    corner_gate_report(&g, r, n, 1040, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(0, o.r[0][1 + 0]);
    TEST_ASSERT_EQUAL(0, o.r[0][1 + 10]);
    TEST_ASSERT_FALSE(o.pressed); /* no click for a consumed press */
    n = gen1_keys(r, 15, (const int[]){0, 10, 6}, 3); /* another key meanwhile: passed at once */
    corner_gate_report(&g, r, n, 1100, &o);
    TEST_ASSERT_EQUAL(1, o.r[o.n - 1][1 + 6]);
    TEST_ASSERT_EQUAL(0, o.r[o.n - 1][1 + 0]);
    corner_gate_tick(&g, 1040 + CORNER_HOLD_DEFAULT_MS, &o);
    TEST_ASSERT_TRUE(o.open);
    TEST_ASSERT_FALSE(o.pressed);
    TEST_ASSERT_EQUAL(0, o.n);
    n = gen1_keys(r, 15, (const int[]){6}, 1); /* let go after: nothing of theirs */
    corner_gate_report(&g, r, n, 4000, &o);
    for (int i = 0; i < o.n; i++) {
        TEST_ASSERT_EQUAL(0, o.r[i][1 + 0]);
        TEST_ASSERT_EQUAL(0, o.r[i][1 + 10]);
    }
    corner_gate_reset(&g); /* the connection ended: nothing held back, nothing armed */
    TEST_ASSERT_EQUAL(0, corner_gate_due(&g));
    n = gen1_keys(r, 15, (const int[]){10}, 1);
    corner_gate_report(&g, r, n, 4100, &o);
    TEST_ASSERT_EQUAL(1, o.r[o.n - 1][1 + 10]); /* not armed after the reset */
}

static void test_gate_other_reports_pass(void)
{
    corner_gate_t g;
    corner_gate_out_t o;
    corner_gate_init(&g, M(0x0084), NULL); /* + */
    uint8_t r[CORNER_REPORT_MAX] = {0x01, 0x03, 0x05, 0x00, 0x01, 0x02}; /* an encoder turn */
    corner_gate_report(&g, r, 9, 0, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL_MEMORY(r, o.r[0], 9);
    corner_gate_init(&g, M(0x0086), NULL); /* the Pedal: no gesture, everything as given */
    uint8_t p[CORNER_REPORT_MAX];
    const size_t n = gen2_keys(p, 3, (const int[]){2}, 1);
    corner_gate_report(&g, p, n, 0, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(1, o.r[0][4 + 2]);
    TEST_ASSERT_EQUAL(0, corner_gate_due(&g));
}

/* ---- Satellite: the order Companion sees, through corner_sat_event / corner_sat_tick ---- */

typedef struct {
    int key; /* report index */
    bool down;
} sat_ev_t;

typedef struct {
    int64_t t;
    int key;
    bool down;
} sat_in_t;

static int run_sat_cfg(const deck_model_t *m, const corner_cfg_t *cfg, const sat_in_t *in, int n, int64_t end,
                       sat_ev_t *out, bool *opened)
{
    uint8_t hw[2];
    const bool has = corner_keys(m, cfg, hw);
    corner_t c;
    corner_init(&c, cfg);
    deck_event_t ev[3];
    bool open = false;
    *opened = false;
    int k = 0;
    int64_t t = 0;
    for (int i = 0; i <= n; i++) {
        const int64_t until = i < n ? in[i].t : end;
        for (; t < until; t += 10) { /* the surfaces task's passes */
            const int m2 = corner_sat_tick(&c, has, hw, t, ev, &open);
            *opened |= open;
            for (int j = 0; j < m2; j++) out[k++] = (sat_ev_t){ev[j].index, ev[j].pressed};
        }
        if (i == n) break;
        const deck_event_t e = {.kind = DECK_EV_KEY, .index = (uint8_t)in[i].key, .pressed = in[i].down};
        const int m2 = corner_sat_event(&c, has, hw, &e, in[i].t, ev, &open);
        *opened |= open;
        for (int j = 0; j < m2; j++) out[k++] = (sat_ev_t){ev[j].index, ev[j].pressed};
    }
    return k;
}

static int run_sat(const deck_model_t *m, const sat_in_t *in, int n, int64_t end, sat_ev_t *out, bool *opened)
{
    return run_sat_cfg(m, NULL, in, n, end, out, opened);
}

static void test_satellite_order(void)
{
    const deck_model_t *m = M(0x00BA); /* Module 32: TR 7, BR 31 */
    sat_ev_t out[24];
    bool opened;
    /* unarmed: everything at once, in order */
    const sat_in_t a[] = {{0, 31, true}, {20, 9, true}, {50, 9, false}, {300, 31, false}};
    int n = run_sat(m, a, 4, 400, out, &opened);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL(31, out[0].key);
    TEST_ASSERT_EQUAL(9, out[1].key);
    TEST_ASSERT_EQUAL(9, out[2].key);
    TEST_ASSERT_EQUAL(31, out[3].key);
    /* armed: a BR press, then another key 20 ms later: the other key first, BR after its window */
    const sat_in_t b[] = {{0, 7, true}, {80, 7, false}, {500, 31, true}, {520, 9, true}, {550, 9, false}, {900, 31, false}};
    n = run_sat(m, b, 6, 1000, out, &opened);
    TEST_ASSERT_EQUAL(6, n);
    TEST_ASSERT_EQUAL(7, out[0].key); /* the tap, at once */
    TEST_ASSERT_EQUAL(7, out[1].key);
    TEST_ASSERT_EQUAL(9, out[2].key);
    TEST_ASSERT_TRUE(out[2].down);
    TEST_ASSERT_EQUAL(9, out[3].key);
    TEST_ASSERT_EQUAL(31, out[4].key);
    TEST_ASSERT_TRUE(out[4].down);
    TEST_ASSERT_EQUAL(31, out[5].key);
    TEST_ASSERT_FALSE(out[5].down);
    /* the gesture with another key in it: only the tap and the other key reach Companion; it opens */
    const sat_in_t c[] = {{0, 7, true},     {80, 7, false},    {1000, 7, true},   {1030, 31, true},
                          {1100, 12, true}, {1150, 12, false}, {3500, 31, false}, {3600, 7, false}};
    n = run_sat(m, c, 8, 4000, out, &opened);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL(7, out[0].key);
    TEST_ASSERT_EQUAL(7, out[1].key);
    TEST_ASSERT_EQUAL(12, out[2].key);
    TEST_ASSERT_EQUAL(12, out[3].key);
    TEST_ASSERT_TRUE(opened);
}

/* ---- the gesture as configured ---- */

static corner_cfg_t CFG(bool on, int first, int second, int taps, int hold_ms)
{
    return (corner_cfg_t){.on = on, .first = (uint8_t)first, .second = (uint8_t)second, .taps = (uint8_t)taps,
                          .hold_ms = (uint16_t)hold_ms};
}

static void test_cfg_default_and_invalid(void)
{
    const corner_cfg_t d = corner_cfg_default(); /* the default gesture */
    TEST_ASSERT_TRUE(d.on);
    TEST_ASSERT_EQUAL(CORNER_TOP_RIGHT, d.first);
    TEST_ASSERT_EQUAL(CORNER_BOTTOM_RIGHT, d.second);
    TEST_ASSERT_EQUAL(1, d.taps);
    TEST_ASSERT_EQUAL(CORNER_HOLD_DEFAULT_MS, d.hold_ms);
    TEST_ASSERT_EQUAL(2000, CORNER_HOLD_DEFAULT_MS);
    TEST_ASSERT_TRUE(corner_cfg_ok(&d));
    const corner_cfg_t zero = {0}; /* none given yet */
    const corner_cfg_t bad[] = {zero,
                                CFG(true, CORNER_TOP_LEFT, CORNER_TOP_LEFT, 1, 2000), /* the same corner twice */
                                CFG(true, 4, CORNER_TOP_LEFT, 1, 2000),               /* not a corner */
                                CFG(true, CORNER_TOP_LEFT, 4, 1, 2000),
                                CFG(true, CORNER_TOP_LEFT, CORNER_TOP_RIGHT, 0, 2000), /* taps 1-3 */
                                CFG(true, CORNER_TOP_LEFT, CORNER_TOP_RIGHT, CORNER_TAPS_MAX + 1, 2000),
                                CFG(true, CORNER_TOP_LEFT, CORNER_TOP_RIGHT, 1, 0), /* no hold */
                                CFG(false, CORNER_TOP_LEFT, CORNER_TOP_LEFT, 1, 2000)};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE(corner_cfg_ok(&bad[i]));
        const corner_cfg_t u = corner_cfg_use(&bad[i]); /* taken as the default */
        TEST_ASSERT_TRUE(corner_cfg_eq(&u, &d));
        TEST_ASSERT_TRUE(corner_cfg_eq(&bad[i], &d));
        corner_t c;
        corner_init(&c, &bad[i]);
        TEST_ASSERT_TRUE(corner_cfg_eq(&c.cfg, &d));
        uint8_t hw[2];
        TEST_ASSERT_TRUE(corner_keys(M(0x0080), &bad[i], hw)); /* MK.2: TR 4, BR 14 */
        TEST_ASSERT_EQUAL(4, hw[CORNER_FIRST]);
        TEST_ASSERT_EQUAL(14, hw[CORNER_SECOND]);
    }
    const corner_cfg_t nul = corner_cfg_use(NULL);
    TEST_ASSERT_TRUE(corner_cfg_eq(&nul, &d));
    const corner_cfg_t off = CFG(false, CORNER_TOP_RIGHT, CORNER_BOTTOM_RIGHT, 1, 2000);
    TEST_ASSERT_TRUE(corner_cfg_ok(&off));
    TEST_ASSERT_FALSE(corner_cfg_eq(&off, &d));
    const corner_cfg_t h3 = CFG(true, CORNER_TOP_RIGHT, CORNER_BOTTOM_RIGHT, 1, 3000);
    TEST_ASSERT_FALSE(corner_cfg_eq(&h3, &d));
}

static void test_cfg_keys_any_two_corners(void)
{
    uint8_t hw[2];
    corner_cfg_t g = CFG(true, CORNER_TOP_LEFT, CORNER_BOTTOM_RIGHT, 1, 2000);
    TEST_ASSERT_TRUE(corner_keys(M(0x0080), &g, hw)); /* MK.2 5 x 3 */
    TEST_ASSERT_EQUAL(0, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(14, hw[CORNER_SECOND]);
    g = CFG(true, CORNER_BOTTOM_LEFT, CORNER_TOP_RIGHT, 1, 2000); /* the order given: the first is tapped */
    TEST_ASSERT_TRUE(corner_keys(M(0x0080), &g, hw));
    TEST_ASSERT_EQUAL(10, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(4, hw[CORNER_SECOND]);
    g = CFG(true, CORNER_TOP_LEFT, CORNER_BOTTOM_LEFT, 1, 2000);
    TEST_ASSERT_TRUE(corner_keys(M(0x0090), &g, hw)); /* Mini 3 x 2 */
    TEST_ASSERT_EQUAL(0, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(3, hw[CORNER_SECOND]);
    TEST_ASSERT_TRUE(corner_keys(M(0x0060), &g, hw)); /* the Original: each row right to left */
    TEST_ASSERT_EQUAL(4, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(14, hw[CORNER_SECOND]);
    g = CFG(true, CORNER_TOP_LEFT, CORNER_TOP_RIGHT, 1, 2000);
    TEST_ASSERT_TRUE(corner_keys(M(0x00BA), &g, hw)); /* Module 32, 8 x 4 */
    TEST_ASSERT_EQUAL(0, hw[CORNER_FIRST]);
    TEST_ASSERT_EQUAL(7, hw[CORNER_SECOND]);
    TEST_ASSERT_FALSE(corner_keys(M(0x0086), &g, hw)); /* never the Pedal */
    /* every corner, every model: its logical place, as deck_model_controls has it */
    const uint16_t pids[] = {0x0060, 0x0090, 0x0080, 0x00BA, 0x0084, 0x009A, 0x00C6, 0x006C, 0x00B9, 0x00B8};
    for (size_t k = 0; k < sizeof pids / sizeof pids[0]; k++) {
        const deck_model_t *m = M(pids[k]);
        deck_ctl_t ctl[64];
        const int n = deck_model_controls(m, ctl, 64);
        for (int a = 0; a < 4; a++) {
            const int b = (a + 1) % 4;
            g = CFG(true, a, b, 1, 2000);
            TEST_ASSERT_TRUE(corner_keys(m, &g, hw));
            int found = 0;
            for (int i = 0; i < n; i++) {
                if (ctl[i].kind != CTL_KEY_LCD) continue;
                const bool right = ctl[i].col == m->cols - 1, left = ctl[i].col == 0;
                const bool top = ctl[i].row == 0, bottom = ctl[i].row == m->rows - 1;
                const bool at[4] = {top && left, top && right, bottom && left, bottom && right};
                const int want = at[a] ? CORNER_FIRST : at[b] ? CORNER_SECOND : -1;
                TEST_ASSERT_EQUAL(want, corner_which(hw, ctl[i].hw));
                found += want >= 0;
            }
            TEST_ASSERT_EQUAL(2, found);
        }
    }
}

static void test_cfg_off(void)
{
    const corner_cfg_t off = CFG(false, CORNER_TOP_RIGHT, CORNER_BOTTOM_RIGHT, 1, 2000);
    uint8_t hw[2];
    TEST_ASSERT_FALSE(corner_keys(M(0x0080), &off, hw)); /* no key is the gesture's */
    /* the dock-mode gate: everything as given, at once, even after the tap */
    corner_gate_t g;
    corner_gate_out_t o;
    corner_gate_init(&g, M(0x0080), &off);
    TEST_ASSERT_FALSE(g.on);
    uint8_t r[CORNER_REPORT_MAX];
    size_t n = gen2_keys(r, 15, (const int[]){4}, 1);
    corner_gate_report(&g, r, n, 0, &o);
    n = gen2_keys(r, 15, NULL, 0);
    corner_gate_report(&g, r, n, 80, &o);
    n = gen2_keys(r, 15, (const int[]){4, 14}, 2);
    corner_gate_report(&g, r, n, 500, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    TEST_ASSERT_EQUAL(1, o.r[0][4 + 4]);
    TEST_ASSERT_EQUAL(1, o.r[0][4 + 14]);
    corner_gate_tick(&g, 10000, &o);
    TEST_ASSERT_FALSE(o.open);
    TEST_ASSERT_EQUAL(0, corner_gate_due(&g));
    /* Satellite: the default gesture made in full sends every event at once and opens nothing */
    sat_ev_t out[16];
    bool opened;
    const sat_in_t a[] = {{0, 4, true}, {80, 4, false}, {500, 4, true}, {520, 14, true}, {3000, 14, false}, {3100, 4, false}};
    TEST_ASSERT_EQUAL(6, run_sat_cfg(M(0x0080), &off, a, 6, 4000, out, &opened));
    TEST_ASSERT_FALSE(opened);
    TEST_ASSERT_EQUAL(4, out[2].key);
    TEST_ASSERT_TRUE(out[2].down);
    TEST_ASSERT_EQUAL(14, out[3].key);
    TEST_ASSERT_TRUE(out[3].down);
}

static void test_cfg_other_corners(void)
{
    /* top-left tapped, then top-left + bottom-left on an MK.2 (0 and 10): consumed and opens; the right column's
     * keys (the default ones) are ordinary keys now, never held back, even in the armed window */
    const corner_cfg_t tl = CFG(true, CORNER_TOP_LEFT, CORNER_BOTTOM_LEFT, 1, 2000);
    corner_gate_t g;
    corner_gate_out_t o;
    corner_gate_init(&g, M(0x0080), &tl);
    uint8_t r[CORNER_REPORT_MAX];
    size_t n = gen2_keys(r, 15, (const int[]){0}, 1);
    corner_gate_report(&g, r, n, 0, &o);
    TEST_ASSERT_EQUAL(1, o.r[o.n - 1][4 + 0]); /* the tap: at once */
    n = gen2_keys(r, 15, NULL, 0);
    corner_gate_report(&g, r, n, 80, &o); /* armed */
    n = gen2_keys(r, 15, (const int[]){14}, 1);
    corner_gate_report(&g, r, n, 300, &o);
    TEST_ASSERT_EQUAL(1, o.r[o.n - 1][4 + 14]); /* BR: not the gesture's, at once */
    n = gen2_keys(r, 15, (const int[]){14, 4}, 2);
    corner_gate_report(&g, r, n, 320, &o);
    TEST_ASSERT_EQUAL(1, o.r[o.n - 1][4 + 4]); /* TR too */
    n = gen2_keys(r, 15, NULL, 0);
    corner_gate_report(&g, r, n, 400, &o);
    n = gen2_keys(r, 15, (const int[]){0}, 1);
    corner_gate_report(&g, r, n, 1000, &o);
    TEST_ASSERT_EQUAL(0, o.r[o.n - 1][4 + 0]); /* TL, armed: held back */
    n = gen2_keys(r, 15, (const int[]){0, 10}, 2);
    corner_gate_report(&g, r, n, 1050, &o);
    TEST_ASSERT_EQUAL(0, o.r[o.n - 1][4 + 0]);
    TEST_ASSERT_EQUAL(0, o.r[o.n - 1][4 + 10]); /* the chord: neither */
    corner_gate_tick(&g, 1050 + 2000, &o);
    TEST_ASSERT_TRUE(o.open);
    /* the reset keeps the configuration */
    corner_gate_reset(&g);
    TEST_ASSERT_TRUE(corner_cfg_eq(&g.c.cfg, &tl));
}

static void test_cfg_taps(void)
{
    const corner_cfg_t two = CFG(true, CORNER_TOP_RIGHT, CORNER_BOTTOM_RIGHT, 2, 2000);
    corner_t c;
    corner_out_t o;
    /* one tap: not armed yet; the chord's presses go on at once, and it opens nothing */
    corner_init(&c, &two);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_FIRST, true, 1000, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    corner_key(&c, CORNER_SECOND, true, 1010, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    corner_tick(&c, 5000, &o);
    TEST_ASSERT_FALSE(o.open);
    /* two taps in a row (the second within 3 s of the first): armed; both taps reached Companion at once */
    corner_init(&c, &two);
    corner_key(&c, CORNER_FIRST, true, 0, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    corner_key(&c, CORNER_FIRST, false, 80, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    corner_key(&c, CORNER_FIRST, true, 80 + CORNER_ARM_MS - 100, &o); /* the second tap: not held back either */
    TEST_ASSERT_EQUAL(1, o.n);
    corner_key(&c, CORNER_FIRST, false, 80 + CORNER_ARM_MS - 20, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    const int64_t t = 80 + CORNER_ARM_MS + 500;
    corner_key(&c, CORNER_FIRST, true, t, &o);
    TEST_ASSERT_EQUAL(0, o.n); /* armed: held back */
    corner_key(&c, CORNER_SECOND, true, t + 30, &o);
    TEST_ASSERT_EQUAL(0, o.n);
    corner_tick(&c, t + 30 + 2000, &o);
    TEST_ASSERT_TRUE(o.open);
    /* taps too far apart: the count starts again */
    corner_init(&c, &two);
    tap_tr(&c, 0);
    tap_tr(&c, 80 + CORNER_ARM_MS);
    corner_key(&c, CORNER_FIRST, true, 80 + CORNER_ARM_MS + 500, &o);
    TEST_ASSERT_EQUAL(1, o.n); /* one tap counted: not armed */
    corner_key(&c, CORNER_FIRST, false, 80 + CORNER_ARM_MS + 580, &o); /* that press was the second */
    corner_key(&c, CORNER_FIRST, true, 80 + CORNER_ARM_MS + 900, &o);
    TEST_ASSERT_EQUAL(0, o.n); /* armed now */
    /* the other key pressed between the taps: the count starts again */
    corner_init(&c, &two);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_SECOND, true, 500, &o);
    TEST_ASSERT_EQUAL(1, o.n);
    corner_key(&c, CORNER_SECOND, false, 600, &o);
    tap_tr(&c, 1000);
    corner_key(&c, CORNER_FIRST, true, 1500, &o);
    TEST_ASSERT_EQUAL(1, o.n); /* one tap since: not armed */
    /* three taps */
    const corner_cfg_t three = CFG(true, CORNER_TOP_RIGHT, CORNER_BOTTOM_RIGHT, 3, 2000);
    corner_init(&c, &three);
    tap_tr(&c, 0);
    tap_tr(&c, 500);
    corner_key(&c, CORNER_FIRST, true, 1000, &o);
    TEST_ASSERT_EQUAL(1, o.n); /* two: not yet */
    corner_key(&c, CORNER_FIRST, false, 1080, &o);
    corner_key(&c, CORNER_FIRST, true, 1500, &o);
    TEST_ASSERT_EQUAL(0, o.n); /* three: armed */
    /* the chord made: the count is used up */
    corner_key(&c, CORNER_SECOND, true, 1520, &o);
    corner_tick(&c, 1520 + 2000, &o);
    TEST_ASSERT_TRUE(o.open);
    corner_key(&c, CORNER_FIRST, false, 4000, &o);
    corner_key(&c, CORNER_SECOND, false, 4010, &o);
    tap_tr(&c, 4500);
    corner_key(&c, CORNER_FIRST, true, 5000, &o);
    TEST_ASSERT_EQUAL(1, o.n); /* one tap since: not armed */
}

static void test_cfg_hold(void)
{
    const corner_cfg_t five = CFG(true, CORNER_TOP_RIGHT, CORNER_BOTTOM_RIGHT, 1, 5000);
    corner_t c;
    corner_out_t o;
    corner_init(&c, &five);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_FIRST, true, 1000, &o);
    corner_key(&c, CORNER_SECOND, true, 1040, &o);
    TEST_ASSERT_EQUAL(1040 + 5000, corner_due(&c));
    corner_tick(&c, 1040 + 2000, &o);
    TEST_ASSERT_FALSE(o.open); /* the default 2 s: not yet */
    corner_tick(&c, 1040 + 4999, &o);
    TEST_ASSERT_FALSE(o.open);
    corner_tick(&c, 1040 + 5000, &o);
    TEST_ASSERT_TRUE(o.open);
    /* let go at 3 s: nothing */
    corner_init(&c, &five);
    tap_tr(&c, 0);
    corner_key(&c, CORNER_FIRST, true, 1000, &o);
    corner_key(&c, CORNER_SECOND, true, 1040, &o);
    corner_key(&c, CORNER_SECOND, false, 1040 + 3000, &o);
    TEST_ASSERT_EQUAL(0, o.n);
    corner_tick(&c, 20000, &o);
    TEST_ASSERT_FALSE(o.open);
}

static void test_cfg_describe(void)
{
    char s[96];
    const corner_cfg_t d = corner_cfg_default();
    corner_cfg_describe(&d, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("tap top-right, then hold top-right + bottom-right 2 s", s);
    corner_cfg_t g = CFG(true, CORNER_BOTTOM_LEFT, CORNER_TOP_LEFT, 2, 3000);
    corner_cfg_describe(&g, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("tap bottom-left twice, then hold bottom-left + top-left 3 s", s);
    g = CFG(true, CORNER_TOP_LEFT, CORNER_BOTTOM_RIGHT, 3, 5000);
    corner_cfg_describe(&g, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("tap top-left 3 times, then hold top-left + bottom-right 5 s", s);
    g = CFG(false, CORNER_TOP_LEFT, CORNER_BOTTOM_RIGHT, 3, 5000);
    corner_cfg_describe(&g, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("off", s);
    const corner_cfg_t zero = {0}; /* as the gates take it: the default */
    corner_cfg_describe(&zero, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("tap top-right, then hold top-right + bottom-right 2 s", s);
    corner_cfg_describe(&d, s, 8); /* cut, terminated */
    TEST_ASSERT_EQUAL_STRING("tap top", s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_corner_keys_per_model);
    RUN_TEST(test_unarmed_nothing_delayed);
    RUN_TEST(test_armed_chord_held_opens);
    RUN_TEST(test_armed_window_ends);
    RUN_TEST(test_armed_lone_press_late_and_complete);
    RUN_TEST(test_armed_chord_released_early);
    RUN_TEST(test_late_tick_keeps_order);
    RUN_TEST(test_gate_unarmed_passes);
    RUN_TEST(test_gate_armed_masks_and_passes_late);
    RUN_TEST(test_gate_chord_never_reaches_companion);
    RUN_TEST(test_gate_other_reports_pass);
    RUN_TEST(test_satellite_order);
    RUN_TEST(test_cfg_default_and_invalid);
    RUN_TEST(test_cfg_keys_any_two_corners);
    RUN_TEST(test_cfg_off);
    RUN_TEST(test_cfg_other_corners);
    RUN_TEST(test_cfg_taps);
    RUN_TEST(test_cfg_hold);
    RUN_TEST(test_cfg_describe);
    return UNITY_END();
}
