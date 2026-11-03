#pragma once
// ESP-only hardware JPEG encoder wrapper.
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocates the encoder engine and DMA-capable buffers (input 360000 bytes,
// output 128 KB), sized once for the largest image (120000 RGB pixels).
esp_err_t img_jpeg_init(void);

// Encodes RGB888 (w*h*3 bytes, R first; converted to the encoder's BGR input) to JPEG, quality 90, YUV444 subsampling.
// *out points to an internal buffer valid until the next call.
// NOT thread-safe: a single image worker task must be the only caller.
// Returns ESP_ERR_INVALID_STATE if not initialised, ESP_ERR_INVALID_SIZE if
// w*h*3 exceeds the input buffer.
esp_err_t img_jpeg_encode(const uint8_t *rgb, int w, int h, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
