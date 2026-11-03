#include <stdio.h>
#include <string.h>

#include "satproto/lineasm.h"
#include "satproto/session.h"
#include "unity.h"

#define MAX_SENT 16
static char sent[MAX_SENT][512];
static int nsent, nsent_total; /* nsent: lines kept in sent[]; nsent_total: every line */
static int ready_n, too_old_n, ks_n, clear_n, bright_n, add_n;
static char old_ver[32];
static sat_caps_t ready_caps;
static sat_key_state_t last_ks;
static char ks_dev[64], ks_ctl[64], ks_bmp[64];
static bool add_ok;
static char add_msg[64], add_dev[64];
static char tx[16384];
static sat_session_t S;
static char wire[16384]; /* every byte sent, back to back (the TCP stream) */
static size_t nwire;

static void cb_send(void *c, const char *d, size_t n)
{
    (void)c;
    nsent_total++;
    TEST_ASSERT_TRUE(nwire + n <= sizeof wire);
    memcpy(wire + nwire, d, n);
    nwire += n;
    if (nsent < MAX_SENT && n < sizeof(sent[0])) {
        memcpy(sent[nsent], d, n);
        sent[nsent][n] = '\0';
        nsent++;
    }
}
static void cb_ready(void *c, const sat_caps_t *caps) { (void)c; ready_n++; ready_caps = *caps; }
static void cb_old(void *c, const char *v) { (void)c; too_old_n++; snprintf(old_ver, sizeof old_ver, "%s", v); }
static void cb_ks(void *c, const sat_key_state_t *k)
{
    (void)c;
    ks_n++;
    last_ks = *k;
    snprintf(ks_dev, sizeof ks_dev, "%s", k->device_id);
    snprintf(ks_ctl, sizeof ks_ctl, "%s", k->control_id);
    last_ks.device_id = ks_dev;
    last_ks.control_id = ks_ctl;
    if (k->bitmap_b64) {
        snprintf(ks_bmp, sizeof ks_bmp, "%s", k->bitmap_b64);
        last_ks.bitmap_b64 = ks_bmp;
    }
}
static void cb_clear(void *c, const char *d) { (void)c; (void)d; clear_n++; }
static void cb_bright(void *c, const char *d, int p) { (void)c; (void)d; (void)p; bright_n++; }
static void cb_add(void *c, const char *d, bool ok, const char *m)
{
    (void)c;
    add_n++;
    add_ok = ok;
    snprintf(add_dev, sizeof add_dev, "%s", d);
    snprintf(add_msg, sizeof add_msg, "%s", m ? m : "");
}

static const sat_session_cb_t CB = {cb_send, cb_ready, cb_old, cb_ks, cb_clear, cb_bright, cb_add};

void setUp(void)
{
    nsent = nsent_total = ready_n = too_old_n = ks_n = clear_n = bright_n = add_n = 0;
    nwire = 0;
    memset(&last_ks, 0, sizeof last_ks);
    sat_session_init(&S, &CB, NULL, tx, sizeof tx);
    sat_session_reset(&S, 0);
}
void tearDown(void) {}

static void feed(const char *line, uint32_t t)
{
    char b[1024];
    snprintf(b, sizeof b, "%s", line);
    sat_session_on_line(&S, b, t);
}

static void handshake(uint32_t t)
{
    feed("BEGIN CompanionVersion=5.1.1 ApiVersion=1.14.0", t);
    feed("CAPS SUBSCRIPTIONS=1 NONSQUARE=1 ROTARY_AMOUNT=1", t);
}

static void test_begin_then_caps_ready(void)
{
    feed("BEGIN CompanionVersion=5.1.1 ApiVersion=1.14.0", 0);
    TEST_ASSERT_EQUAL(SAT_S_WAIT_CAPS, S.state);
    TEST_ASSERT_EQUAL(0, ready_n);
    feed("CAPS SUBSCRIPTIONS=1 NONSQUARE=1 ROTARY_AMOUNT=1", 0);
    TEST_ASSERT_EQUAL(1, ready_n);
    TEST_ASSERT_EQUAL(SAT_S_READY, S.state);
    TEST_ASSERT_EQUAL(14, ready_caps.minor);
    TEST_ASSERT_TRUE(ready_caps.nonsquare);
    TEST_ASSERT_TRUE(ready_caps.rotary_amount);
    TEST_ASSERT_FALSE(ready_caps.leds);
    TEST_ASSERT_EQUAL_STRING("5.1.1", ready_caps.companion_version);
}

static void test_old_api_too_old(void)
{
    feed("BEGIN CompanionVersion=4.2.0 ApiVersion=1.10.0", 0);
    TEST_ASSERT_EQUAL(1, too_old_n);
    TEST_ASSERT_EQUAL_STRING("1.10.0", old_ver);
    TEST_ASSERT_EQUAL(SAT_S_TOO_OLD, S.state);
    feed("CAPS NONSQUARE=1", 0);
    TEST_ASSERT_EQUAL(0, ready_n);
}

static void test_version_compare_numeric_and_malformed(void)
{
    feed("BEGIN ApiVersion=1.9.0", 0);
    TEST_ASSERT_EQUAL(SAT_S_TOO_OLD, S.state);
    sat_session_reset(&S, 0);
    feed("BEGIN ApiVersion=1.11.0", 0);
    TEST_ASSERT_EQUAL(SAT_S_WAIT_CAPS, S.state);
    sat_session_reset(&S, 0);
    feed("BEGIN ApiVersion=2.0.0", 0);
    TEST_ASSERT_EQUAL(SAT_S_WAIT_CAPS, S.state);
    const char *bad[] = {"BEGIN ApiVersion=abc", "BEGIN ApiVersion=1.14", "BEGIN ApiVersion=1.14.0.1", "BEGIN"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        sat_session_reset(&S, 0);
        feed(bad[i], 0);
        TEST_ASSERT_EQUAL_MESSAGE(SAT_S_TOO_OLD, S.state, bad[i]);
    }
}

static void test_ping_reply(void)
{
    feed("PING abc", 0);
    TEST_ASSERT_EQUAL(1, nsent);
    TEST_ASSERT_EQUAL_STRING("PONG abc\n", sent[0]);
    feed("PING a b=c \"q\"", 0);
    TEST_ASSERT_EQUAL_STRING("PONG a b=c \"q\"\n", sent[1]);
}

static void test_ping_reply_buffer_bounds(void)
{
    struct { char buf[16]; char canary[8]; } m;
    sat_session_t s2;
    char line[64];
    memset(&m, 0xAA, sizeof m);
    sat_session_init(&s2, &CB, NULL, m.buf, sizeof m.buf);
    /* payload txcap-6 bytes: "PONG "+payload+"\n" fits but NUL does not -> dropped */
    memcpy(line, "PING ", 5);
    memset(line + 5, 'p', sizeof m.buf - 6);
    line[5 + sizeof m.buf - 6] = '\0';
    sat_session_on_line(&s2, line, 0);
    TEST_ASSERT_EQUAL(0, nsent);
    for (size_t i = 0; i < sizeof m.canary; i++) TEST_ASSERT_EQUAL_HEX8(0xAA, (uint8_t)m.canary[i]);
    /* payload txcap-7 bytes: fits exactly */
    memcpy(line, "PING ", 5);
    memset(line + 5, 'p', sizeof m.buf - 7);
    line[5 + sizeof m.buf - 7] = '\0';
    sat_session_on_line(&s2, line, 0);
    TEST_ASSERT_EQUAL(1, nsent);
    TEST_ASSERT_EQUAL(sizeof m.buf - 1, strlen(sent[0]));
    for (size_t i = 0; i < sizeof m.canary; i++) TEST_ASSERT_EQUAL_HEX8(0xAA, (uint8_t)m.canary[i]);
}

static void test_tick_sends_ping_every_second(void)
{
    handshake(0);
    sat_session_tick(&S, 999);
    TEST_ASSERT_EQUAL(0, nsent);
    sat_session_tick(&S, 1000);
    TEST_ASSERT_EQUAL(1, nsent);
    TEST_ASSERT_EQUAL_STRING("PING \n", sent[0]);
    sat_session_tick(&S, 1500);
    TEST_ASSERT_EQUAL(1, nsent);
    sat_session_tick(&S, 2000);
    TEST_ASSERT_EQUAL(2, nsent);
}

static void test_no_ping_before_begin(void)
{
    sat_session_tick(&S, 5000);
    TEST_ASSERT_EQUAL(0, nsent);
}

static void test_alive_timeout(void)
{
    feed("PING x", 0);
    TEST_ASSERT_TRUE(sat_session_alive(&S, 4999));
    TEST_ASSERT_FALSE(sat_session_alive(&S, 5000));
}

static void test_key_state_parsed(void)
{
    handshake(0);
    feed("KEY-STATE DEVICEID=\"streamdeck:A1\" CONTROLID=\"1/2\" BITMAP=\"AAAA\" COLOR=\"#ff8000\" PRESSED=0", 1);
    TEST_ASSERT_EQUAL(1, ks_n);
    TEST_ASSERT_EQUAL_STRING("streamdeck:A1", last_ks.device_id);
    TEST_ASSERT_EQUAL_STRING("1/2", last_ks.control_id);
    TEST_ASSERT_EQUAL(4, last_ks.bitmap_b64_len);
    TEST_ASSERT_TRUE(last_ks.has_color);
    TEST_ASSERT_EQUAL(255, last_ks.r);
    TEST_ASSERT_EQUAL(128, last_ks.g);
    TEST_ASSERT_EQUAL(0, last_ks.b);
    TEST_ASSERT_FALSE(last_ks.pressed);
}

static void test_key_state_key_fallback(void)
{
    handshake(0);
    feed("KEY-STATE DEVICEID=\"d\" KEY=\"0/3\" PRESSED=true", 1);
    TEST_ASSERT_EQUAL(1, ks_n);
    TEST_ASSERT_EQUAL_STRING("0/3", last_ks.control_id);
    TEST_ASSERT_NULL(last_ks.bitmap_b64);
    TEST_ASSERT_FALSE(last_ks.has_color);
    TEST_ASSERT_TRUE(last_ks.pressed);
}

static void test_key_state_bad_color_and_missing_ids(void)
{
    handshake(0);
    feed("KEY-STATE DEVICEID=\"d\" CONTROLID=\"0/0\" COLOR=\"rgb(1,2,3)\"", 1);
    TEST_ASSERT_FALSE(last_ks.has_color);
    feed("KEY-STATE DEVICEID=\"d\" COLOR=\"#ffffff\"", 1);
    feed("KEY-STATE CONTROLID=\"0/0\"", 1);
    TEST_ASSERT_EQUAL(1, ks_n);
}

static void test_add_device_format(void)
{
    sat_device_desc_t d = {"streamdeck:A1", "Elgato Stream Deck XL", "{}", true, NULL};
    TEST_ASSERT_TRUE(sat_session_add_device(&S, &d));
    TEST_ASSERT_EQUAL(1, nsent);
    const char *l = sent[0];
    TEST_ASSERT_EQUAL(0, strncmp(l, "ADD-DEVICE DEVICEID=\"streamdeck:A1\" ", 36));
    TEST_ASSERT_NOT_NULL(strstr(l, "PRODUCT_NAME=\"Elgato Stream Deck XL\""));
    TEST_ASSERT_NOT_NULL(strstr(l, "LAYOUT_MANIFEST=\"e30=\""));
    TEST_ASSERT_NOT_NULL(strstr(l, "BRIGHTNESS=1"));
    TEST_ASSERT_NOT_NULL(strstr(l, "SERIAL=\"streamdeck:A1\""));
    TEST_ASSERT_NOT_NULL(strstr(l, "SERIAL_IS_UNIQUE=1"));
    TEST_ASSERT_NULL(strstr(l, "CAN_CHANGE_PAGE"));
    TEST_ASSERT_EQUAL('\n', l[strlen(l) - 1]);
}

static void test_add_device_change_page_label(void)
{
    sat_device_desc_t d = {"d", "P", "{}", false, "Horizontal Swipe Changes Page"};
    TEST_ASSERT_TRUE(sat_session_add_device(&S, &d));
    TEST_ASSERT_NOT_NULL(strstr(sent[0], "CAN_CHANGE_PAGE=\"Horizontal Swipe Changes Page\""));
    TEST_ASSERT_NOT_NULL(strstr(sent[0], "BRIGHTNESS=0"));
}

static void test_add_device_too_big_dropped(void)
{
    char small[64];
    sat_session_t s2;
    sat_session_init(&s2, &CB, NULL, small, sizeof small);
    sat_device_desc_t d = {"streamdeck:A1", "Elgato Stream Deck XL", "{}", true, NULL};
    TEST_ASSERT_FALSE(sat_session_add_device(&s2, &d));
    TEST_ASSERT_EQUAL(0, nsent);
    /* Large manifest in a mid-size buffer: also dropped, nothing partial sent. */
    char mid[200];
    char manifest[300];
    memset(manifest, 'x', sizeof manifest - 1);
    manifest[sizeof manifest - 1] = '\0';
    sat_session_init(&s2, &CB, NULL, mid, sizeof mid);
    d.manifest_json = manifest;
    TEST_ASSERT_FALSE(sat_session_add_device(&s2, &d));
    TEST_ASSERT_EQUAL(0, nsent);
}

static void test_add_result(void)
{
    feed("ADD-DEVICE OK DEVICEID=\"x\"", 0);
    TEST_ASSERT_EQUAL(1, add_n);
    TEST_ASSERT_TRUE(add_ok);
    TEST_ASSERT_EQUAL_STRING("x", add_dev);
    feed("ADD-DEVICE ERROR DEVICEID=\"x\" MESSAGE=\"bad\"", 0);
    TEST_ASSERT_EQUAL(2, add_n);
    TEST_ASSERT_FALSE(add_ok);
    TEST_ASSERT_EQUAL_STRING("bad", add_msg);
}

/* without ROTARY_AMOUNT (Companion 5.0, API < 1.14) a turn of n steps is n single-step lines, so
 * fast turns don't lose steps; with it one line carries the signed count. Both capped at 100 steps,
 * as the reference client (MAX_ROTATION_STEPS). */
static const char ROT_UP[] = "KEY-ROTATE DEVICEID=\"d\" CONTROLID=\"2/1\" KEY=\"2/1\" DIRECTION=1 \n";
static const char ROT_DOWN[] = "KEY-ROTATE DEVICEID=\"d\" CONTROLID=\"2/1\" KEY=\"2/1\" DIRECTION=-1 \n";

/* The stream holds `lines` copies of line, and nothing else. */
static void assert_wire_lines(const char *line, int lines)
{
    const size_t l = strlen(line);
    TEST_ASSERT_EQUAL((size_t)lines * l, nwire);
    for (int i = 0; i < lines; i++) TEST_ASSERT_EQUAL_MEMORY(line, wire + (size_t)i * l, l);
}

static void test_rotate_steps_without_rotary_amount(void)
{
    feed("BEGIN ApiVersion=1.13.0", 0);
    feed("CAPS SUBSCRIPTIONS=1", 0);
    sat_session_rotate(&S, "d", "2/1", 3);
    /* a line a step, the same bytes as ever; in one send, not 3 */
    TEST_ASSERT_EQUAL_INT(1, nsent);
    assert_wire_lines(ROT_UP, 3);
    nwire = 0;
    sat_session_rotate(&S, "d", "2/1", -2);
    TEST_ASSERT_EQUAL_INT(2, nsent);
    assert_wire_lines(ROT_DOWN, 2);
    nsent = nsent_total = 0;
    nwire = 0;
    sat_session_rotate(&S, "d", "2/1", 250);
    assert_wire_lines(ROT_UP, 100);
    TEST_ASSERT_EQUAL_INT(1, nsent_total);
    nsent = nsent_total = 0;
    nwire = 0;
    sat_session_rotate(&S, "d", "2/1", 0);
    TEST_ASSERT_EQUAL_INT(0, nsent_total);
}

/* a txbuf too small for every step's line: sent in batches of whole lines (as many as fit with the NUL), the
 * stream the same as one send a line; a txbuf too small for even one line: nothing, as before. */
static void test_rotate_batches_flush_when_txbuf_full(void)
{
    static char small[300];
    const int l = (int)strlen(ROT_UP);
    for (int cap = l - 2; cap <= 4 * l + 2; cap++)
        for (int steps = 1; steps <= 12; steps++) {
            sat_session_init(&S, &CB, NULL, small, (size_t)cap);
            sat_session_reset(&S, 0);
            feed("BEGIN ApiVersion=1.13.0", 0);
            feed("CAPS SUBSCRIPTIONS=1", 0);
            nsent = nsent_total = 0;
            nwire = 0;
            sat_session_rotate(&S, "d", "2/1", steps);
            const int per = (cap - 1) / l; /* lines a batch */
            if (per == 0) {
                TEST_ASSERT_EQUAL_INT(0, nsent_total);
                continue;
            }
            assert_wire_lines(ROT_UP, steps);
            TEST_ASSERT_EQUAL_INT((steps + per - 1) / per, nsent_total);
            for (int i = 0; i < nsent; i++) TEST_ASSERT_EQUAL(0, strlen(sent[i]) % (size_t)l);
        }
}

static void test_rotate_amount_with_rotary_amount(void)
{
    handshake(0);
    nsent = 0;
    sat_session_rotate(&S, "d", "2/1", 3);
    TEST_ASSERT_NOT_NULL(strstr(sent[0], "DIRECTION=3 "));
    sat_session_rotate(&S, "d", "2/1", -2);
    TEST_ASSERT_EQUAL_STRING("KEY-ROTATE DEVICEID=\"d\" CONTROLID=\"2/1\" KEY=\"2/1\" DIRECTION=-2 \n", sent[1]);
    sat_session_rotate(&S, "d", "2/1", 250);
    TEST_ASSERT_NOT_NULL(strstr(sent[2], "DIRECTION=100 "));
    sat_session_rotate(&S, "d", "2/1", -250);
    TEST_ASSERT_NOT_NULL(strstr(sent[3], "DIRECTION=-100 "));
    TEST_ASSERT_EQUAL_INT(4, nsent);
}

static void test_key_press_format(void)
{
    sat_session_key(&S, "d", "2/1", true);
    TEST_ASSERT_EQUAL_STRING("KEY-PRESS DEVICEID=\"d\" CONTROLID=\"2/1\" KEY=\"2/1\" PRESSED=1 \n", sent[0]);
}

static void test_remove_and_change_page(void)
{
    sat_session_remove_device(&S, "d");
    TEST_ASSERT_EQUAL_STRING("REMOVE-DEVICE DEVICEID=\"d\" \n", sent[0]);
    sat_session_change_page(&S, "d", true);
    TEST_ASSERT_EQUAL_STRING("CHANGE-PAGE DEVICEID=\"d\" DIRECTION=1 \n", sent[1]);
    sat_session_change_page(&S, "d", false);
    TEST_ASSERT_EQUAL_STRING("CHANGE-PAGE DEVICEID=\"d\" DIRECTION=0 \n", sent[2]);
}

static void test_clear_and_brightness(void)
{
    handshake(0);
    feed("KEYS-CLEAR DEVICEID=\"d\"", 1);
    feed("BRIGHTNESS DEVICEID=\"d\" VALUE=50", 1);
    TEST_ASSERT_EQUAL(1, clear_n);
    TEST_ASSERT_EQUAL(1, bright_n);
}

static void test_session_reset_then_ready_again(void)
{
    handshake(0);
    TEST_ASSERT_EQUAL(1, ready_n);
    sat_session_reset(&S, 100);
    TEST_ASSERT_EQUAL(SAT_S_WAIT_BEGIN, S.state);
    handshake(100);
    TEST_ASSERT_EQUAL(2, ready_n);
}

static void test_unknown_command_ignored(void)
{
    handshake(0);
    int r = ready_n;
    feed("VARIABLE-VALUE VARIABLE=\"a\" VALUE=\"b\"", 1);
    feed("", 1);
    TEST_ASSERT_EQUAL(r, ready_n);
    TEST_ASSERT_EQUAL(0, ks_n + clear_n + bright_n + add_n + nsent);
}

/* --- line assembler --- */
static char got[8][64];
static int ngot;
static void on_line(void *c, char *l) { (void)c; if (ngot < 8) snprintf(got[ngot++], 64, "%s", l); }

static void test_lineasm_split_and_crlf(void)
{
    char buf[64];
    sat_lineasm_t la;
    ngot = 0;
    sat_lineasm_init(&la, buf, sizeof buf);
    sat_lineasm_feed(&la, "A=1\r\nB", 6, on_line, NULL);
    sat_lineasm_feed(&la, "=2\n", 3, on_line, NULL);
    TEST_ASSERT_EQUAL(2, ngot);
    TEST_ASSERT_EQUAL_STRING("A=1", got[0]);
    TEST_ASSERT_EQUAL_STRING("B=2", got[1]);
}

static void test_lineasm_overflow_discards_until_newline(void)
{
    char buf[8];
    sat_lineasm_t la;
    ngot = 0;
    sat_lineasm_init(&la, buf, sizeof buf);
    const char *in = "0123456789ABC\nOK\n";
    sat_lineasm_feed(&la, in, strlen(in), on_line, NULL);
    TEST_ASSERT_EQUAL(1, ngot);
    TEST_ASSERT_EQUAL_STRING("OK", got[0]);
}

static void test_lineasm_exact_fit_and_split_overflow(void)
{
    char buf[8];
    sat_lineasm_t la;
    ngot = 0;
    sat_lineasm_init(&la, buf, sizeof buf);
    sat_lineasm_feed(&la, "1234567\n", 8, on_line, NULL); /* cap-1 chars: fits */
    sat_lineasm_feed(&la, "12345678", 8, on_line, NULL);  /* overflow, newline in next chunk */
    sat_lineasm_feed(&la, "9\nZ\n", 4, on_line, NULL);
    TEST_ASSERT_EQUAL(2, ngot);
    TEST_ASSERT_EQUAL_STRING("1234567", got[0]);
    TEST_ASSERT_EQUAL_STRING("Z", got[1]);
}

/* ---- The old line assembler, kept here as the reference the bulk-copy one must match ---- */

static uint32_t rng = 0x9E3779B9u;
static uint32_t rnd(void) /* xorshift32: the same cases every run */
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void ref_lineasm_feed(sat_lineasm_t *la, const char *data, size_t n, void (*cb)(void *ctx, char *line),
                             void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (!la->discarding && la->len > 0) {
                la->buf[la->len] = '\0';
                la->len = 0;
                if (cb) cb(ctx, la->buf);
            }
            la->len = 0;
            la->discarding = false;
            continue;
        }
        if (la->discarding) continue;
        if (la->cap == 0 || la->len >= la->cap - 1) {
            la->discarding = true;
            la->len = 0;
            continue;
        }
        la->buf[la->len++] = c;
    }
}

typedef struct {
    char s[2048];
    size_t n;
    int lines;
} line_log_t;

static void log_line(void *c, char *l) /* every line as given, and its end */
{
    line_log_t *g = c;
    const size_t k = strlen(l);
    TEST_ASSERT_TRUE(g->n + k + 1 <= sizeof g->s);
    memcpy(g->s + g->n, l, k);
    g->n += k;
    g->s[g->n++] = '|';
    g->lines++;
}

/* the same lines (and the same state and buffer after every feed) as the old assembler, over 100k streams of
 * 1..6 feeds, cap 0..12: CRs anywhere, empty lines, lines of exactly cap - 1, longer ones, lines across feeds. */
static void test_lineasm_matches_old(void)
{
    static const char A[] = "ab\r\n\n\r=x";
    static line_log_t ga, gb;
    char bufa[16], bufb[16], data[40];
    long lines = 0, drops = 0;
    for (int it = 0; it < 100000; it++) {
        const size_t cap = rnd() % 13;
        memset(bufa, 0x5A, sizeof bufa);
        memset(bufb, 0x5A, sizeof bufb);
        sat_lineasm_t la, lb;
        sat_lineasm_init(&la, cap ? bufa : NULL, cap);
        sat_lineasm_init(&lb, cap ? bufb : NULL, cap);
        ga.n = gb.n = 0;
        ga.lines = gb.lines = 0;
        const int feeds = 1 + (int)(rnd() % 6);
        for (int f = 0; f < feeds; f++) {
            const size_t n = rnd() % sizeof data;
            for (size_t i = 0; i < n; i++)
                data[i] = rnd() % 10 ? A[rnd() % (sizeof A - 1)] : (char)rnd(); /* NUL too */
            sat_lineasm_feed(&la, data, n, log_line, &ga);
            ref_lineasm_feed(&lb, data, n, log_line, &gb);
            TEST_ASSERT_EQUAL(lb.len, la.len);
            TEST_ASSERT_EQUAL(lb.discarding, la.discarding);
            TEST_ASSERT_EQUAL_MEMORY(bufb, bufa, sizeof bufa);
            TEST_ASSERT_EQUAL(gb.lines, ga.lines);
            TEST_ASSERT_EQUAL(gb.n, ga.n);
            if (gb.n) TEST_ASSERT_EQUAL_MEMORY(gb.s, ga.s, gb.n);
            drops += la.discarding;
        }
        lines += ga.lines;
    }
    TEST_ASSERT_TRUE(lines > 100000 && drops > 10000); /* lines out, and long lines dropped, both well covered */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_begin_then_caps_ready);
    RUN_TEST(test_old_api_too_old);
    RUN_TEST(test_version_compare_numeric_and_malformed);
    RUN_TEST(test_ping_reply);
    RUN_TEST(test_ping_reply_buffer_bounds);
    RUN_TEST(test_tick_sends_ping_every_second);
    RUN_TEST(test_no_ping_before_begin);
    RUN_TEST(test_alive_timeout);
    RUN_TEST(test_key_state_parsed);
    RUN_TEST(test_key_state_key_fallback);
    RUN_TEST(test_key_state_bad_color_and_missing_ids);
    RUN_TEST(test_add_device_format);
    RUN_TEST(test_add_device_change_page_label);
    RUN_TEST(test_add_device_too_big_dropped);
    RUN_TEST(test_add_result);
    RUN_TEST(test_rotate_steps_without_rotary_amount);
    RUN_TEST(test_rotate_batches_flush_when_txbuf_full);
    RUN_TEST(test_rotate_amount_with_rotary_amount);
    RUN_TEST(test_key_press_format);
    RUN_TEST(test_remove_and_change_page);
    RUN_TEST(test_clear_and_brightness);
    RUN_TEST(test_session_reset_then_ready_again);
    RUN_TEST(test_unknown_command_ignored);
    RUN_TEST(test_lineasm_split_and_crlf);
    RUN_TEST(test_lineasm_overflow_discards_until_newline);
    RUN_TEST(test_lineasm_exact_fit_and_split_overflow);
    RUN_TEST(test_lineasm_matches_old);
    return UNITY_END();
}
