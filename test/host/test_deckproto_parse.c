/* deckproto_parse_image_report: build -> parse round trips on the builders' real output, for every
 * image output in the model table, plus reports that are not images. */
#include <string.h>
#include "unity.h"
#include "deckproto/deckproto.h"

void setUp(void) {}
void tearDown(void) {}

static const deck_model_t *M(uint16_t pid)
{
    const deck_model_t *m = deck_model_find(0x0FD9, pid);
    TEST_ASSERT_NOT_NULL(m);
    return m;
}

static uint8_t img[40000];
static uint8_t rep[8191];

static void fill_img(size_t n)
{
    for (size_t i = 0; i < n; i++) img[i] = (uint8_t)(i * 13 + 5);
}

typedef struct {
    deck_tgt_kind_t kind;
    uint8_t key;
    uint16_t x, y, w, h;
} want_t;

/* Parses every report the writer makes. Checks the target, the chunk index (counting from
 * deckproto_image_first_chunk), the last flag (only on the final report) and the payload: for gen2
 * exactly the builder's bytes, so the payloads put together are the image; for gen1 the builder's
 * bytes (chunk[i] of them) followed by zeros up to the report's end. Returns the report count. */
static int round_trip(const deck_model_t *m, deck_img_writer_t *w, const want_t *want, size_t len,
                      const size_t *gen1_chunk)
{
    size_t off = 0;
    int n = 0;
    const uint16_t first = deckproto_image_first_chunk(m);
    bool saw_last = false;
    while (deckproto_img_next(w, rep)) {
        TEST_ASSERT_FALSE_MESSAGE(saw_last, "a report after the last one");
        deck_img_target_t t;
        uint16_t chunk = 0xFFFF;
        bool last = false;
        const uint8_t *pl = NULL;
        size_t pl_len = 0;
        TEST_ASSERT_TRUE(deckproto_parse_image_report(m, rep, m->report_len, &t, &chunk, &last, &pl, &pl_len));
        TEST_ASSERT_EQUAL_INT(want->kind, t.kind);
        if (want->kind == DECK_TGT_KEY) TEST_ASSERT_EQUAL_UINT8(want->key, t.key);
        if (want->kind == DECK_TGT_LCD) {
            TEST_ASSERT_EQUAL_UINT16(want->x, t.x);
            TEST_ASSERT_EQUAL_UINT16(want->y, t.y);
            TEST_ASSERT_EQUAL_UINT16(want->w, t.w);
            TEST_ASSERT_EQUAL_UINT16(want->h, t.h);
        }
        TEST_ASSERT_EQUAL_UINT16(first + n, chunk);
        TEST_ASSERT_TRUE(pl >= rep && pl + pl_len <= rep + m->report_len);
        if (m->gen == DECK_GEN2) {
            TEST_ASSERT_TRUE(off + pl_len <= len);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(img + off, pl, pl_len);
            off += pl_len;
        } else {
            const size_t k = gen1_chunk[n];
            TEST_ASSERT_EQUAL_size_t((size_t)m->report_len - 16, pl_len);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(img + off, pl, k);
            for (size_t i = k; i < pl_len; i++) TEST_ASSERT_EQUAL_UINT8(0, pl[i]);
            off += k;
        }
        saw_last = last;
        n++;
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_last, "the final report carries the last flag");
    TEST_ASSERT_EQUAL_size_t(len, off);
    return n;
}

/* gen2 key image on the XL: two chunks. */
static void test_gen2_xl_key_two_chunks(void)
{
    const deck_model_t *m = M(0x006C);
    fill_img(1500);
    deck_img_writer_t w;
    deckproto_img_key(&w, m, 31, img, 1500);
    const want_t want = {.kind = DECK_TGT_KEY, .key = 31};
    TEST_ASSERT_EQUAL_INT(2, round_trip(m, &w, &want, 1500, NULL));
}

/* gen2 key images: Module 32 (the deck on the bench), 15-key, Neo, Plus, Plus XL. */
static void test_gen2_keys_every_model(void)
{
    const uint16_t pids[] = {0x00BA, 0x0080, 0x009A, 0x0084, 0x00C6};
    for (size_t i = 0; i < sizeof pids / sizeof pids[0]; i++) {
        const deck_model_t *m = M(pids[i]);
        const uint8_t key = (uint8_t)(m->cols * m->rows - 1);
        fill_img(3000);
        deck_img_writer_t w;
        deckproto_img_key(&w, m, key, img, 3000);
        const want_t want = {.kind = DECK_TGT_KEY, .key = key};
        TEST_ASSERT_EQUAL_INT(3, round_trip(m, &w, &want, 3000, NULL));
    }
}

/* gen1 Mini family: 1008 bytes per report, the last one zero padded. */
static void test_gen1_mini_key(void)
{
    const uint16_t pids[] = {0x0063, 0x0090, 0x00B3, 0x00B8};
    for (size_t i = 0; i < sizeof pids / sizeof pids[0]; i++) {
        const deck_model_t *m = M(pids[i]);
        fill_img(19254);
        deck_img_writer_t w;
        deckproto_img_key(&w, m, 4, img, 19254);
        size_t chunk[20];
        for (int k = 0; k < 19; k++) chunk[k] = 1008;
        chunk[19] = 19254 - 19 * 1008;
        const want_t want = {.kind = DECK_TGT_KEY, .key = 4};
        TEST_ASSERT_EQUAL_INT(20, round_trip(m, &w, &want, 19254, chunk));
    }
}

/* gen1 Original v1: two 8191-byte pages, numbered 1 and 2, split at the half. */
static void test_gen1_original_key(void)
{
    const deck_model_t *m = M(0x0060);
    TEST_ASSERT_EQUAL_UINT16(1, deckproto_image_first_chunk(m));
    fill_img(15606);
    deck_img_writer_t w;
    deckproto_img_key(&w, m, 14, img, 15606);
    const size_t chunk[2] = {7803, 7803};
    const want_t want = {.kind = DECK_TGT_KEY, .key = 14};
    TEST_ASSERT_EQUAL_INT(2, round_trip(m, &w, &want, 15606, chunk));
}

/* Plus LCD strip region: 1008 bytes per report. */
static void test_plus_lcd_region(void)
{
    const deck_model_t *m = M(0x0084);
    fill_img(2100);
    deck_img_writer_t w;
    deckproto_img_lcd(&w, m, 200, 0, 200, 100, img, 2100);
    const want_t want = {.kind = DECK_TGT_LCD, .x = 200, .y = 0, .w = 200, .h = 100};
    TEST_ASSERT_EQUAL_INT(3, round_trip(m, &w, &want, 2100, NULL));
}

/* Plus XL strip region: the header carries the region unturned (only the JPEG is turned). */
static void test_plus_xl_lcd_region(void)
{
    const deck_model_t *m = M(0x00C6);
    fill_img(1009);
    deck_img_writer_t w;
    deckproto_img_lcd(&w, m, 1000, 0, 200, 100, img, 1009);
    const want_t want = {.kind = DECK_TGT_LCD, .x = 1000, .y = 0, .w = 200, .h = 100};
    TEST_ASSERT_EQUAL_INT(2, round_trip(m, &w, &want, 1009, NULL));
}

/* Neo info bar. */
static void test_neo_info_bar(void)
{
    const deck_model_t *m = M(0x009A);
    fill_img(2000);
    deck_img_writer_t w;
    deckproto_img_info(&w, m, img, 2000);
    const want_t want = {.kind = DECK_TGT_INFO};
    TEST_ASSERT_EQUAL_INT(2, round_trip(m, &w, &want, 2000, NULL));
}

/* A report the size the host library sends (a whole report) or shorter is fine; the header decides. */
static void test_short_report_with_whole_payload(void)
{
    const deck_model_t *m = M(0x00BA);
    fill_img(100);
    deck_img_writer_t w;
    deckproto_img_key(&w, m, 3, img, 100);
    TEST_ASSERT_TRUE(deckproto_img_next(&w, rep));
    deck_img_target_t t;
    uint16_t chunk;
    bool last;
    const uint8_t *pl;
    size_t n;
    TEST_ASSERT_TRUE(deckproto_parse_image_report(m, rep, 8 + 100, &t, &chunk, &last, &pl, &n));
    TEST_ASSERT_EQUAL_size_t(100, n);
    TEST_ASSERT_TRUE(last);
    TEST_ASSERT_FALSE(deckproto_parse_image_report(m, rep, 8 + 99, &t, &chunk, &last, &pl, &n)); /* cut */
}

static bool parses(const deck_model_t *m, const uint8_t *r, size_t len)
{
    deck_img_target_t t;
    uint16_t chunk;
    bool last;
    const uint8_t *pl;
    size_t n;
    return deckproto_parse_image_report(m, r, len, &t, &chunk, &last, &pl, &n);
}

static void test_not_image_reports(void)
{
    const deck_model_t *xl = M(0x006C), *plus = M(0x0084), *neo = M(0x009A), *mini = M(0x0063);
    uint8_t r[1024] = {0};

    /* feature-style and input reports */
    r[0] = 0x03; r[1] = 0x08; r[2] = 50;
    TEST_ASSERT_FALSE(parses(xl, r, 32));
    memset(r, 0, sizeof r);
    r[0] = 0x01;
    TEST_ASSERT_FALSE(parses(xl, r, 512));
    TEST_ASSERT_FALSE(parses(xl, NULL, 1024));
    TEST_ASSERT_FALSE(parses(NULL, r, 1024));
    TEST_ASSERT_FALSE(parses(xl, r, 0));

    /* gen2 key header: key out of range, header cut, payload longer than the report */
    memset(r, 0, sizeof r);
    r[0] = 0x02; r[1] = 0x07; r[2] = 32; r[3] = 1; r[4] = 10;
    TEST_ASSERT_FALSE(parses(xl, r, 1024));
    r[2] = 31;
    TEST_ASSERT_TRUE(parses(xl, r, 1024));
    TEST_ASSERT_FALSE(parses(xl, r, 7));
    r[4] = 0xF9; r[5] = 0x03; /* 1017 > 1024 - 8 */
    TEST_ASSERT_FALSE(parses(xl, r, 1024));
    /* unknown gen2 command */
    r[1] = 0x09; r[4] = 10; r[5] = 0;
    TEST_ASSERT_FALSE(parses(xl, r, 1024));

    /* an LCD region to a deck without a strip; an info bar to a deck without one */
    memset(r, 0, sizeof r);
    r[0] = 0x02; r[1] = 0x0C; r[6] = 200; r[8] = 100; r[10] = 1; r[13] = 10;
    TEST_ASSERT_TRUE(parses(plus, r, 1024));
    TEST_ASSERT_FALSE(parses(xl, r, 1024));
    TEST_ASSERT_FALSE(parses(neo, r, 1024));
    TEST_ASSERT_FALSE(parses(plus, r, 15));
    memset(r, 0, sizeof r);
    r[0] = 0x02; r[1] = 0x0B; r[3] = 1; r[4] = 10;
    TEST_ASSERT_TRUE(parses(neo, r, 1024));
    TEST_ASSERT_FALSE(parses(plus, r, 1024));

    /* gen1: key 0 (byte 5 = key + 1) and keys past the model; a gen2 header on gen1 and back */
    memset(r, 0, sizeof r);
    r[0] = 0x02; r[1] = 0x01; r[4] = 1; r[5] = 0;
    TEST_ASSERT_FALSE(parses(mini, r, 1024));
    r[5] = 7;
    TEST_ASSERT_FALSE(parses(mini, r, 1024));
    r[5] = 6;
    TEST_ASSERT_TRUE(parses(mini, r, 1024));
    TEST_ASSERT_FALSE(parses(mini, r, 16)); /* header only: no image bytes */
    TEST_ASSERT_FALSE(parses(xl, r, 1024));
    r[1] = 0x07;
    TEST_ASSERT_FALSE(parses(mini, r, 1024));

    /* the Pedal has no display */
    memset(r, 0, sizeof r);
    r[0] = 0x02; r[1] = 0x07; r[3] = 1; r[4] = 10;
    TEST_ASSERT_FALSE(parses(M(0x0086), r, 1024));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gen2_xl_key_two_chunks);
    RUN_TEST(test_gen2_keys_every_model);
    RUN_TEST(test_gen1_mini_key);
    RUN_TEST(test_gen1_original_key);
    RUN_TEST(test_plus_lcd_region);
    RUN_TEST(test_plus_xl_lcd_region);
    RUN_TEST(test_neo_info_bar);
    RUN_TEST(test_short_report_with_whole_payload);
    RUN_TEST(test_not_image_reports);
    return UNITY_END();
}
