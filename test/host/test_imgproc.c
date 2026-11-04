#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgproc/imgproc.h"

void setUp(void) {}
void tearDown(void) {}

// 3x2 image; pixel (x,y) = {10*(y*3+x)+1, +2, +3}
static void make(uint8_t *img)
{
    for (int i = 0; i < 6; i++) { img[i*3] = 10*i+1; img[i*3+1] = 10*i+2; img[i*3+2] = 10*i+3; }
}
static const uint8_t *px(const uint8_t *b, int w, int x, int y) { return b + (y*w + x)*3; }

void test_rot180(void)
{
    uint8_t s[18], d[18]; make(s);
    img_transform_rgb(s, 3, 2, IMG_XF_ROT180, d);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 3; x++)
        TEST_ASSERT_EQUAL_MEMORY(px(s,3,2-x,1-y), px(d,3,x,y), 3);
}

void test_mirror_h(void)
{
    uint8_t s[18], d[18]; make(s);
    img_transform_rgb(s, 3, 2, IMG_XF_MIRROR_H, d);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 3; x++)
        TEST_ASSERT_EQUAL_MEMORY(px(s,3,2-x,y), px(d,3,x,y), 3);
}

void test_rot90_ccw_swaps_dims(void)
{
    uint8_t s[18], d[18]; make(s);
    img_transform_rgb(s, 3, 2, IMG_XF_ROT90_CCW, d);
    // output is h x w = 2 wide, 3 tall: dst[y][x] = src[x][w-1-y]
    for (int y = 0; y < 3; y++) for (int x = 0; x < 2; x++)
        TEST_ASSERT_EQUAL_MEMORY(px(s,3,2-y,x), px(d,2,x,y), 3);
}

void test_mini_transform(void)
{
    uint8_t s[27], d[27];
    for (int i = 0; i < 9; i++) { s[i*3] = i; s[i*3+1] = 100+i; s[i*3+2] = 200+i; }
    img_transform_rgb(s, 3, 3, IMG_XF_MINI, d);
    for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++)
        TEST_ASSERT_EQUAL_MEMORY(px(s,3,2-y,x), px(d,3,x,y), 3);
}

void test_bmp_header_and_rows(void)
{
    uint8_t rgb[12] = {1,2,3, 4,5,6, 7,8,9, 10,11,12};
    uint8_t out[80]; memset(out, 0xAA, sizeof out);
    TEST_ASSERT_EQUAL_UINT(70, img_bmp_pack(rgb, 2, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT8('B', out[0]); TEST_ASSERT_EQUAL_UINT8('M', out[1]);
    TEST_ASSERT_EQUAL_UINT8(70, out[2]);
    TEST_ASSERT_EQUAL_UINT8(54, out[10]);
    TEST_ASSERT_EQUAL_UINT8(40, out[14]);
    TEST_ASSERT_EQUAL_UINT8(2, out[18]);
    TEST_ASSERT_EQUAL_UINT8(2, out[22]);  TEST_ASSERT_EQUAL_UINT8(0, out[25]); // positive
    TEST_ASSERT_EQUAL_UINT8(24, out[28]);
    TEST_ASSERT_EQUAL_UINT8(0x13, out[38]); TEST_ASSERT_EQUAL_UINT8(0x0B, out[39]); // 2835
    TEST_ASSERT_EQUAL_UINT8(3, out[54]); TEST_ASSERT_EQUAL_UINT8(2, out[55]); TEST_ASSERT_EQUAL_UINT8(1, out[56]);
    TEST_ASSERT_EQUAL_UINT8(0, out[54+6]); TEST_ASSERT_EQUAL_UINT8(0, out[54+7]);
    // second input row is written second
    TEST_ASSERT_EQUAL_UINT8(9, out[62]);
    TEST_ASSERT_EQUAL_UINT8(0, out[62+6]); TEST_ASSERT_EQUAL_UINT8(0, out[62+7]);
    TEST_ASSERT_EQUAL_UINT(0, img_bmp_pack(rgb, 2, 2, out, 69));
}

void test_bmp_72px_size(void)
{
    static uint8_t rgb[80*80*3], out[19254];
    TEST_ASSERT_EQUAL_UINT(15606, img_bmp_pack(rgb, 72, 72, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT(19254, img_bmp_pack(rgb, 80, 80, out, sizeof out));
}

void test_fill(void)
{
    uint8_t b[12] = {0};
    img_fill_rgb(b, 2, 2, 9, 8, 7);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(9, b[i*3]); TEST_ASSERT_EQUAL_UINT8(8, b[i*3+1]); TEST_ASSERT_EQUAL_UINT8(7, b[i*3+2]);
    }
}

void test_dim_halves(void)
{
    uint8_t b[4] = {200, 255, 1, 0};
    img_dim_rgb(b, 4, 50);
    TEST_ASSERT_EQUAL_UINT8(100, b[0]); TEST_ASSERT_EQUAL_UINT8(127, b[1]);
    TEST_ASSERT_EQUAL_UINT8(0, b[2]);   TEST_ASSERT_EQUAL_UINT8(0, b[3]);
    uint8_t c[2] = {200, 255};
    img_dim_rgb(c, 2, 250); // clamped to 100
    TEST_ASSERT_EQUAL_UINT8(200, c[0]); TEST_ASSERT_EQUAL_UINT8(255, c[1]);
}

// The P4 JPEG encoder's "RGB888" input is BGR in memory: Companion's yellow (ff ff 00) must
// reach it as 00 ff ff, or it comes out cyan on the deck.
void test_rgb_to_bgr_swaps_red_and_blue(void)
{
    const uint8_t yellow_red[6] = { 0xff, 0xff, 0x00, 0xff, 0x00, 0x00 };
    uint8_t d[6];
    img_rgb_to_bgr(yellow_red, d, 2);
    const uint8_t want[6] = { 0x00, 0xff, 0xff, 0x00, 0x00, 0xff };
    TEST_ASSERT_EQUAL_MEMORY(want, d, 6);
}

void test_rgb_to_bgr_in_place(void)
{
    uint8_t b[6]; memcpy(b, (const uint8_t[]){ 1, 2, 3, 4, 5, 6 }, 6);
    img_rgb_to_bgr(b, b, 2);
    TEST_ASSERT_EQUAL_MEMORY(((const uint8_t[]){ 3, 2, 1, 6, 5, 4 }), b, 6);
}

/* img_untransform_rgb undoes img_transform_rgb exactly (the box's dimmed "Reconnecting" in dock mode: Companion's
 * key images come back from the deck's orientation to the upright one), on odd, non-square sizes too */
static void fill_rand(uint8_t *b, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        b[i] = (uint8_t)(seed >> 16);
    }
}

void test_untransform_inverts_every_transform(void)
{
    const img_xform_t xf[] = {IMG_XF_NONE, IMG_XF_ROT180, IMG_XF_ROT90_CCW, IMG_XF_MIRROR_H, IMG_XF_MINI};
    const int sizes[][2] = {{7, 5}, {5, 7}, {1, 4}, {96, 96}, {9, 9}};
    for (size_t i = 0; i < sizeof xf / sizeof xf[0]; i++)
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            const int w = sizes[s][0], h = sizes[s][1];
            if (xf[i] == IMG_XF_MINI && w != h) continue; /* square only */
            const size_t n = (size_t)w * h * 3;
            uint8_t *a = malloc(n), *t = malloc(n), *b = malloc(n);
            fill_rand(a, n, (uint32_t)(i * 31 + s));
            img_transform_rgb(a, w, h, xf[i], t);
            memset(b, 0, n);
            img_untransform_rgb(t, w, h, xf[i], b);
            char msg[48];
            snprintf(msg, sizeof msg, "xform %d, %dx%d", (int)xf[i], w, h);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(a, b, n, msg);
            free(a);
            free(t);
            free(b);
        }
}

/* img_bmp_unpack reads back what img_bmp_pack writes (and what a gen1 deck takes from Companion): rows in file
 * order, BGR, padded to 4 bytes; trailing bytes (a gen1 report's zero padding) ignored; anything else refused */
void test_bmp_unpack_round_trip(void)
{
    const int sizes[][2] = {{72, 72}, {80, 80}, {5, 3}, {1, 1}};
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        const int w = sizes[s][0], h = sizes[s][1];
        const size_t n = (size_t)w * h * 3, cap = 54 + (size_t)h * ((w * 3 + 3) & ~3) + 64;
        uint8_t *a = malloc(n), *b = malloc(n), *bmp = calloc(1, cap);
        fill_rand(a, n, (uint32_t)s + 7);
        const size_t len = img_bmp_pack(a, w, h, bmp, cap);
        TEST_ASSERT_TRUE(len > 0);
        TEST_ASSERT_TRUE(img_bmp_unpack(bmp, len, w, h, b));
        TEST_ASSERT_EQUAL_MEMORY(a, b, n);
        TEST_ASSERT_TRUE(img_bmp_unpack(bmp, cap, w, h, b)); /* zeros after it */
        TEST_ASSERT_EQUAL_MEMORY(a, b, n);
        TEST_ASSERT_FALSE(img_bmp_unpack(bmp, len - 1, w, h, b)); /* cut short */
        TEST_ASSERT_FALSE(img_bmp_unpack(bmp, len, w + 1, h, b)); /* another size */
        TEST_ASSERT_FALSE(img_bmp_unpack(bmp, len, w, h + 1, b));
        bmp[0] = 'X';
        TEST_ASSERT_FALSE(img_bmp_unpack(bmp, len, w, h, b)); /* not "BM" */
        bmp[0] = 'B';
        bmp[28] = 32;
        TEST_ASSERT_FALSE(img_bmp_unpack(bmp, len, w, h, b)); /* not 24 bpp */
        free(a);
        free(b);
        free(bmp);
    }
    uint8_t one[3];
    TEST_ASSERT_FALSE(img_bmp_unpack(NULL, 100, 1, 1, one));
    TEST_ASSERT_FALSE(img_bmp_unpack((const uint8_t *)"BM", 2, 1, 1, one));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rot180); RUN_TEST(test_mirror_h); RUN_TEST(test_rot90_ccw_swaps_dims);
    RUN_TEST(test_mini_transform); RUN_TEST(test_bmp_header_and_rows); RUN_TEST(test_bmp_72px_size);
    RUN_TEST(test_fill); RUN_TEST(test_dim_halves);
    RUN_TEST(test_rgb_to_bgr_swaps_red_and_blue); RUN_TEST(test_rgb_to_bgr_in_place);
    RUN_TEST(test_untransform_inverts_every_transform); RUN_TEST(test_bmp_unpack_round_trip);
    return UNITY_END();
}
