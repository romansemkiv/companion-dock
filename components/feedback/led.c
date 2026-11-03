/* The status light: one WS2812B-type NeoPixel (GRB) on a header pin, through RMT (one
 * pixel, no DMA). Used by the feedback task only. Nothing breaks without a
 * pixel on the pin: the frames just go nowhere. */
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "feedback_priv.h"

static const char *TAG = "led";

#define RES_HZ (10 * 1000 * 1000) /* 0.1 us ticks */

static rmt_channel_handle_t s_ch;
static rmt_encoder_handle_t s_enc;
static int s_pin = -1;
static uint8_t s_px[3]; /* G, R, B: read by the RMT driver while it sends (internal RAM) */
static fb_rgb_t s_shown;
static bool s_known; /* s_shown is on the pixel */

static void send(fb_rgb_t c)
{
    s_px[0] = c.g;
    s_px[1] = c.r;
    s_px[2] = c.b;
    const rmt_transmit_config_t tc = {.loop_count = 0};
    s_known = rmt_transmit(s_ch, s_enc, s_px, sizeof s_px, &tc) == ESP_OK && rmt_tx_wait_all_done(s_ch, 100) == ESP_OK;
    s_shown = c;
}

bool led_open(int gpio)
{
    led_close();
    if (!s_enc) {
        const rmt_bytes_encoder_config_t ec = {
            /* T0H 0.3 us, T0L 0.9, T1H 0.9, T1L 0.3 */
            .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
            .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
            .flags.msb_first = 1,
        };
        if (rmt_new_bytes_encoder(&ec, &s_enc) != ESP_OK) {
            s_enc = NULL;
            return false;
        }
    }
    const rmt_tx_channel_config_t cc = {
        .gpio_num = gpio, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = RES_HZ,
        .mem_block_symbols = 48, .trans_queue_depth = 2,
    };
    if (rmt_new_tx_channel(&cc, &s_ch) != ESP_OK) {
        s_ch = NULL;
        return false;
    }
    if (rmt_enable(s_ch) != ESP_OK) {
        rmt_del_channel(s_ch);
        s_ch = NULL;
        return false;
    }
    s_pin = gpio;
    s_known = false;
    ESP_LOGI(TAG, "status light on GPIO%d", gpio);
    return true;
}

void led_close(void)
{
    if (!s_ch) return;
    send((fb_rgb_t){0, 0, 0}); /* the pixel keeps its last colour otherwise */
    rmt_disable(s_ch);
    rmt_del_channel(s_ch);
    s_ch = NULL;
    gpio_reset_pin(s_pin);
    s_pin = -1;
}

void led_show(fb_rgb_t c)
{
    if (!s_ch || (s_known && c.r == s_shown.r && c.g == s_shown.g && c.b == s_shown.b)) return;
    send(c);
}
