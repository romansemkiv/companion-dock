#include "imgproc/jpeg_dec.h"

#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_log.h"

static const char *TAG = "jpeg_dec";
static jpeg_decoder_handle_t s_dec; /* made at the first decode */

static bool engine(void)
{
    if (s_dec) return true;
    const jpeg_decode_engine_cfg_t cfg = {.intr_priority = 0, .timeout_ms = 100};
    if (jpeg_new_decoder_engine(&cfg, &s_dec) == ESP_OK) return true;
    s_dec = NULL;
    ESP_LOGW(TAG, "no decoder engine");
    return false;
}

bool img_jpeg_decode(const uint8_t *jpg, size_t len, int w, int h, uint8_t *rgb)
{
    if (!jpg || !len || !rgb || w <= 0 || h <= 0 || w > 512 || h > 512) return false;
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg, (uint32_t)len, &info) != ESP_OK || info.width != (uint32_t)w ||
        info.height != (uint32_t)h)
        return false;
    if (!engine()) return false;
    /* the codec may pad its output to whole 16-pixel blocks (IDF's jpeg_decode example): room for that */
    const size_t pw = ((size_t)w + 15) & ~(size_t)15, ph = ((size_t)h + 15) & ~(size_t)15;
    const jpeg_decode_memory_alloc_cfg_t icfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    const jpeg_decode_memory_alloc_cfg_t ocfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t in_cap = 0, out_cap = 0;
    uint8_t *in = jpeg_alloc_decoder_mem(len, &icfg, &in_cap);
    uint8_t *out = jpeg_alloc_decoder_mem(pw * ph * 3, &ocfg, &out_cap);
    bool ok = false;
    if (in && out) {
        memcpy(in, jpg, len);
        /* ORDER_RGB: R first in memory (ORDER_BGR is OpenCV's B first, per the IDF example) */
        const jpeg_decode_cfg_t dcfg = {.output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
                                        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
                                        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601};
        uint32_t n = 0;
        if (jpeg_decoder_process(s_dec, &dcfg, in, (uint32_t)len, out, (uint32_t)out_cap, &n) == ESP_OK) {
            const size_t row = (size_t)w * 3;
            /* rows exactly w wide, or padded to pw */
            const size_t stride = n == row * (size_t)h ? row : pw * 3;
            if ((size_t)n >= stride * ((size_t)h - 1) + row) {
                for (int y = 0; y < h; y++) memcpy(rgb + (size_t)y * row, out + (size_t)y * stride, row);
                ok = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "no memory for a %dx%d decode", w, h);
    }
    free(in);
    free(out);
    return ok;
}
