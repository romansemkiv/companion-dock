#pragma once
// Pure RGB888 image helpers (no ESP-IDF dependencies).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "imgproc/xform.h"

#ifdef __cplusplus
extern "C" {
#endif

// RGB888 transform. src is w*h*3 bytes. dst must hold w*h*3 bytes and must not
// overlap src. ROT90_CCW produces an h x w image. MINI requires w == h.
//   ROT180    dst[y][x] = src[h-1-y][w-1-x]
//   ROT90_CCW dst[y][x] = src[x][w-1-y]
//   MIRROR_H  dst[y][x] = src[y][w-1-x]
//   MINI      dst[y][x] = src[x][w-1-y]   (square images)
void img_transform_rgb(const uint8_t *src, int w, int h, img_xform_t xf, uint8_t *dst);

// The inverse: src is what img_transform_rgb made of a w x h image with xf (h x w for ROT90_CCW and MINI), dst
// gets the w x h image back. dst must not overlap src. (A deck's image back upright, e.g. dimmed for "Reconnecting".)
void img_untransform_rgb(const uint8_t *src, int w, int h, img_xform_t xf, uint8_t *dst);

// 24 bpp BMP (BITMAPFILEHEADER + BITMAPINFOHEADER, positive height). Rows are
// written in input order as BGR, padded to 4 bytes. Returns total size, or 0
// if cap is too small.
size_t img_bmp_pack(const uint8_t *rgb, int w, int h, uint8_t *out, size_t cap);

// The reverse of img_bmp_pack: a 24 bpp uncompressed BMP of exactly w x h (either sign of height) from bmp (len
// bytes; bytes after the image, as a gen1 report's zero padding, are ignored) into rgb (w*h*3), rows in file order.
// False, with rgb untouched, for anything else.
bool img_bmp_unpack(const uint8_t *bmp, size_t len, int w, int h, uint8_t *rgb);

// RGB888 -> BGR888 (swaps bytes 0 and 2 of every pixel). dst may equal src. The P4 hardware
// JPEG encoder's JPEG_ENCODE_IN_FORMAT_RGB888 reads BGR byte order (ESP-IDF jpeg docs).
void img_rgb_to_bgr(const uint8_t *src, uint8_t *dst, size_t pixels);

void img_fill_rgb(uint8_t *dst, int w, int h, uint8_t r, uint8_t g, uint8_t b);

// Each byte = byte * pct / 100 (pct clamped to 100).
void img_dim_rgb(uint8_t *buf, size_t bytes, uint8_t pct);

#ifdef __cplusplus
}
#endif
