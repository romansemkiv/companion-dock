#include "imgproc/imgproc.h"
#include <string.h>

static inline void cp(uint8_t *d, const uint8_t *s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }

void img_transform_rgb(const uint8_t *src, int w, int h, img_xform_t xf, uint8_t *dst)
{
    switch (xf) {
    case IMG_XF_ROT180:
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                cp(&dst[(y * w + x) * 3], &src[((h - 1 - y) * w + (w - 1 - x)) * 3]);
        break;
    case IMG_XF_MIRROR_H:
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                cp(&dst[(y * w + x) * 3], &src[(y * w + (w - 1 - x)) * 3]);
        break;
    case IMG_XF_ROT90_CCW:
    case IMG_XF_MINI:
        // Output is h wide and w tall: dst[y][x] = src[x][w-1-y], y in [0,w), x in [0,h).
        for (int y = 0; y < w; y++)
            for (int x = 0; x < h; x++)
                cp(&dst[(y * h + x) * 3], &src[(x * w + (w - 1 - y)) * 3]);
        break;
    case IMG_XF_NONE:
    default:
        memcpy(dst, src, (size_t)w * h * 3);
        break;
    }
}

void img_untransform_rgb(const uint8_t *src, int w, int h, img_xform_t xf, uint8_t *dst)
{
    switch (xf) {
    case IMG_XF_ROT180:
    case IMG_XF_MIRROR_H: /* each its own inverse */
        img_transform_rgb(src, w, h, xf, dst);
        break;
    case IMG_XF_ROT90_CCW:
    case IMG_XF_MINI:
        // src is h wide and w tall, src[y'][x'] = dst[x'][w-1-y']: so dst[r][c] = src[w-1-c][r].
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++)
                cp(&dst[(r * w + c) * 3], &src[((w - 1 - c) * h + r) * 3]);
        break;
    case IMG_XF_NONE:
    default:
        memcpy(dst, src, (size_t)w * h * 3);
        break;
    }
}

void img_fill_rgb(uint8_t *dst, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
        dst[i * 3] = r; dst[i * 3 + 1] = g; dst[i * 3 + 2] = b;
    }
}

void img_dim_rgb(uint8_t *buf, size_t bytes, uint8_t pct)
{
    if (pct > 100) pct = 100;
    for (size_t i = 0; i < bytes; i++) buf[i] = (uint8_t)(buf[i] * pct / 100);
}

void img_rgb_to_bgr(const uint8_t *src, uint8_t *dst, size_t pixels)
{
    for (size_t i = 0; i < pixels * 3; i += 3) {
        uint8_t r = src[i];
        dst[i + 1] = src[i + 1];
        dst[i] = src[i + 2];
        dst[i + 2] = r;
    }
}
