#pragma once
// ESP-only hardware JPEG decoder wrapper (the P4's codec, shared with the encoder: the driver takes turns).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A baseline JPEG of exactly w x h (at most 512 x 512) as w*h*3 bytes of RGB888, R first, at rgb. False, with rgb
// untouched, for another size, a JPEG the codec can't take (progressive), no memory, or a codec error. Allocates its
// DMA buffers per call (it is used rarely: Companion's last key images, dimmed when it lets go of a deck).
// One caller at a time (the surfaces task); the codec itself is shared with the encoder by the driver.
bool img_jpeg_decode(const uint8_t *jpg, size_t len, int w, int h, uint8_t *rgb);

#ifdef __cplusplus
}
#endif
