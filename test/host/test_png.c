/* img_png_decode_rgb: Companion's PNG button images (Satellite BITMAP_FORMAT=png) to the RGB the image worker
 * draws from. The fixtures (fixtures/png, made by Pillow: make_fixtures.py) cover every colour type and depth;
 * each <name>.rgb is what the decoder must give: alpha over black, 16-bit samples by their high byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgproc/png.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t *load(const char *name, const char *ext, size_t *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.%s", PNG_FIXTURES, name, ext);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL((size_t)n, fread(b, 1, (size_t)n, f));
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void check(const char *name, int w, int h)
{
    size_t plen, rlen;
    uint8_t *png = load(name, "png", &plen), *want = load(name, "rgb", &rlen);
    TEST_ASSERT_EQUAL((size_t)w * h * 3, rlen);
    uint8_t *got = malloc(rlen);
    memset(got, 0xA5, rlen);
    TEST_ASSERT_TRUE_MESSAGE(img_png_decode_rgb(png, plen, w, h, got), name);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, got, rlen, name);
    free(png);
    free(want);
    free(got);
}

static void test_every_colour_type(void)
{
    const char *const names[] = {"rgb8", "rgba8_opaque", "rgba8_alpha", "grey8", "grey_alpha", "grey16",
                                 "bw1", "palette8", "palette4", "palette_trns"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) check(names[i], 16, 16);
}

static void test_button_sized(void) { check("button96", 96, 96); }

/* Another size than the key's: refused, nothing written past the buffer (it is sized for the key) */
static void test_wrong_size_refused(void)
{
    size_t plen;
    uint8_t *png = load("button96", "png", &plen);
    uint8_t out[72 * 72 * 3 + 16];
    memset(out, 0x5A, sizeof out);
    TEST_ASSERT_FALSE(img_png_decode_rgb(png, plen, 72, 72, out));
    TEST_ASSERT_FALSE(img_png_decode_rgb(png, plen, 96, 72, out));
    for (size_t i = 0; i < sizeof out; i++) TEST_ASSERT_EQUAL_HEX8(0x5A, out[i]);
    free(png);
}

/* Not a PNG, cut short, a damaged chunk (CRC), damaged pixel data: refused */
static void test_damaged_refused(void)
{
    size_t plen;
    uint8_t *png = load("button96", "png", &plen);
    uint8_t *out = malloc(96 * 96 * 3);
    TEST_ASSERT_FALSE(img_png_decode_rgb((const uint8_t *)"hello", 5, 96, 96, out));
    TEST_ASSERT_FALSE(img_png_decode_rgb(png, 0, 96, 96, out));
    TEST_ASSERT_FALSE(img_png_decode_rgb(NULL, 10, 96, 96, out));
    TEST_ASSERT_FALSE(img_png_decode_rgb(png, plen, 96, 96, NULL));
    for (size_t cut = 8; cut < plen; cut += plen / 9) TEST_ASSERT_FALSE(img_png_decode_rgb(png, cut, 96, 96, out));
    uint8_t *bad = malloc(plen);
    for (size_t at = 33; at < plen - 12; at += 37) { /* every chunk's data and CRC, from the first after IHDR */
        memcpy(bad, png, plen);
        bad[at] ^= 0x40;
        TEST_ASSERT_FALSE_MESSAGE(img_png_decode_rgb(bad, plen, 96, 96, out), "a damaged byte was taken");
    }
    free(bad);
    free(out);
    free(png);
}

/* A size no key has (over the image worker's largest bitmap): refused before anything is decoded */
static void test_absurd_size_refused(void)
{
    size_t plen;
    uint8_t *png = load("button96", "png", &plen);
    uint8_t out[3];
    TEST_ASSERT_FALSE(img_png_decode_rgb(png, plen, 0, 96, out));
    TEST_ASSERT_FALSE(img_png_decode_rgb(png, plen, 96, -1, out));
    TEST_ASSERT_FALSE(img_png_decode_rgb(png, plen, 100000, 100000, out));
    free(png);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_colour_type);
    RUN_TEST(test_button_sized);
    RUN_TEST(test_wrong_size_refused);
    RUN_TEST(test_damaged_refused);
    RUN_TEST(test_absurd_size_refused);
    return UNITY_END();
}
