/* dockcache: the Network Dock mode image cache and gate. Companion's traffic is made with the
 * deckproto builders (the same layouts the host library writes) and fed to the gate, which writes to
 * a simulated deck ("sut"); the same traffic goes straight to a reference deck ("ref"), as plain
 * passthrough would deliver it. Once a hold has ended and the replay is done the two decks must show
 * exactly the same thing: the latest image of every target, fills and clears in their order.
 * The simulated deck takes one image at a time, as the real ones do: a report of another target
 * while an image is being taken is counted as `interleaved` and must never happen. */
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "dockcache.h"

/* ---- memory (counted, failures injectable) ---- */

static int live, fail_alloc_in = -1;

static void *t_alloc(size_t n)
{
    if (fail_alloc_in == 0) return NULL;
    if (fail_alloc_in > 0) fail_alloc_in--;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
static void *t_resize(void *p, size_t n) { return realloc(p, n); }
static void t_release(void *p)
{
    if (!p) return;
    live--;
    free(p);
}
static const dc_mem_t MEM = {t_alloc, t_resize, t_release};

/* ---- simulated deck ---- */

#define ID_IMG(h) ((1ull << 32) | (h))
#define ID_FILL(r, g, b) ((2ull << 32) | ((uint64_t)(r) << 16) | ((uint64_t)(g) << 8) | (b))
#define ID_LOGO (3ull << 32)

typedef struct {
    const deck_model_t *m;
    uint64_t key[DC_KEYS_MAX];
    uint64_t info;
    uint64_t *lcd; /* lcd_w * lcd_h pixel owners */
    int brightness;
    bool on; /* an image being taken */
    deck_img_target_t t;
    uint16_t next;
    uint32_t h;
    int interleaved;
    int out_writes, feat_writes;
    int fail_in; /* writes left before one fails; -1 never */
} vdeck_t;

static void vd_init(vdeck_t *v, const deck_model_t *m)
{
    memset(v, 0, sizeof *v);
    v->m = m;
    v->fail_in = -1;
    if (m->lcd_w) v->lcd = calloc((size_t)m->lcd_w * m->lcd_h, sizeof *v->lcd);
}

static void vd_free(vdeck_t *v) { free(v->lcd); }

/* The deck forgot everything it was shown (images dropped during a quarantine). */
static void vd_wipe(vdeck_t *v)
{
    const deck_model_t *m = v->m;
    const int b = v->brightness;
    vd_free(v);
    vd_init(v, m);
    v->brightness = b;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static bool same(const deck_img_target_t *a, const deck_img_target_t *b)
{
    if (a->kind != b->kind) return false;
    if (a->kind == DECK_TGT_KEY) return a->key == b->key;
    if (a->kind == DECK_TGT_LCD) return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
    return true;
}

static void vd_output(vdeck_t *v, const uint8_t *r, size_t len)
{
    deck_img_target_t t;
    uint16_t ch;
    bool last;
    const uint8_t *pl;
    size_t n;
    if (!deckproto_parse_image_report(v->m, r, len, &t, &ch, &last, &pl, &n)) return;
    if (v->on && !same(&v->t, &t)) v->interleaved++;
    if (ch == deckproto_image_first_chunk(v->m)) {
        v->on = true;
        v->t = t;
        v->h = 2166136261u;
    } else if (!v->on || !same(&v->t, &t) || ch != v->next) {
        v->on = false; /* broken: shown never */
        return;
    }
    v->h = fnv(v->h, pl, n);
    v->next = (uint16_t)(ch + 1);
    if (!last) return;
    v->on = false;
    const uint64_t id = ID_IMG(v->h);
    if (t.kind == DECK_TGT_KEY) v->key[t.key] = id;
    if (t.kind == DECK_TGT_INFO) v->info = id;
    if (t.kind == DECK_TGT_LCD)
        for (unsigned y = t.y; y < (unsigned)t.y + t.h && y < v->m->lcd_h; y++)
            for (unsigned x = t.x; x < (unsigned)t.x + t.w && x < v->m->lcd_w; x++) v->lcd[y * v->m->lcd_w + x] = id;
}

/* The whole panel: every key, the strip and the info bar (Elgato's HID reference: 03 05 "fills the
 * entire LCD", colour at +2; core models the Plus / Plus XL strip and the Neo info bar inside the
 * one panel). */
static void vd_panel(vdeck_t *v, uint64_t id)
{
    for (int k = 0; k < DC_KEYS_MAX; k++) v->key[k] = id;
    if (v->lcd)
        for (size_t i = 0; i < (size_t)v->m->lcd_w * v->m->lcd_h; i++) v->lcd[i] = id;
    if (v->m->info_w) v->info = id;
}

static void vd_feature(vdeck_t *v, const uint8_t *r, size_t len)
{
    if (len < 2 || r[0] != 0x03) return;
    if (r[1] == 0x06 && len >= 6 && r[2] < DC_KEYS_MAX) v->key[r[2]] = ID_FILL(r[3], r[4], r[5]);
    if (r[1] == 0x05 && len >= 5) vd_panel(v, ID_FILL(r[2], r[3], r[4]));
    if (r[1] == 0x02) vd_panel(v, ID_LOGO);
    if (r[1] == 0x08 && len >= 3) v->brightness = r[2];
}

static bool vd_write(void *ctx, bool feature, const uint8_t *r, size_t len)
{
    vdeck_t *v = ctx;
    if (v->fail_in == 0) return false;
    if (v->fail_in > 0) v->fail_in--;
    if (feature) {
        v->feat_writes++;
        vd_feature(v, r, len);
    } else {
        v->out_writes++;
        vd_output(v, r, len);
    }
    return true;
}

/* ---- fixture: the gate, the deck behind it, and the reference deck ---- */

typedef struct {
    const deck_model_t *m;
    dc_t dc;
    vdeck_t sut, ref;
    dc_io_t io;
} fx_t;

static fx_t F;
static uint32_t now_ms;
static uint8_t imgbuf[400000];
static uint8_t rep[8191];

void setUp(void)
{
    live = 0;
    fail_alloc_in = -1;
    now_ms = 1000;
}
void tearDown(void) {}

static void fx_init(uint16_t pid)
{
    F.m = deck_model_find(0x0FD9, pid);
    TEST_ASSERT_NOT_NULL(F.m);
    dc_init(&F.dc, F.m, &MEM);
    vd_init(&F.sut, F.m);
    vd_init(&F.ref, F.m);
    F.io = (dc_io_t){.write = vd_write, .ctx = &F.sut};
}

static void fx_done(void)
{
    dc_reset(&F.dc);
    TEST_ASSERT_EQUAL_size_t(0, dc_bytes(&F.dc));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, live, "every buffer freed");
    vd_free(&F.sut);
    vd_free(&F.ref);
}

static size_t make_img(uint32_t seed, size_t n)
{
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        imgbuf[i] = (uint8_t)(x >> 16);
    }
    return n;
}

static void send_out(const uint8_t *r, size_t len)
{
    F.io.now_ms = now_ms;
    TEST_ASSERT_TRUE(dc_output(&F.dc, &F.io, r, len));
    vd_output(&F.ref, r, len);
}

static void send_feat(const uint8_t *r, size_t len)
{
    F.io.now_ms = now_ms;
    TEST_ASSERT_TRUE(dc_feature(&F.dc, &F.io, r, len));
    vd_feature(&F.ref, r, len);
}

/* Companion draws an image on a target: its reports [from, upto) (upto -1: to the end). */
static void send_writer(deck_img_writer_t *w, int from, int upto)
{
    for (int i = 0; deckproto_img_next(w, rep); i++) {
        if (upto >= 0 && i >= upto) break;
        if (i >= from) send_out(rep, F.m->report_len);
    }
}

static void host_key_part(uint8_t hw, uint32_t seed, size_t n, int from, int upto)
{
    make_img(seed, n);
    deck_img_writer_t w;
    deckproto_img_key(&w, F.m, hw, imgbuf, n);
    send_writer(&w, from, upto);
}

static void host_key(uint8_t hw, uint32_t seed) { host_key_part(hw, seed, 2500, 0, -1); }

static void host_lcd(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t seed, size_t n)
{
    make_img(seed, n);
    deck_img_writer_t wr;
    deckproto_img_lcd(&wr, F.m, x, y, w, h, imgbuf, n);
    send_writer(&wr, 0, -1);
}

static void host_info(uint32_t seed)
{
    make_img(seed, 1800);
    deck_img_writer_t wr;
    deckproto_img_info(&wr, F.m, imgbuf, 1800);
    send_writer(&wr, 0, -1);
}

static void host_fill(uint8_t hw, uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t f[32] = {0x03, 0x06, hw, r, g, b};
    send_feat(f, 32);
}

static void host_simple(uint8_t cmd, uint8_t arg)
{
    const uint8_t f[32] = {0x03, cmd, arg};
    send_feat(f, 32);
}

static void pump(void)
{
    F.io.now_ms = now_ms;
    TEST_ASSERT_TRUE(dc_pump(&F.dc, &F.io));
}

static int nkeys(void) { return F.m->cols * F.m->rows; }

/* The box's message frame for key hw; returns the id the deck shows for it. */
static uint64_t box_key(uint8_t hw, uint32_t seed)
{
    const size_t n = make_img(seed ^ 0xB0B0u, 900);
    TEST_ASSERT_TRUE(dc_box_key(&F.dc, hw, imgbuf, n));
    vdeck_t v;
    vd_init(&v, F.m);
    deck_img_writer_t w;
    deckproto_img_key(&w, F.m, hw, imgbuf, n);
    while (deckproto_img_next(&w, rep)) vd_output(&v, rep, F.m->report_len);
    const uint64_t id = v.key[hw];
    vd_free(&v);
    return id;
}

/* Message frames on every key; the ids. */
static void box_all(uint64_t *ids, uint32_t seed)
{
    for (int k = 0; k < nkeys(); k++) ids[k] = box_key((uint8_t)k, seed + (uint32_t)k);
}

static void assert_message_shown(const uint64_t *ids)
{
    for (int k = 0; k < nkeys(); k++) TEST_ASSERT_EQUAL_UINT64_MESSAGE(ids[k], F.sut.key[k], "message frame on the key");
}

static void assert_same_but(int interleaved);
static void assert_same(void) { assert_same_but(0); }

/* The two decks show the same; `interleaved` images the box couldn't help cutting (0 normally). */
static void assert_same_but(int interleaved)
{
    char msg[48];
    for (int k = 0; k < DC_KEYS_MAX; k++) {
        snprintf(msg, sizeof msg, "key %d", k);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(F.ref.key[k], F.sut.key[k], msg);
    }
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(F.ref.info, F.sut.info, "info bar");
    if (F.m->lcd_w)
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(F.ref.lcd, F.sut.lcd, (size_t)F.m->lcd_w * F.m->lcd_h * sizeof(uint64_t), "strip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(F.ref.brightness, F.sut.brightness, "brightness");
    TEST_ASSERT_EQUAL_INT_MESSAGE(interleaved, F.sut.interleaved, "the box never wrote into an image being taken");
    TEST_ASSERT_TRUE(dc_settled(&F.dc));
}

static void fill_all_keys(uint32_t seed)
{
    for (int k = 0; k < nkeys(); k++) host_key((uint8_t)k, seed + (uint32_t)k);
}

/* ---- tests ---- */

/* Without a hold everything goes on unchanged and at once. */
static void test_passthrough_unchanged(void)
{
    fx_init(0x0084); /* Plus: keys, strip */
    fill_all_keys(100);
    host_lcd(0, 0, 800, 100, 7, 5000);
    host_lcd(200, 0, 200, 100, 8, 2000);
    host_simple(0x08, 70);
    host_fill(3, 1, 2, 3);
    assert_same();
    TEST_ASSERT_EQUAL_UINT32(0, F.dc.stats.held_reports);
    fx_done();

    fx_init(0x009A); /* Neo: info bar, touch keys (fill 8/9) */
    fill_all_keys(1);
    host_info(3);
    host_fill(8, 9, 9, 9);
    assert_same();
    fx_done();
}

/* A message over a full page while Companion keeps drawing; afterwards exactly the
 * latest image per target. */
static void test_message_then_replay_gives_latest(void)
{
    fx_init(0x00BA); /* Module 32 */
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(1000);
    dc_set_hold(&F.dc, true);
    box_all(ids, 1);
    pump();
    assert_message_shown(ids);

    for (int k = 0; k < 10; k++) host_key((uint8_t)k, 2000 + (uint32_t)k); /* a new page, partly */
    host_key(3, 3000);                                                     /* key 3 twice: the latest counts */
    host_fill(20, 10, 20, 30);
    host_simple(0x08, 30); /* brightness goes on at once */
    assert_message_shown(ids);
    TEST_ASSERT_EQUAL_INT(30, F.sut.brightness);
    TEST_ASSERT_EQUAL_UINT32(11, F.dc.stats.held_images);

    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    TEST_ASSERT_EQUAL_UINT32(32, F.dc.stats.replayed);
    fx_done();
}

/* A sequence already being passed on when the hold starts is passed on to its end before any
 * message frame; then the frames go on. */
static void test_open_sequence_finishes_before_frames(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    host_key_part(5, 77, 2500, 0, 2); /* 2 of 3 reports */
    dc_set_hold(&F.dc, true);
    box_all(ids, 2);
    pump();
    TEST_ASSERT_EQUAL_UINT64(F.ref.key[0], F.sut.key[0]); /* nothing drawn yet */
    host_key_part(5, 77, 2500, 2, -1);                    /* its end: passed on, then the frames */
    assert_message_shown(ids);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* A sequence held back when the hold ends stays held to its end, then goes on after the replay. */
static void test_held_sequence_finishing_after_hold(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 3);
    pump();
    host_key_part(7, 555, 3000, 0, 2);
    dc_set_hold(&F.dc, false);
    pump();
    TEST_ASSERT_EQUAL_UINT64(F.ref.key[7], F.sut.key[7]); /* the old image back meanwhile */
    host_key_part(7, 555, 3000, 2, -1);
    assert_same();
    fx_done();
}

/* Incomplete sequences are dropped, not cached: the previous image stays. A new one replaces the
 * old only when complete. */
static void test_incomplete_dropped(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 4);
    pump();
    host_key_part(2, 900, 3000, 0, 2); /* never finished */
    now_ms += DC_STALE_MS + 1;
    pump();
    host_key_part(4, 901, 3000, 0, 1); /* a gap: report 1 never comes */
    host_key_part(4, 901, 3000, 2, -1);
    host_key(6, 902); /* complete */
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* A panel clear in the hold is held too and replayed at its turn: before what came after it. */
static void test_clear_held_replayed_in_order(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 5);
    pump();
    host_key(1, 40);
    host_simple(0x05, 0); /* clearPanel */
    host_key(9, 41);
    host_fill(10, 5, 5, 5);
    assert_message_shown(ids);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* A reset (Companion closing) in the hold: held, replayed at its turn. */
static void test_reset_held_replayed(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 6);
    pump();
    host_simple(0x02, 0);
    assert_message_shown(ids);
    host_key(0, 11);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* Strip regions and the info bar are never under the message: they go on during the hold. */
static void test_strip_and_info_pass_during_hold(void)
{
    fx_init(0x0084);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 7);
    pump();
    host_lcd(0, 0, 200, 100, 50, 1500);
    host_lcd(600, 0, 200, 100, 51, 1500);
    TEST_ASSERT_EQUAL_MEMORY(F.ref.lcd, F.sut.lcd, (size_t)800 * 100 * sizeof(uint64_t));
    assert_message_shown(ids);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();

    fx_init(0x009A);
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 8);
    pump();
    host_info(60);
    TEST_ASSERT_EQUAL_UINT64(F.ref.info, F.sut.info);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* A quarantine lifted: the deck lost everything; the replay brings back every target, overlapping
 * strip regions in their order. A region drawn over whole drops the regions it covers. */
static void test_replay_all_after_quarantine(void)
{
    fx_init(0x0084);
    fill_all_keys(10);
    host_fill(2, 9, 8, 7);
    host_lcd(0, 0, 800, 100, 70, 6000); /* the strip cleared, then cells */
    host_lcd(0, 0, 200, 100, 71, 1500);
    host_lcd(200, 0, 200, 100, 72, 1500);
    host_lcd(100, 0, 200, 100, 73, 1500); /* overlaps two cells */
    vd_wipe(&F.sut);
    dc_replay_all(&F.dc);
    pump();
    assert_same();

    host_lcd(0, 0, 800, 100, 74, 6000); /* covers every region kept */
    int used = 0;
    for (int i = 0; i < DC_LCD_MAX; i++) used += F.dc.lcd[i].kind != DC_NONE;
    TEST_ASSERT_EQUAL_INT(1, used);
    vd_wipe(&F.sut);
    dc_replay_all(&F.dc);
    pump();
    assert_same();
    fx_done();
}

/* While holding, a quarantine replay rewrites the message frames and the strip; the keys come
 * back when the hold ends. */
static void test_replay_all_during_hold(void)
{
    fx_init(0x0084);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    host_lcd(0, 0, 800, 100, 70, 6000);
    dc_set_hold(&F.dc, true);
    box_all(ids, 9);
    pump();
    vd_wipe(&F.sut);
    dc_replay_all(&F.dc);
    pump();
    assert_message_shown(ids);
    TEST_ASSERT_EQUAL_MEMORY(F.ref.lcd, F.sut.lcd, (size_t)800 * 100 * sizeof(uint64_t));
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* An image too big to keep, in a hold: what was held is flushed and the rest passed on, so nothing
 * Companion sent is lost (the key then shows it over the message). */
static void test_too_big_is_flushed_not_lost(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 10);
    pump();
    const deck_img_target_t t = {.kind = DECK_TGT_KEY, .key = 12};
    const size_t big = (dc_target_cap(F.m, &t) + 2) * 1016;
    host_key_part(12, 123, big, 0, -1);
    TEST_ASSERT_EQUAL_UINT64(F.ref.key[12], F.sut.key[12]);
    TEST_ASSERT_EQUAL_UINT32(1, F.dc.stats.uncached);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* No memory for a sequence in a hold: it can't be held, so it is passed on. */
static void test_no_memory_passes_on(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 11);
    pump();
    fail_alloc_in = 0;
    host_key(13, 321);
    fail_alloc_in = -1;
    TEST_ASSERT_EQUAL_UINT64(F.ref.key[13], F.sut.key[13]);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* A key with nothing kept gets black when the message ends (gen2); a gen1 key keeps the frame. */
static void test_uncached_key_after_message(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    dc_set_hold(&F.dc, true);
    box_all(ids, 12);
    pump();
    dc_set_hold(&F.dc, false);
    pump();
    for (int k = 0; k < 32; k++) TEST_ASSERT_EQUAL_UINT64(ID_FILL(0, 0, 0), F.sut.key[k]);
    TEST_ASSERT_TRUE(dc_settled(&F.dc));
    fx_done();

    fx_init(0x0063); /* Mini */
    dc_set_hold(&F.dc, true);
    box_all(ids, 13);
    pump();
    host_key(1, 5); /* held, then replayed */
    dc_set_hold(&F.dc, false);
    pump();
    TEST_ASSERT_EQUAL_UINT64(ids[0], F.sut.key[0]);
    TEST_ASSERT_EQUAL_UINT64(F.ref.key[1], F.sut.key[1]);
    fx_done();
}

/* gen1 decks (Mini, Original) hold and replay the same way. */
static void test_gen1_hold_and_replay(void)
{
    const uint16_t pids[] = {0x0063, 0x0060};
    for (size_t i = 0; i < 2; i++) {
        fx_init(pids[i]);
        uint64_t ids[DC_KEYS_MAX];
        for (int k = 0; k < nkeys(); k++) host_key_part((uint8_t)k, 10 + (uint32_t)k, 15606, 0, -1);
        dc_set_hold(&F.dc, true);
        box_all(ids, 14);
        pump();
        assert_message_shown(ids);
        host_key_part(2, 99, 15606, 0, -1);
        dc_set_hold(&F.dc, false);
        pump();
        assert_same();
        fx_done();
    }
}

/* A write that fails stops the work; what is left is done at the next call, whole. */
static void test_write_failure_resumes(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 15);
    pump();
    host_key(0, 1234);
    dc_set_hold(&F.dc, false);
    F.sut.fail_in = 4;
    F.io.now_ms = now_ms;
    TEST_ASSERT_FALSE(dc_pump(&F.dc, &F.io));
    TEST_ASSERT_FALSE(dc_settled(&F.dc));
    F.sut.fail_in = -1;
    pump(); /* the entry cut short is written again whole, from its first report */
    assert_same();
    fx_done();
}

/* A connection replaced mid-replay (hand-over): the box's write failed half-way, the new
 * connection's first image comes before the box can retry. The cut image is the one interleaving
 * (nothing can prevent it: the old connection is gone); the rewrite then makes the deck right. */
static void test_handover_after_failed_write(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 18);
    pump();
    host_key(0, 1234);
    dc_set_hold(&F.dc, false);
    F.sut.fail_in = 2; /* cut inside a 3-report image */
    F.io.now_ms = now_ms;
    TEST_ASSERT_FALSE(dc_pump(&F.dc, &F.io));
    F.sut.fail_in = -1;
    dc_end_sequences(&F.dc); /* the new connection */
    host_key(20, 77);
    pump();
    assert_same_but(1);
    fx_done();
}

/* A hand-over drops the old connection's images under way: a held one can't finish, and one being
 * passed on no longer keeps the box's frames out. */
static void test_handover_drops_sequences(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    host_key_part(5, 66, 2500, 0, 2); /* passed on, cut by the connection going */
    dc_set_hold(&F.dc, true);
    box_all(ids, 19);
    host_key_part(6, 67, 2500, 0, 1); /* held, cut too */
    pump();
    TEST_ASSERT_EQUAL_UINT64(F.ref.key[0], F.sut.key[0]); /* frames wait for key 5 */
    dc_end_sequences(&F.dc);
    pump();
    assert_message_shown(ids);
    dc_set_hold(&F.dc, false);
    pump();
    F.ref.on = false; /* the reference deck never got the rest either */
    assert_same_but(1); /* key 5's cut image, interleaved by the frames */
    fx_done();
}

/* during a hold a report of no image the box knows (its sequence given way to a 5th, or gone
 * stale) is passed on, never dropped. */
static void test_orphan_passed_on_in_hold(void)
{
    fx_init(0x00BA);
    for (int k = 0; k < 4; k++) host_key_part((uint8_t)k, 300 + (uint32_t)k, 2500, 0, 1);
    host_key_part(4, 304, 2500, 0, 1); /* a 5th: key 0's gives way */
    dc_set_hold(&F.dc, true);
    const int before = F.sut.out_writes;
    host_key_part(0, 300, 2500, 1, -1); /* key 0's rest: no sequence known */
    TEST_ASSERT_EQUAL_INT(before + 2, F.sut.out_writes);
    TEST_ASSERT_EQUAL_UINT32(0, F.dc.stats.held_reports);
    fx_done();
}

/* a strip image passed on but not kept forgets the regions it covers, and takes no slot from a
 * region it doesn't cover. */
static void test_uncached_region_keeps_others(void)
{
    fx_init(0x0084);
    for (int i = 0; i < DC_LCD_MAX; i++) host_lcd((uint16_t)(i * 100), 0, 100, 100, 500 + (uint32_t)i, 800);
    fail_alloc_in = 0;
    host_lcd(50, 0, 100, 100, 600, 800); /* over half of regions 0 and 1, can't be kept */
    fail_alloc_in = -1;
    int used = 0;
    for (int i = 0; i < DC_LCD_MAX; i++) used += F.dc.lcd[i].kind != DC_NONE;
    TEST_ASSERT_EQUAL_INT(DC_LCD_MAX, used); /* no slot taken, nothing covered whole */
    fail_alloc_in = 0;
    host_lcd(300, 0, 100, 100, 601, 800); /* over region 3 whole */
    fail_alloc_in = -1;
    used = 0;
    for (int i = 0; i < DC_LCD_MAX; i++) used += F.dc.lcd[i].kind != DC_NONE;
    TEST_ASSERT_EQUAL_INT(DC_LCD_MAX - 1, used); /* region 3 forgotten: the deck shows another image there */
    fx_done();
}

/* a panel clear held in a message (Companion connecting meanwhile) covers the strip and the info
 * bar too. Replayed at its turn, then everything newer than it, strip and info included. */
static void test_held_clear_strip_and_info(void)
{
    fx_init(0x0084); /* Plus */
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    host_lcd(0, 0, 800, 100, 30, 5000);
    dc_set_hold(&F.dc, true);
    box_all(ids, 20);
    pump();
    host_simple(0x05, 0); /* Companion connects: blank() */
    fill_all_keys(40);    /* held */
    host_lcd(0, 0, 200, 100, 31, 1500);
    host_lcd(400, 0, 200, 100, 32, 1500); /* passed on, newer than the clear */
    assert_message_shown(ids);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();

    fx_init(0x009A); /* Neo */
    fill_all_keys(10);
    host_info(33);
    dc_set_hold(&F.dc, true);
    box_all(ids, 21);
    pump();
    host_simple(0x05, 0);
    host_info(34); /* passed on */
    host_key(2, 35);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* A clear passed on outside a hold blackens the strip too: a quarantine replay must not bring back
 * the regions drawn before it. */
static void test_passed_clear_then_replay_all(void)
{
    fx_init(0x0084);
    fill_all_keys(10);
    host_lcd(0, 0, 800, 100, 50, 5000);
    host_lcd(200, 0, 200, 100, 51, 1500);
    host_simple(0x05, 0);
    host_lcd(600, 0, 200, 100, 52, 1500);
    host_key(1, 53);
    vd_wipe(&F.sut);
    dc_replay_all(&F.dc);
    pump();
    assert_same();
    fx_done();
}

/* New message text: new frames replace the old ones on the deck. Frames need a hold. */
static void test_frames_replace_and_need_hold(void)
{
    fx_init(0x00BA);
    uint64_t ids[DC_KEYS_MAX];
    TEST_ASSERT_FALSE(dc_box_key(&F.dc, 0, imgbuf, 100)); /* no hold */
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    box_all(ids, 16);
    pump();
    TEST_ASSERT_TRUE(dc_settled(&F.dc));
    box_all(ids, 17);
    TEST_ASSERT_FALSE(dc_settled(&F.dc));
    pump();
    assert_message_shown(ids);
    TEST_ASSERT_FALSE(dc_box_key(&F.dc, 32, imgbuf, 100)); /* no such key */
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* A hold that ends before any frame was drawn only replays what it held. */
static void test_short_hold(void)
{
    fx_init(0x00BA);
    fill_all_keys(10);
    dc_set_hold(&F.dc, true);
    host_key(4, 44);
    dc_set_hold(&F.dc, false);
    const int before = F.sut.out_writes;
    pump();
    assert_same();
    TEST_ASSERT_TRUE(F.sut.out_writes > before);
    TEST_ASSERT_EQUAL_UINT32(1, F.dc.stats.replayed);
    fx_done();
}

/* The box's frame on a strip region / the info bar (hold_all); the id the deck shows for it. */
static uint64_t box_lcd(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t seed)
{
    const size_t n = make_img(seed ^ 0xC0C0u, 1200);
    TEST_ASSERT_TRUE(dc_box_lcd(&F.dc, x, y, w, h, imgbuf, n));
    vdeck_t v;
    vd_init(&v, F.m);
    deck_img_writer_t wr;
    deckproto_img_lcd(&wr, F.m, x, y, w, h, imgbuf, n);
    while (deckproto_img_next(&wr, rep)) vd_output(&v, rep, F.m->report_len);
    const uint64_t id = v.lcd[(size_t)y * F.m->lcd_w + x];
    vd_free(&v);
    return id;
}

static uint64_t box_info(uint32_t seed)
{
    const size_t n = make_img(seed ^ 0xD0D0u, 1000);
    TEST_ASSERT_TRUE(dc_box_info(&F.dc, imgbuf, n));
    vdeck_t v;
    vd_init(&v, F.m);
    deck_img_writer_t wr;
    deckproto_img_info(&wr, F.m, imgbuf, n);
    while (deckproto_img_next(&wr, rep)) vd_output(&v, rep, F.m->report_len);
    const uint64_t id = v.info;
    vd_free(&v);
    return id;
}

/* The maintenance screen on demand (hold_all): the strip's images are held back too, the box's own strip
 * frames go on it, and when the hold ends the deck shows exactly what passthrough would have. */
static void test_hold_all_strip(void)
{
    fx_init(0x0084);
    uint64_t ids[DC_KEYS_MAX];
    TEST_ASSERT_FALSE(dc_box_lcd(&F.dc, 0, 0, 200, 100, imgbuf, 100)); /* no hold */
    fill_all_keys(10);
    for (int c = 0; c < 4; c++) host_lcd((uint16_t)(200 * c), 0, 200, 100, 30 + (uint32_t)c, 1500);
    dc_set_hold(&F.dc, true);
    TEST_ASSERT_FALSE(dc_box_lcd(&F.dc, 0, 0, 200, 100, imgbuf, 100)); /* the message's hold: keys only */
    dc_set_hold_all(&F.dc, true);
    box_all(ids, 7);
    uint64_t cell[4];
    for (int c = 0; c < 4; c++) cell[c] = box_lcd((uint16_t)(200 * c), 0, 200, 100, 70 + (uint32_t)c);
    pump();
    assert_message_shown(ids);
    for (int c = 0; c < 4; c++) TEST_ASSERT_EQUAL_UINT64(cell[c], F.sut.lcd[200 * c + 50]);
    host_lcd(200, 0, 200, 100, 90, 1500); /* Companion meanwhile: held, kept */
    host_key(3, 91);
    TEST_ASSERT_EQUAL_UINT64(cell[1], F.sut.lcd[250]);
    TEST_ASSERT_TRUE(dc_settled(&F.dc));
    cell[2] = box_lcd(400, 0, 200, 100, 80); /* a new frame replaces the region's */
    TEST_ASSERT_FALSE(dc_settled(&F.dc));
    pump();
    TEST_ASSERT_EQUAL_UINT64(cell[2], F.sut.lcd[450]);
    dc_set_hold(&F.dc, false);
    pump();
    assert_same(); /* every key and every strip region Companion's latest */
    fx_done();
}

/* on demand over Companion's deck: the box's brightness goes to the deck (between Companion's
 * image sequences); Companion's brightness meanwhile is kept, not written; when on demand closes,
 * Companion's last brightness comes back. Without one known, the deck keeps the box's. */
static void test_hold_all_brightness(void)
{
    fx_init(0x00BA);
    uint8_t b[32];
    fill_all_keys(10);
    TEST_ASSERT_FALSE(dc_box_brightness(&F.dc, 40)); /* no on demand: Companion's */
    size_t n = deckproto_feat_brightness(F.m, 80, b);
    send_feat(b, n); /* Companion's 80 */
    TEST_ASSERT_EQUAL(80, F.sut.brightness);
    host_key_part(2, 77, 2500, 0, 1); /* an image being passed on when on demand opens: it goes on to its end */
    dc_set_hold(&F.dc, true);
    dc_set_hold_all(&F.dc, true);
    TEST_ASSERT_TRUE(dc_box_brightness(&F.dc, 50));
    TEST_ASSERT_FALSE(dc_settled(&F.dc));
    pump(); /* the box waits for it */
    TEST_ASSERT_EQUAL(80, F.sut.brightness);
    host_key_part(2, 77, 2500, 1, -1);
    pump();
    TEST_ASSERT_EQUAL(50, F.sut.brightness);
    TEST_ASSERT_EQUAL(0, F.sut.interleaved);
    TEST_ASSERT_TRUE(dc_box_brightness(&F.dc, 60)); /* Brighter */
    pump();
    TEST_ASSERT_EQUAL(60, F.sut.brightness);
    n = deckproto_feat_brightness(F.m, 90, b);
    TEST_ASSERT_TRUE(dc_feature(&F.dc, &F.io, b, n)); /* Companion's 90 meanwhile: kept, not written */
    pump();
    TEST_ASSERT_EQUAL(60, F.sut.brightness);
    dc_set_hold_all(&F.dc, false); /* on demand closed */
    dc_set_hold(&F.dc, false);
    pump();
    TEST_ASSERT_EQUAL(90, F.sut.brightness); /* Companion's last back */
    fx_done();
    /* none known from Companion: the deck keeps the box's */
    fx_init(0x00BA);
    dc_set_hold(&F.dc, true);
    dc_set_hold_all(&F.dc, true);
    TEST_ASSERT_TRUE(dc_box_brightness(&F.dc, 30));
    pump();
    TEST_ASSERT_EQUAL(30, F.sut.brightness);
    const int writes = F.sut.feat_writes;
    dc_set_hold_all(&F.dc, false);
    dc_set_hold(&F.dc, false);
    pump();
    TEST_ASSERT_EQUAL(30, F.sut.brightness);
    TEST_ASSERT_EQUAL(writes, F.sut.feat_writes);
    fx_done();
    /* a hold let go and taken again before Companion's brightness went back (a hand-over during on
     * demand): it stays the box's; and a new connection (dc_reset) knows no Companion brightness */
    fx_init(0x00BA);
    n = deckproto_feat_brightness(F.m, 80, b);
    send_feat(b, n);
    dc_set_hold(&F.dc, true);
    dc_set_hold_all(&F.dc, true);
    TEST_ASSERT_TRUE(dc_box_brightness(&F.dc, 50));
    pump();
    dc_set_hold_all(&F.dc, false); /* reset_cache's let go ... */
    dc_set_hold_all(&F.dc, true);  /* ... and the screen still covers it */
    pump();
    TEST_ASSERT_EQUAL(50, F.sut.brightness); /* Companion's 80 never lands over the box's screen */
    dc_reset(&F.dc);
    dc_set_hold_all(&F.dc, false);
    dc_set_hold(&F.dc, false);
    const int w2 = F.sut.feat_writes;
    pump();
    TEST_ASSERT_EQUAL(w2, F.sut.feat_writes); /* nothing known of the old connection: nothing written back */
    fx_done();
}

/* The info bar the same (Neo); on demand closing while a message holds the keys: the info bar comes
 * back, the keys keep the message. */
static void test_hold_all_info_and_back_to_message(void)
{
    fx_init(0x009A);
    uint64_t ids[DC_KEYS_MAX];
    fill_all_keys(10);
    host_info(60);
    dc_set_hold(&F.dc, true);
    dc_set_hold_all(&F.dc, true);
    TEST_ASSERT_FALSE(dc_box_lcd(&F.dc, 0, 0, 10, 10, imgbuf, 100)); /* no strip */
    const uint64_t info = box_info(5);
    box_all(ids, 8);
    pump();
    TEST_ASSERT_EQUAL_UINT64(info, F.sut.info);
    host_info(61); /* held */
    pump();
    TEST_ASSERT_EQUAL_UINT64(info, F.sut.info);
    dc_set_hold_all(&F.dc, false); /* on demand closed, the message still on */
    box_all(ids, 9);
    pump();
    TEST_ASSERT_EQUAL_UINT64(F.ref.info, F.sut.info);
    assert_message_shown(ids);
    host_info(62); /* passes again */
    TEST_ASSERT_EQUAL_UINT64(F.ref.info, F.sut.info);
    TEST_ASSERT_FALSE(dc_box_info(&F.dc, imgbuf, 100));
    dc_set_hold(&F.dc, false);
    pump();
    assert_same();
    fx_done();
}

/* The bounds (the report quotes them). */
/* dc_key_image: Companion's last state of a key, for the box's dimmed "Reconnecting" once it lets go: a complete
 * image's bytes (gen2 exactly; gen1 with its last report's zero padding), a fill's colour (a panel clear: black), or
 * nothing (never drawn, the logo, out too small); a sequence still open never counts */
static void test_key_image(void)
{
    static uint8_t out[16384];
    size_t len = 0;
    uint8_t rgb[3] = {0};
    fx_init(0x006C); /* XL, gen2 JPEG */
    TEST_ASSERT_EQUAL(DC_NONE, dc_key_image(&F.dc, 3, out, sizeof out, &len, rgb));
    TEST_ASSERT_EQUAL_size_t(0, dc_key_image_cap(&F.dc, 3));
    host_key_part(3, 41, 2500, 0, -1);
    TEST_ASSERT_TRUE(dc_key_image_cap(&F.dc, 3) >= 2500);
    TEST_ASSERT_EQUAL(DC_IMAGE, dc_key_image(&F.dc, 3, out, sizeof out, &len, rgb));
    make_img(41, 2500);
    TEST_ASSERT_EQUAL_size_t(2500, len);
    TEST_ASSERT_EQUAL_MEMORY(imgbuf, out, 2500);
    TEST_ASSERT_EQUAL(DC_NONE, dc_key_image(&F.dc, 3, out, 2499, &len, rgb)); /* too small */
    host_key_part(3, 42, 3000, 0, 1);                                       /* a newer one, not finished */
    TEST_ASSERT_EQUAL(DC_IMAGE, dc_key_image(&F.dc, 3, out, sizeof out, &len, rgb));
    make_img(41, 2500);
    TEST_ASSERT_EQUAL_MEMORY(imgbuf, out, 2500); /* still the complete one */
    host_fill(5, 10, 20, 30);
    TEST_ASSERT_EQUAL(DC_FILL, dc_key_image(&F.dc, 5, out, sizeof out, &len, rgb));
    TEST_ASSERT_EQUAL_UINT8(10, rgb[0]);
    TEST_ASSERT_EQUAL_UINT8(20, rgb[1]);
    TEST_ASSERT_EQUAL_UINT8(30, rgb[2]);
    TEST_ASSERT_EQUAL(DC_NONE, dc_key_image(&F.dc, 200, out, sizeof out, &len, rgb)); /* no such key */
    host_simple(0x05, 0); /* clear: every key black */
    TEST_ASSERT_EQUAL(DC_FILL, dc_key_image(&F.dc, 3, out, sizeof out, &len, rgb));
    TEST_ASSERT_EQUAL_UINT8(0, rgb[0] | rgb[1] | rgb[2]);
    host_simple(0x02, 0); /* the logo: no image of the key's own kept */
    TEST_ASSERT_EQUAL(DC_NONE, dc_key_image(&F.dc, 3, out, sizeof out, &len, rgb));
    fx_done();

    fx_init(0x0063); /* Mini, gen1 BMP: the payloads keep their padding */
    host_key_part(2, 77, 1500, 0, -1);
    TEST_ASSERT_EQUAL(DC_IMAGE, dc_key_image(&F.dc, 2, out, sizeof out, &len, rgb));
    TEST_ASSERT_TRUE(len >= 1500);
    make_img(77, 1500);
    TEST_ASSERT_EQUAL_MEMORY(imgbuf, out, 1500);
    for (size_t i = 1500; i < len; i++) TEST_ASSERT_EQUAL_HEX8(0, out[i]);
    fx_done();
}

static void test_bounds(void)
{
    const deck_model_t *m32 = deck_model_find(0x0FD9, 0x00BA), *orig = deck_model_find(0x0FD9, 0x0060);
    const deck_model_t *plus = deck_model_find(0x0FD9, 0x0084);
    const deck_img_target_t key = {.kind = DECK_TGT_KEY};
    TEST_ASSERT_EQUAL_size_t(29, dc_target_cap(m32, &key)); /* (96*96*3 + 1024) / 1016, up */
    TEST_ASSERT_EQUAL_size_t(3, dc_target_cap(orig, &key)); /* (72*72*3 + 1024) / 8175, up */
    const deck_img_target_t strip = {.kind = DECK_TGT_LCD, .w = 800, .h = 100};
    const deck_img_target_t huge = {.kind = DECK_TGT_LCD, .w = 801, .h = 100};
    TEST_ASSERT_EQUAL_size_t(240, dc_target_cap(plus, &strip)); /* (240000 + 1024) / 1008, up */
    TEST_ASSERT_EQUAL_size_t(0, dc_target_cap(plus, &huge));
    /* 32 keys + 32 frames at 29 reports, 4 sequences at 29: (32 + 32 + 4) * 29 * 1024 */
    TEST_ASSERT_EQUAL_size_t((size_t)(32 + 32 + 4) * 29 * 1024, dc_bound_bytes(m32));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_passthrough_unchanged);
    RUN_TEST(test_key_image);
    RUN_TEST(test_message_then_replay_gives_latest);
    RUN_TEST(test_open_sequence_finishes_before_frames);
    RUN_TEST(test_held_sequence_finishing_after_hold);
    RUN_TEST(test_incomplete_dropped);
    RUN_TEST(test_clear_held_replayed_in_order);
    RUN_TEST(test_reset_held_replayed);
    RUN_TEST(test_strip_and_info_pass_during_hold);
    RUN_TEST(test_replay_all_after_quarantine);
    RUN_TEST(test_replay_all_during_hold);
    RUN_TEST(test_too_big_is_flushed_not_lost);
    RUN_TEST(test_no_memory_passes_on);
    RUN_TEST(test_uncached_key_after_message);
    RUN_TEST(test_gen1_hold_and_replay);
    RUN_TEST(test_write_failure_resumes);
    RUN_TEST(test_handover_after_failed_write);
    RUN_TEST(test_handover_drops_sequences);
    RUN_TEST(test_orphan_passed_on_in_hold);
    RUN_TEST(test_uncached_region_keeps_others);
    RUN_TEST(test_held_clear_strip_and_info);
    RUN_TEST(test_passed_clear_then_replay_all);
    RUN_TEST(test_frames_replace_and_need_hold);
    RUN_TEST(test_short_hold);
    RUN_TEST(test_hold_all_strip);
    RUN_TEST(test_hold_all_info_and_back_to_message);
    RUN_TEST(test_hold_all_brightness);
    RUN_TEST(test_bounds);
    return UNITY_END();
}
