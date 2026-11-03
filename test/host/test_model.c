#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "deckmodel/model.h"
#include "deckmodel/manifest.h"

#define SURF_TEST_CTLS 64

void setUp(void) {}
void tearDown(void) {}

/* The control of kind with hardware index hw, or NULL (once deckmodel's; only these tests used it). */
static const deck_ctl_t *deck_ctl_by_hw(const deck_ctl_t *list, int n, deck_ctl_kind_t kind, uint8_t hw)
{
    for (int i = 0; i < n; i++)
        if (list[i].kind == kind && list[i].hw == hw) return &list[i];
    return NULL;
}

static const deck_model_t *M(uint16_t pid)
{
    const deck_model_t *m = deck_model_find(0x0FD9, pid);
    TEST_ASSERT_NOT_NULL(m);
    return m;
}

static int ctls(uint16_t pid, deck_ctl_t *out)
{
    int n = deck_model_controls(M(pid), out, 64);
    TEST_ASSERT_TRUE(n >= 0);
    return n;
}

static int count_kind(const deck_ctl_t *l, int n, deck_ctl_kind_t k)
{
    int c = 0;
    for (int i = 0; i < n; i++) c += l[i].kind == k;
    return c;
}

void test_find_known_and_unknown(void)
{
    TEST_ASSERT_EQUAL(8, deck_model_find(0x0FD9, 0x006C)->cols);
    TEST_ASSERT_NULL(deck_model_find(0x0FD9, 0x1234));
    TEST_ASSERT_NULL(deck_model_find(0x1234, 0x006C));
}

void test_xl_controls(void)
{
    deck_ctl_t l[64];
    int n = ctls(0x006C, l);
    TEST_ASSERT_EQUAL(32, n);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(CTL_KEY_LCD, l[i].kind);
        TEST_ASSERT_EQUAL(96, l[i].w);
        TEST_ASSERT_EQUAL(96, l[i].h);
    }
    const deck_ctl_t *c = deck_ctl_find(l, n, "3/7");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(31, c->hw);
    TEST_ASSERT_NULL(deck_ctl_find(l, n, "4/0"));
}

void test_original_v1_rtl(void)
{
    deck_ctl_t l[64];
    int n = ctls(0x0060, l);
    TEST_ASSERT_EQUAL(4, deck_ctl_find(l, n, "0/0")->hw);
    TEST_ASSERT_EQUAL(10, deck_ctl_find(l, n, "2/4")->hw);
}

void test_plus_controls(void)
{
    deck_ctl_t l[64];
    int n = ctls(0x0084, l);
    TEST_ASSERT_EQUAL(16, n);
    TEST_ASSERT_EQUAL(8, count_kind(l, n, CTL_KEY_LCD));
    const char *lcd[] = {"2/0", "2/1", "2/2", "2/3"};
    const char *enc[] = {"3/0", "3/1", "3/2", "3/3"};
    for (int i = 0; i < 4; i++) {
        const deck_ctl_t *c = deck_ctl_find(l, n, lcd[i]);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_EQUAL(CTL_LCD_CELL, c->kind);
        TEST_ASSERT_EQUAL(200, c->w);
        TEST_ASSERT_EQUAL(100, c->h);
        TEST_ASSERT_EQUAL(200 * i, c->x);
        c = deck_ctl_find(l, n, enc[i]);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_EQUAL(CTL_ENCODER, c->kind);
        TEST_ASSERT_EQUAL(i, c->hw);
        TEST_ASSERT_EQUAL_PTR(c, deck_ctl_by_hw(l, n, CTL_ENCODER, (uint8_t)i));
    }
}

void test_plus_xl_controls(void)
{
    deck_ctl_t l[64];
    int n = ctls(0x00C6, l);
    TEST_ASSERT_EQUAL(36, count_kind(l, n, CTL_KEY_LCD));
    TEST_ASSERT_EQUAL(112, l[0].w);
    const char *lcd[] = {"4/0", "4/2", "4/3", "4/5", "4/6", "4/8"};
    const char *enc[] = {"5/0", "5/2", "5/3", "5/5", "5/6", "5/8"};
    for (int i = 0; i < 6; i++) {
        const deck_ctl_t *c = deck_ctl_find(l, n, lcd[i]);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_EQUAL(CTL_LCD_CELL, c->kind);
        TEST_ASSERT_EQUAL(200 * i, c->x);
        TEST_ASSERT_EQUAL(200, c->w);
        c = deck_ctl_find(l, n, enc[i]);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_EQUAL(CTL_ENCODER, c->kind);
        TEST_ASSERT_EQUAL(i, c->hw);
    }
}

void test_neo_controls(void)
{
    deck_ctl_t l[64];
    int n = ctls(0x009A, l);
    TEST_ASSERT_EQUAL(8, count_kind(l, n, CTL_KEY_LCD));
    const deck_ctl_t *a = deck_ctl_find(l, n, "2/0");
    const deck_ctl_t *b = deck_ctl_find(l, n, "2/3");
    const deck_ctl_t *i = deck_ctl_find(l, n, "2/1");
    TEST_ASSERT_EQUAL(CTL_KEY_RGB, a->kind);
    TEST_ASSERT_EQUAL(8, a->hw);
    TEST_ASSERT_EQUAL(CTL_KEY_RGB, b->kind);
    TEST_ASSERT_EQUAL(9, b->hw);
    TEST_ASSERT_EQUAL(CTL_INFO, i->kind);
    TEST_ASSERT_EQUAL(248, i->w);
    TEST_ASSERT_EQUAL(58, i->h);
    TEST_ASSERT_EQUAL(IMG_XF_ROT180, M(0x009A)->lcd_xf);
    TEST_ASSERT_EQUAL(IMG_XF_NONE, M(0x0084)->lcd_xf);
    TEST_ASSERT_EQUAL(IMG_XF_ROT90_CCW, M(0x00C6)->lcd_xf);
}

void test_pedal_controls(void)
{
    deck_ctl_t l[64];
    int n = ctls(0x0086, l);
    TEST_ASSERT_EQUAL(3, n);
    const char *ids[] = {"0/0", "0/1", "0/2"};
    for (int i = 0; i < 3; i++) {
        const deck_ctl_t *c = deck_ctl_find(l, n, ids[i]);
        TEST_ASSERT_EQUAL(CTL_KEY_NONE, c->kind);
        TEST_ASSERT_EQUAL(i, c->hw);
        TEST_ASSERT_EQUAL(0, c->w);
    }
}

void test_controls_max_too_small(void)
{
    deck_ctl_t l[4];
    TEST_ASSERT_EQUAL(-1, deck_model_controls(M(0x006C), l, 4));
    TEST_ASSERT_EQUAL(-1, deck_model_controls(M(0x0084), l, 4));
}

static cJSON *manifest(uint16_t pid, char **raw)
{
    *raw = deck_manifest_json(M(pid));
    TEST_ASSERT_NOT_NULL(*raw);
    cJSON *j = cJSON_Parse(*raw);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

static const char *preset_of(cJSON *j, const char *id)
{
    cJSON *c = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "controls"), id);
    TEST_ASSERT_NOT_NULL(c);
    return cJSON_GetObjectItem(c, "stylePreset")->valuestring;
}

void test_manifest_xl(void)
{
    char *raw;
    cJSON *j = manifest(0x006C, &raw);
    cJSON *sp = cJSON_GetObjectItem(j, "stylePresets");
    cJSON *def = cJSON_GetObjectItem(sp, "default");
    TEST_ASSERT_TRUE(cJSON_IsObject(def));
    TEST_ASSERT_NULL(def->child);
    cJSON *btn = cJSON_GetObjectItem(sp, "btn_96x96");
    TEST_ASSERT_EQUAL(96, cJSON_GetObjectItem(cJSON_GetObjectItem(btn, "bitmap"), "w")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(cJSON_GetObjectItem(btn, "bitmap"), "format"));
    cJSON *c = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "controls"), "3/7");
    TEST_ASSERT_EQUAL_STRING("btn_96x96", cJSON_GetObjectItem(c, "stylePreset")->valuestring);
    TEST_ASSERT_EQUAL(3, cJSON_GetObjectItem(c, "row")->valueint);
    TEST_ASSERT_EQUAL(7, cJSON_GetObjectItem(c, "column")->valueint);
    cJSON_Delete(j);
    free(raw);
}

void test_manifest_neo_plus_pedal(void)
{
    char *raw;
    cJSON *j = manifest(0x009A, &raw);
    TEST_ASSERT_EQUAL_STRING("rgb", preset_of(j, "2/0"));
    TEST_ASSERT_EQUAL_STRING("hex",
        cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "stylePresets"), "rgb"),
                            "colors")->valuestring);
    TEST_ASSERT_EQUAL_STRING("lcd_248x58", preset_of(j, "2/1"));
    cJSON_Delete(j);
    free(raw);

    j = manifest(0x0084, &raw);
    TEST_ASSERT_EQUAL_STRING("lcd_200x100", preset_of(j, "2/1"));
    TEST_ASSERT_EQUAL_STRING("empty", preset_of(j, "3/1"));
    cJSON_Delete(j);
    free(raw);

    j = manifest(0x0086, &raw);
    TEST_ASSERT_EQUAL_STRING("empty", preset_of(j, "0/2"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "stylePresets"), "btn_0x0"));
    cJSON_Delete(j);
    free(raw);
}

/* the LCD region header of a strip cell is in the strip's own coordinates, before any turn,
 * as the host library writes it (@elgato-stream-deck/core 7.8.0 lcdSegmentDisplay/generic.js: the
 * header gets {...sourceOptions, x, y} with the region's width and height; only the JPEG is turned,
 * rotate + yFlip on the Plus XL, which is IMG_XF_ROT90_CCW). */
static void test_lcd_cell_region(void)
{
    deck_ctl_t ctls[SURF_TEST_CTLS];
    uint16_t x, y, w, h;
    const deck_model_t *plus = M(0x0084), *xl = M(0x00C6);

    int n = deck_model_controls(plus, ctls, SURF_TEST_CTLS);
    const deck_ctl_t *c = deck_ctl_by_hw(ctls, n, CTL_LCD_CELL, 2);
    TEST_ASSERT_NOT_NULL(c);
    deck_lcd_cell_region(plus, c, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(400, x);
    TEST_ASSERT_EQUAL_UINT16(0, y);
    TEST_ASSERT_EQUAL_UINT16(200, w);
    TEST_ASSERT_EQUAL_UINT16(100, h);

    n = deck_model_controls(xl, ctls, SURF_TEST_CTLS);
    c = deck_ctl_by_hw(ctls, n, CTL_LCD_CELL, 5);
    TEST_ASSERT_NOT_NULL(c);
    deck_lcd_cell_region(xl, c, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(1000, x); /* not the turned panel's 0 / 0 / 100 / 200 */
    TEST_ASSERT_EQUAL_UINT16(0, y);
    TEST_ASSERT_EQUAL_UINT16(200, w);
    TEST_ASSERT_EQUAL_UINT16(100, h);
    TEST_ASSERT_EQUAL_INT(IMG_XF_ROT90_CCW, xl->lcd_xf); /* the JPEG is turned as the keys are */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_find_known_and_unknown);
    RUN_TEST(test_xl_controls);
    RUN_TEST(test_original_v1_rtl);
    RUN_TEST(test_plus_controls);
    RUN_TEST(test_plus_xl_controls);
    RUN_TEST(test_neo_controls);
    RUN_TEST(test_lcd_cell_region);
    RUN_TEST(test_pedal_controls);
    RUN_TEST(test_controls_max_too_small);
    RUN_TEST(test_manifest_xl);
    RUN_TEST(test_manifest_neo_plus_pedal);
    return UNITY_END();
}
