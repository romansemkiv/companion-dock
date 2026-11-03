#include "imgproc/jpeg_enc.h"
#include "imgproc/imgproc.h"
#include "driver/jpeg_encode.h"
#include "esp_log.h"

#define IN_BYTES  (120000u * 3u)
#define OUT_BYTES (128u * 1024u)

static const char *TAG = "jpeg_enc";
static jpeg_encoder_handle_t s_enc;
static uint8_t *s_in;
static uint8_t *s_out;
static size_t s_in_cap, s_out_cap;

esp_err_t img_jpeg_init(void)
{
    if (s_enc) return ESP_OK;
    jpeg_encode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 100 };
    esp_err_t err = jpeg_new_encoder_engine(&ecfg, &s_enc);
    if (err != ESP_OK) { s_enc = NULL; return err; }

    jpeg_encode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_in = jpeg_alloc_encoder_mem(IN_BYTES, &in_cfg, &s_in_cap);
    s_out = jpeg_alloc_encoder_mem(OUT_BYTES, &out_cfg, &s_out_cap);
    if (!s_in || !s_out) {
        ESP_LOGE(TAG, "buffer alloc failed");
        jpeg_del_encoder_engine(s_enc);
        s_enc = NULL; s_in = s_out = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t img_jpeg_encode(const uint8_t *rgb, int w, int h, uint8_t **out, size_t *out_len)
{
    if (!s_enc) return ESP_ERR_INVALID_STATE;
    if (!rgb || !out || !out_len || w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;
    size_t bytes = (size_t)w * (size_t)h * 3;
    if (bytes > s_in_cap) return ESP_ERR_INVALID_SIZE;

    /* JPEG_ENCODE_IN_FORMAT_RGB888 is BGR in memory: without the swap red and blue trade places
     * (Companion's yellow shows cyan on the deck). */
    img_rgb_to_bgr(rgb, s_in, (size_t)w * (size_t)h);
    jpeg_encode_cfg_t cfg = {
        .height = (uint32_t)h,
        .width = (uint32_t)w,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV444,
        .image_quality = 90,
        .pixel_reverse = false,
    };
    uint32_t n = 0;
    esp_err_t err = jpeg_encoder_process(s_enc, &cfg, s_in, (uint32_t)bytes, s_out, (uint32_t)s_out_cap, &n);
    if (err != ESP_OK) return err;
    *out = s_out;
    *out_len = n;
    return ESP_OK;
}
