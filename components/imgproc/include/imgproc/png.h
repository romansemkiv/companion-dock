// PNG button images (Satellite mode, BITMAP_FORMAT=png) to RGB: a wrapper over LodePNG (lodepng/, decoder only).
// Pure C: host-tested (test/host/test_png.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The PNG at png (len bytes, any colour type and bit depth) as w x h RGB888 at rgb (w * h * 3 bytes): alpha
 * composited over black ((c * a + 127) / 255), 16-bit samples by their high byte. False, with nothing written, when
 * it is not a PNG, is damaged (a chunk's CRC, the zlib data), or is not exactly w x h; the size is checked before
 * anything is decoded. Allocates its working memory (about w * h * 4 bytes, more while inflating) and frees it. */
bool img_png_decode_rgb(const uint8_t *png, size_t len, int w, int h, uint8_t *rgb);

#ifdef __cplusplus
}
#endif
