#include <string.h>
#include "unity.h"
#include "deckproto/deckproto.h"

void setUp(void) {}
void tearDown(void) {}

static const deck_model_t *M(uint16_t pid) {
    const deck_model_t *m = deck_model_find(0x0FD9, pid);
    TEST_ASSERT_NOT_NULL(m);
    return m;
}

static uint8_t img[20000];
static uint8_t rep[8191];

static void fill_img(size_t n) {
    for (size_t i = 0; i < n; i++) img[i] = (uint8_t)(i * 7 + 1);
}

static void test_gen2_key_chunks(void) {
    const deck_model_t *m = M(0x006C);
    fill_img(2500);
    deck_img_writer_t w;
    deckproto_img_key(&w, m, 7, img, 2500);
    static const uint8_t h0[] = {0x02, 0x07, 0x07, 0x00, 0xF8, 0x03, 0x00, 0x00};
    static const uint8_t h2[] = {0x02, 0x07, 0x07, 0x01, 0xD4, 0x01, 0x02, 0x00};
    int count = 0;
    while (deckproto_img_next(&w, rep)) {
        if (count == 0) TEST_ASSERT_EQUAL_UINT8_ARRAY(h0, rep, 8);
        if (count == 0) TEST_ASSERT_EQUAL_UINT8_ARRAY(img, rep + 8, 1016);
        if (count == 2) {
            TEST_ASSERT_EQUAL_UINT8_ARRAY(h2, rep, 8);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(img + 2032, rep + 8, 468);
            for (int i = 8 + 468; i < 1024; i++) TEST_ASSERT_EQUAL_UINT8(0, rep[i]);
        }
        count++;
    }
    TEST_ASSERT_EQUAL_INT(3, count);
    TEST_ASSERT_FALSE(deckproto_img_next(&w, rep));
}

static void test_gen2_exact_fit(void) {
    fill_img(1016);
    deck_img_writer_t w;
    deckproto_img_key(&w, M(0x006C), 0, img, 1016);
    TEST_ASSERT_TRUE(deckproto_img_next(&w, rep));
    TEST_ASSERT_EQUAL_UINT8(1, rep[3]);
    TEST_ASSERT_EQUAL_UINT8(0xF8, rep[4]);
    TEST_ASSERT_EQUAL_UINT8(0x03, rep[5]);
    TEST_ASSERT_FALSE(deckproto_img_next(&w, rep));
}

static void test_zero_length(void) {
    deck_img_writer_t w;
    deckproto_img_key(&w, M(0x006C), 0, img, 0);
    TEST_ASSERT_FALSE(deckproto_img_next(&w, rep));
    deckproto_img_key(&w, M(0x0063), 0, img, 0);
    TEST_ASSERT_FALSE(deckproto_img_next(&w, rep));
    deckproto_img_key(&w, M(0x0060), 0, img, 0);
    TEST_ASSERT_FALSE(deckproto_img_next(&w, rep));
}

static void test_mini_chunks(void) {
    fill_img(19254);
    deck_img_writer_t w;
    deckproto_img_key(&w, M(0x0063), 2, img, 19254);
    int count = 0;
    while (deckproto_img_next(&w, rep)) {
        static const uint8_t h0[] = {0x02, 0x01, 0x00, 0x00, 0x00, 0x03};
        TEST_ASSERT_EQUAL_UINT8(count, rep[2]);
        if (count == 0) TEST_ASSERT_EQUAL_UINT8_ARRAY(h0, rep, 6);
        TEST_ASSERT_EQUAL_UINT8(count == 19 ? 1 : 0, rep[4]);
        if (count == 19) {
            TEST_ASSERT_EQUAL_UINT8_ARRAY(img + 19 * 1008, rep + 16, 19254 - 19 * 1008);
            TEST_ASSERT_EQUAL_UINT8(0, rep[16 + 19254 - 19 * 1008]);
        }
        count++;
    }
    TEST_ASSERT_EQUAL_INT(20, count);
}

static void test_orig_v1_two_halves(void) {
    fill_img(15606);
    deck_img_writer_t w;
    deckproto_img_key(&w, M(0x0060), 0, img, 15606);
    TEST_ASSERT_TRUE(deckproto_img_next(&w, rep));
    TEST_ASSERT_EQUAL_UINT8(1, rep[2]);
    TEST_ASSERT_EQUAL_UINT8(0, rep[4]);
    TEST_ASSERT_EQUAL_UINT8(1, rep[5]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(img, rep + 16, 7803);
    TEST_ASSERT_EQUAL_UINT8(0, rep[16 + 7803]);
    TEST_ASSERT_TRUE(deckproto_img_next(&w, rep));
    TEST_ASSERT_EQUAL_UINT8(2, rep[2]);
    TEST_ASSERT_EQUAL_UINT8(1, rep[4]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(img + 7803, rep + 16, 7803);
    TEST_ASSERT_EQUAL_UINT8(0, rep[16 + 7803]);
    TEST_ASSERT_FALSE(deckproto_img_next(&w, rep));
}

static void test_plus_lcd_header(void) {
    fill_img(500);
    deck_img_writer_t w;
    deckproto_img_lcd(&w, M(0x0084), 200, 0, 200, 100, img, 500);
    TEST_ASSERT_TRUE(deckproto_img_next(&w, rep));
    static const uint8_t h[] = {0x02, 0x0C, 0xC8, 0, 0, 0, 0xC8, 0, 0x64, 0, 0x01, 0, 0, 0xF4, 0x01, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(h, rep, 16);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(img, rep + 16, 500);
    TEST_ASSERT_FALSE(deckproto_img_next(&w, rep));
}

static void test_neo_info_header(void) {
    fill_img(2000);
    deck_img_writer_t w;
    deckproto_img_info(&w, M(0x009A), img, 2000);
    TEST_ASSERT_TRUE(deckproto_img_next(&w, rep));
    static const uint8_t h[] = {0x02, 0x0B, 0x00, 0x00, 0xF8, 0x03, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(h, rep, 8);
    TEST_ASSERT_TRUE(deckproto_img_next(&w, rep));
    TEST_ASSERT_EQUAL_UINT8(1, rep[3]);
    TEST_ASSERT_EQUAL_UINT8(1, rep[6]);
    TEST_ASSERT_FALSE(deckproto_img_next(&w, rep));
}

static void test_feature_reports(void) {
    uint8_t b[32];
    memset(b, 0xEE, sizeof b);
    TEST_ASSERT_EQUAL_UINT(32, deckproto_feat_brightness(M(0x006C), 55, b));
    TEST_ASSERT_EQUAL_HEX8(0x03, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x08, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0x37, b[2]);
    for (int i = 3; i < 32; i++) TEST_ASSERT_EQUAL_UINT8(0, b[i]);
    memset(b, 0xEE, sizeof b);
    TEST_ASSERT_EQUAL_UINT(17, deckproto_feat_brightness(M(0x0063), 55, b));
    static const uint8_t e[] = {0x05, 0x55, 0xAA, 0xD1, 0x01, 0x37};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(e, b, 6);
    for (int i = 6; i < 17; i++) TEST_ASSERT_EQUAL_UINT8(0, b[i]);
    TEST_ASSERT_EQUAL_UINT(32, deckproto_feat_fill_key(M(0x009A), 9, 0xFF, 0, 0, b));
    static const uint8_t f[] = {0x03, 0x06, 0x09, 0xFF, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(f, b, 6);
    TEST_ASSERT_EQUAL_UINT(32, deckproto_feat_reset(M(0x006C), b));
    TEST_ASSERT_EQUAL_HEX8(0x02, b[1]);
    TEST_ASSERT_EQUAL_UINT(17, deckproto_feat_reset(M(0x0063), b));
    TEST_ASSERT_EQUAL_HEX8(0x0B, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x63, b[1]);
    TEST_ASSERT_EQUAL_UINT(0, deckproto_feat_fill_key(M(0x0063), 1, 1, 2, 3, b));
}

/* Companion's brightness report read back (gen2 03 08 pct; gen1 05 55 AA D1 01 pct); anything else -1. */
static void test_brightness_of(void) {
    uint8_t b[32];
    deckproto_feat_brightness(M(0x006C), 55, b);
    TEST_ASSERT_EQUAL_INT(55, deckproto_feat_brightness_of(M(0x006C), b, 32));
    deckproto_feat_brightness(M(0x0063), 30, b);
    TEST_ASSERT_EQUAL_INT(30, deckproto_feat_brightness_of(M(0x0063), b, 17));
    TEST_ASSERT_EQUAL_INT(-1, deckproto_feat_brightness_of(M(0x0063), b, 5)); /* too short */
    deckproto_feat_fill_key(M(0x006C), 3, 1, 2, 3, b);
    TEST_ASSERT_EQUAL_INT(-1, deckproto_feat_brightness_of(M(0x006C), b, 32));
    deckproto_feat_reset(M(0x006C), b);
    TEST_ASSERT_EQUAL_INT(-1, deckproto_feat_brightness_of(M(0x006C), b, 32));
    const uint8_t big[] = {0x03, 0x08, 200};
    TEST_ASSERT_EQUAL_INT(100, deckproto_feat_brightness_of(M(0x006C), big, 3)); /* as the deck takes it */
}

static void test_report_ids(void) {
    TEST_ASSERT_EQUAL_HEX8(0x06, deckproto_serial_report_id(M(0x006C)));
    TEST_ASSERT_EQUAL_HEX8(0x03, deckproto_serial_report_id(M(0x0063)));
    TEST_ASSERT_EQUAL_HEX8(0x05, deckproto_fw_report_id(M(0x006C)));
    TEST_ASSERT_EQUAL_HEX8(0xA1, deckproto_fw_report_id(M(0x0063)));
    TEST_ASSERT_EQUAL_HEX8(0x04, deckproto_fw_report_id(M(0x0060)));
}

static void test_parse_serial(void) {
    char out[32];
    uint8_t r2[32] = {0x06, 0x0C};
    memcpy(r2 + 2, "AB12CD34EF56", 12);
    TEST_ASSERT_TRUE(deckproto_parse_serial(M(0x006C), r2, sizeof r2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("AB12CD34EF56", out);
    uint8_t r1[32] = {0x03};
    memcpy(r1 + 5, "A00123456", 9);
    TEST_ASSERT_TRUE(deckproto_parse_serial(M(0x0063), r1, sizeof r1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("A00123456", out);
    TEST_ASSERT_FALSE(deckproto_parse_serial(M(0x006C), r2, sizeof r2, out, 5));
    TEST_ASSERT_FALSE(deckproto_parse_serial(M(0x006C), r2, 1, out, sizeof out));
}

static void test_parse_fw(void) {
    char out[32];
    uint8_t r2[32] = {0x05, 0x0C, 1, 2, 3, 4};
    memcpy(r2 + 6, "1.00.123", 8);
    TEST_ASSERT_TRUE(deckproto_parse_fw(M(0x006C), r2, sizeof r2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("1.00.123", out);
    uint8_t r1[32] = {0xA1};
    memcpy(r1 + 5, "1.0.17.000", 10);
    TEST_ASSERT_TRUE(deckproto_parse_fw(M(0x0063), r1, sizeof r1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("1.0.17.000", out);
}

static void test_input_gen2_keys_edges(void) {
    const deck_model_t *m = M(0x006C);
    deck_input_state_t st = {0};
    deck_event_t ev[8];
    uint8_t r[4 + 32 + 4] = {0x01, 0x00, 32, 0};
    r[4 + 3] = 1;
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(m, &st, r, sizeof r, ev, 8));
    TEST_ASSERT_EQUAL_INT(DECK_EV_KEY, ev[0].kind);
    TEST_ASSERT_EQUAL_UINT8(3, ev[0].index);
    TEST_ASSERT_TRUE(ev[0].pressed);
    TEST_ASSERT_EQUAL_INT(0, deckproto_parse_input(m, &st, r, sizeof r, ev, 8));
    r[4 + 3] = 0;
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(m, &st, r, sizeof r, ev, 8));
    TEST_ASSERT_FALSE(ev[0].pressed);
    TEST_ASSERT_EQUAL_UINT8(3, ev[0].index);
    TEST_ASSERT_EQUAL_INT(0, deckproto_parse_input(m, &st, r, 10, ev, 8)); /* truncated */
}

static void test_input_max_too_small_keeps_edges(void) {
    const deck_model_t *m = M(0x006C);
    deck_input_state_t st = {0};
    deck_event_t ev[8];
    uint8_t r[4 + 32] = {0x01, 0x00, 32, 0};
    r[4 + 1] = r[4 + 2] = r[4 + 3] = 1;
    TEST_ASSERT_EQUAL_INT(2, deckproto_parse_input(m, &st, r, sizeof r, ev, 2));
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].index);
    TEST_ASSERT_EQUAL_UINT8(2, ev[1].index);
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(m, &st, r, sizeof r, ev, 8));
    TEST_ASSERT_EQUAL_UINT8(3, ev[0].index);
    TEST_ASSERT_EQUAL_INT(0, deckproto_parse_input(m, &st, r, sizeof r, ev, 8));
}

static void test_input_neo_touch_keys(void) {
    deck_input_state_t st = {0};
    deck_event_t ev[4];
    uint8_t r[4 + 10] = {0x01, 0x00, 10, 0};
    r[4 + 9] = 1;
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(M(0x009A), &st, r, sizeof r, ev, 4));
    TEST_ASSERT_EQUAL_UINT8(9, ev[0].index);
}

static void test_input_plus_encoder_rotate_and_press(void) {
    const deck_model_t *m = M(0x0084);
    deck_input_state_t st = {0};
    deck_event_t ev[8];
    const uint8_t rot[] = {0x01, 0x03, 0x05, 0x00, 0x01, 0xFE, 0x00, 0x03, 0x00};
    TEST_ASSERT_EQUAL_INT(2, deckproto_parse_input(m, &st, rot, sizeof rot, ev, 8));
    TEST_ASSERT_EQUAL_INT(DECK_EV_ENC_ROTATE, ev[0].kind);
    TEST_ASSERT_EQUAL_UINT8(0, ev[0].index);
    TEST_ASSERT_EQUAL_INT8(-2, ev[0].delta);
    TEST_ASSERT_EQUAL_UINT8(2, ev[1].index);
    TEST_ASSERT_EQUAL_INT8(3, ev[1].delta);
    /* rotation repeats are not edges */
    TEST_ASSERT_EQUAL_INT(2, deckproto_parse_input(m, &st, rot, sizeof rot, ev, 8));
    const uint8_t pr[] = {0x01, 0x03, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(m, &st, pr, sizeof pr, ev, 8));
    TEST_ASSERT_EQUAL_INT(DECK_EV_ENC_PRESS, ev[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].index);
    TEST_ASSERT_TRUE(ev[0].pressed);
    TEST_ASSERT_EQUAL_INT(0, deckproto_parse_input(m, &st, pr, sizeof pr, ev, 8));
    const uint8_t rel[] = {0x01, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(m, &st, rel, sizeof rel, ev, 8));
    TEST_ASSERT_FALSE(ev[0].pressed);
    /* encoder report on a model without encoders is ignored */
    TEST_ASSERT_EQUAL_INT(0, deckproto_parse_input(M(0x006C), &st, rot, sizeof rot, ev, 8));
}

static void test_input_plus_touch(void) {
    const deck_model_t *m = M(0x0084);
    deck_input_state_t st = {0};
    deck_event_t ev[4];
    const uint8_t tap[] = {0x01, 0x02, 0x0A, 0x00, 0x01, 0x00, 0xC2, 0x01, 0x28, 0x00};
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(m, &st, tap, sizeof tap, ev, 4));
    TEST_ASSERT_EQUAL_INT(DECK_EV_TOUCH_TAP, ev[0].kind);
    TEST_ASSERT_EQUAL_UINT16(450, ev[0].x);
    TEST_ASSERT_EQUAL_UINT16(40, ev[0].y);
    const uint8_t lng[] = {0x01, 0x02, 0x0A, 0x00, 0x02, 0x00, 0x64, 0x00, 0x32, 0x00};
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(m, &st, lng, sizeof lng, ev, 4));
    TEST_ASSERT_EQUAL_INT(DECK_EV_TOUCH_LONG, ev[0].kind);
    const uint8_t sw[] = {0x01, 0x02, 0x0E, 0x00, 0x03, 0x00, 0x64, 0x00, 0x32, 0x00, 0x2C, 0x01, 0x14, 0x00};
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(m, &st, sw, sizeof sw, ev, 4));
    TEST_ASSERT_EQUAL_INT(DECK_EV_TOUCH_SWIPE, ev[0].kind);
    TEST_ASSERT_EQUAL_UINT16(100, ev[0].x);
    TEST_ASSERT_EQUAL_UINT16(50, ev[0].y);
    TEST_ASSERT_EQUAL_UINT16(300, ev[0].x2);
    TEST_ASSERT_EQUAL_UINT16(20, ev[0].y2);
    TEST_ASSERT_EQUAL_INT(0, deckproto_parse_input(M(0x006C), &st, tap, sizeof tap, ev, 4));
}

static void test_input_gen1_mini(void) {
    deck_input_state_t st = {0};
    deck_event_t ev[4];
    const uint8_t r[] = {0x01, 0, 0, 1, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(M(0x0063), &st, r, sizeof r, ev, 4));
    TEST_ASSERT_EQUAL_UINT8(2, ev[0].index);
    TEST_ASSERT_TRUE(ev[0].pressed);
}

static void test_input_pedal(void) {
    deck_input_state_t st = {0};
    deck_event_t ev[4];
    const uint8_t r[] = {0x01, 0x00, 0x03, 0x00, 0x00, 0x01, 0x00};
    TEST_ASSERT_EQUAL_INT(1, deckproto_parse_input(M(0x0086), &st, r, sizeof r, ev, 4));
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].index);
    TEST_ASSERT_TRUE(ev[0].pressed);
}

static bool has_id(const uint8_t *ids, size_t n, uint8_t id) {
    for (size_t i = 0; i < n; i++)
        if (ids[i] == id) return true;
    return false;
}

/* What the host library reads from a child (@elgato-stream-deck/core 7.8.0): gen2 and the Pedal
 * 0x05 and 0x06 (properties/gen2.ts:40,53, pedal.ts:20,32); gen1 (Original v1, the Mini family)
 * 0x04 and 0x03 (properties/gen1.ts:38-41, 54-64). Plus the box's own serial and firmware reports. */
static void test_kept_report_ids(void) {
    uint8_t ids[DECK_KEPT_REPORTS_MAX];
    TEST_ASSERT_EQUAL_size_t(2, deckproto_kept_report_ids(M(0x00BA), ids)); /* Module 32 */
    TEST_ASSERT_TRUE(has_id(ids, 2, 0x05) && has_id(ids, 2, 0x06));
    TEST_ASSERT_EQUAL_size_t(2, deckproto_kept_report_ids(M(0x0086), ids)); /* Pedal */
    TEST_ASSERT_TRUE(has_id(ids, 2, 0x05) && has_id(ids, 2, 0x06));
    TEST_ASSERT_EQUAL_size_t(3, deckproto_kept_report_ids(M(0x0063), ids)); /* Mini: 0xA1 is the box's */
    TEST_ASSERT_TRUE(has_id(ids, 3, 0x03) && has_id(ids, 3, 0x04) && has_id(ids, 3, 0xA1));
    TEST_ASSERT_EQUAL_size_t(3, deckproto_kept_report_ids(M(0x00B8), ids)); /* Module 6 */
    TEST_ASSERT_TRUE(has_id(ids, 3, 0x04));
    TEST_ASSERT_EQUAL_size_t(2, deckproto_kept_report_ids(M(0x0060), ids)); /* Original v1 */
    TEST_ASSERT_TRUE(has_id(ids, 2, 0x03) && has_id(ids, 2, 0x04));

    static const uint16_t pids[] = {0x0060, 0x0063, 0x0090, 0x00B3, 0x00B8, 0x006D, 0x0080, 0x00A5, 0x00B9,
                                    0x00E4, 0x006C, 0x008F, 0x00BA, 0x009A, 0x0084, 0x00C6, 0x0086};
    for (size_t p = 0; p < sizeof pids / sizeof pids[0]; p++) {
        const deck_model_t *m = M(pids[p]);
        memset(ids, 0, sizeof ids);
        const size_t n = deckproto_kept_report_ids(m, ids);
        TEST_ASSERT_TRUE(n >= 2 && n <= DECK_KEPT_REPORTS_MAX);
        TEST_ASSERT_TRUE(has_id(ids, n, deckproto_serial_report_id(m)));
        TEST_ASSERT_TRUE(has_id(ids, n, deckproto_fw_report_id(m)));
        TEST_ASSERT_TRUE(has_id(ids, n, m->gen == DECK_GEN2 ? 0x05 : 0x04)); /* library firmware */
        TEST_ASSERT_TRUE(has_id(ids, n, m->gen == DECK_GEN2 ? 0x06 : 0x03)); /* library serial */
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++) TEST_ASSERT_NOT_EQUAL(ids[i], ids[j]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_gen2_key_chunks);
    RUN_TEST(test_gen2_exact_fit);
    RUN_TEST(test_zero_length);
    RUN_TEST(test_mini_chunks);
    RUN_TEST(test_orig_v1_two_halves);
    RUN_TEST(test_plus_lcd_header);
    RUN_TEST(test_neo_info_header);
    RUN_TEST(test_feature_reports);
    RUN_TEST(test_brightness_of);
    RUN_TEST(test_report_ids);
    RUN_TEST(test_kept_report_ids);
    RUN_TEST(test_parse_serial);
    RUN_TEST(test_parse_fw);
    RUN_TEST(test_input_gen2_keys_edges);
    RUN_TEST(test_input_max_too_small_keeps_edges);
    RUN_TEST(test_input_neo_touch_keys);
    RUN_TEST(test_input_plus_encoder_rotate_and_press);
    RUN_TEST(test_input_plus_touch);
    RUN_TEST(test_input_gen1_mini);
    RUN_TEST(test_input_pedal);
    return UNITY_END();
}
