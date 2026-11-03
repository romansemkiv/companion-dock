#include "unity.h"
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rot180); RUN_TEST(test_mirror_h); RUN_TEST(test_rot90_ccw_swaps_dims);
    RUN_TEST(test_mini_transform); RUN_TEST(test_bmp_header_and_rows); RUN_TEST(test_bmp_72px_size);
    RUN_TEST(test_fill); RUN_TEST(test_dim_halves);
    RUN_TEST(test_rgb_to_bgr_swaps_red_and_blue); RUN_TEST(test_rgb_to_bgr_in_place);
    return UNITY_END();
}
