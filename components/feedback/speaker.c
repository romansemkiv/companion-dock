/* The board's speaker: the ES8311 codec (control on the board's I2C bus, GPIO7 SDA / GPIO8 SCL,
 * address 0x18; I2S MCLK 13, BCLK 12, WS 10, DSDIN 9 to the codec) and its amplifier (enable GPIO53),
 * through esp_codec_dev, 16 kHz mono, output only (no
 * microphone).
 *
 * speaker_play queues a request (never blocks); the speaker task hands it to the player (fbsound.c:
 * which sound plays, its samples) and writes 10 ms blocks to the codec, the I2S DMA pacing it. Between
 * blocks it takes the next requests, so a new sound starts within a block. After SPK_IDLE_US of
 * silence the codec is closed, which also turns the amplifier off: no hiss while nothing plays. It is
 * opened again for the next sound (a few I2C writes, on this task only).
 *
 * The amplifier: GPIO53 is driven low from the first line of the boot (speaker_amp_off), before the
 * codec exists, after every close, and at every esp_restart (a shutdown handler).
 *
 * Failures: the codec comes up on this task, so the boot never waits for it; one that doesn't answer
 * then is logged once and requests are dropped for the rest of the run. One that fails later (an open
 * or a write) is closed and the amplifier dropped (esp_codec_dev leaves its open state set, and the
 * ES8311 driver its amplifier on, when an enable fails half-way); it is tried again only when a sound
 * comes after a growing wait (fbsound.h, fb_retry_t: 10 s, doubling to 10 min). A streak logs once
 * when it begins and once when it ends; the sounds meanwhile are dropped without a word.
 *
 * Logging: esp_codec_dev logs about four INFO lines at every open (each first sound after a quiet
 * spell); its tags are set to WARN, so only its warnings and errors reach the log ring and the UART. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fbsound.h"
#include "feedback_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "speaker";

#define RATE 16000
#define BLOCK (RATE / 100) /* 10 ms */
#define DMA_BUFS 4         /* 4 x 10 ms queued in the I2S DMA at most: a new sound is heard <= 40 ms later */
/* Silence before the codec closes and the amplifier goes off. It trades the amplifier's switch-on pop
 * (and the reopen) against hiss while idle: at 3 s a burst of key presses costs one amplifier cycle
 * instead of one per press, at the price of up to 3 s of possible hiss after the last sound. To be
 * tuned on the board (docs/manual.md, Hardware test list). */
#define SPK_IDLE_US (3 * 1000000LL)
#define CODEC_VOL 85 /* the codec's own level; the volume setting scales the samples (fbsound.c) */
#define I2C_PORT 0
#define PIN_SDA 7
#define PIN_SCL 8
#define PIN_MCLK 13
#define PIN_BCLK 12
#define PIN_WS 10
#define PIN_DOUT 9
#define PIN_PA 53
#define QUEUE_LEN 4
#define STACK 4096 /* PSRAM: I2C, I2S and the player only */
#define PRIO 3     /* below the image worker and the deck tasks: a sound never holds up input or images */

typedef struct {
    uint8_t ev, volume;
} req_t;

typedef enum {
    SPK_STARTING, /* the codec is being brought up */
    SPK_OK,
    SPK_DEAD,     /* it never came up: no sound in this run */
    SPK_BROKEN,   /* it failed after the start: tried again later (fb_retry_t) */
} spk_state_t;

/* esp_codec_dev's log tags (managed_components/espressif__esp_codec_dev), WARN and up only */
static const char *const CODEC_TAGS[] = {"Adev_Codec", "I2S_IF", "I2C_If", "GPIO_If", "ES8311", "CODEC_REF_MGR"};

static QueueHandle_t s_q;
static esp_codec_dev_handle_t s_out;
static i2s_chan_handle_t s_tx;       /* the codec's I2S channel */
static bool s_tx_on;                 /* s_tx is enabled: while the codec is open (speaker task only) */
static atomic_int s_state;           /* spk_state_t */
static _Atomic(const char *) s_why;  /* why not SPK_OK (a literal), set before s_state */
static fb_player_t s_player;         /* speaker task only */
static fb_retry_t s_retry;           /* speaker task only */
static int16_t s_buf[BLOCK];         /* speaker task only */

void speaker_amp_off(void)
{
    gpio_set_level(PIN_PA, 0); /* the output register first: the pin never drives high on the way */
    const gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_PA, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(PIN_PA, 0);
}

/* GPIO53 low once speaker_amp_off configured it; also esp_restart's shutdown handler (every
 * deliberate restart: breadcrumb_restart, the OTA rollback, IDF's own) */
static void amp_low(void)
{
    gpio_set_level(PIN_PA, 0);
}

static bool dead(const char *why)
{
    atomic_store(&s_why, why);
    atomic_store(&s_state, SPK_DEAD);
    return false;
}

static bool codec_new(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bc = {
        .i2c_port = I2C_PORT, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return dead("the board's I2C bus did not start");

    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true; /* an underrun plays silence, not the last block again */
    cc.dma_desc_num = DMA_BUFS;
    cc.dma_frame_num = BLOCK;
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK) return dead("no I2S channel");
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED},
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) return dead("I2S did not start");
    s_tx = tx; /* initialised, not enabled: codec_open enables it */

    audio_codec_i2s_cfg_t ic = {.port = I2S_NUM_0, .tx_handle = tx};
    audio_codec_i2c_cfg_t cc2 = {.port = I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&ic);
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&cc2);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    es8311_codec_cfg_t ec = {
        .ctrl_if = ctrl, .gpio_if = gpio, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin = PIN_PA,
        .use_mclk = true, .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec = data && ctrl && gpio ? es8311_codec_new(&ec) : NULL;
    if (!codec) return dead("the ES8311 codec does not answer at I2C 0x18");
    esp_codec_dev_cfg_t od = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data};
    s_out = esp_codec_dev_new(&od);
    if (!s_out) return dead("the codec did not start");
    return true;
}

/* esp_codec_dev_open disables the I2S channel before it sets the format, and enables it after; IDF logs
 * an error for disabling a channel that isn't enabled ("i2s_channel_disable ... not been enabled yet",
 * at the first sound after the boot and after every close). So the channel is enabled first: it is
 * enabled exactly while the codec is open. */
static bool codec_open(void)
{
    if (!s_tx_on && i2s_channel_enable(s_tx) != ESP_OK) return false;
    s_tx_on = true; /* esp_codec_dev_open leaves it enabled */
    esp_codec_dev_sample_info_t fs = {.sample_rate = RATE, .channel = 1, .bits_per_sample = 16};
    return esp_codec_dev_open(s_out, &fs) == ESP_CODEC_DEV_OK &&
           esp_codec_dev_set_out_vol(s_out, CODEC_VOL) == ESP_CODEC_DEV_OK;
}

static void codec_close(void)
{
    esp_codec_dev_close(s_out); /* mutes the codec, disables the I2S channel, drops the amplifier when it was enabled */
    s_tx_on = false;
    amp_low();                  /* and when an enable failed half-way, which leaves the amplifier on */
}

/* An open or a write failed at now_us: closed, the amplifier off, the sound dropped, tried again later. */
static void codec_failed(const char *why, int64_t now_us)
{
    codec_close();
    fb_player_init(&s_player, RATE);
    if (!fb_retry_failed(&s_retry, now_us)) return; /* the streak was logged when it began */
    atomic_store(&s_why, why);
    atomic_store(&s_state, SPK_BROKEN);
    ESP_LOGE(TAG, "%s: no sound; the codec is tried again with a sound after 10 s, then less often", why);
}

static void spk_task(void *arg)
{
    (void)arg;
    req_t r;
    if (!codec_new()) {
        amp_low(); /* it may have failed after the driver took the pin */
        ESP_LOGE(TAG, "%s: no sound", atomic_load(&s_why));
        for (;;) xQueueReceive(s_q, &r, portMAX_DELAY); /* dropped (speaker_play refuses them anyway) */
    }
    atomic_store(&s_state, SPK_OK);
    ESP_LOGI(TAG, "ES8311 ready (%d Hz)", RATE);
    bool open = false;
    int64_t last = 0;
    for (;;) {
        TickType_t wait = !fb_player_idle(&s_player) ? 0 : open ? pdMS_TO_TICKS(100) : portMAX_DELAY;
        while (xQueueReceive(s_q, &r, wait) == pdTRUE) {
            wait = 0;
            if (open || fb_retry_due(&s_retry, esp_timer_get_time())) /* else dropped: the codec rests */
                fb_player_post(&s_player, (fb_event_t)r.ev, r.volume);
        }
        const int64_t now = esp_timer_get_time();
        if (fb_player_idle(&s_player)) {
            if (open && now - last > SPK_IDLE_US) {
                codec_close();
                open = false;
            }
            continue;
        }
        if (!open) {
            if (!codec_open()) {
                codec_failed("the codec stopped answering (it did not open)", now);
                continue;
            }
            open = true;
            if (fb_retry_ok(&s_retry)) {
                atomic_store(&s_state, SPK_OK);
                ESP_LOGW(TAG, "the codec answers again: sound is back");
            }
        }
        fb_player_fill(&s_player, s_buf, BLOCK);
        /* blocks until the DMA takes it: the pace */
        if (esp_codec_dev_write(s_out, s_buf, sizeof s_buf) != ESP_CODEC_DEV_OK) {
            codec_failed("audio data did not reach the codec", now);
            open = false;
            continue;
        }
        last = esp_timer_get_time();
    }
}

esp_err_t speaker_start(void)
{
    if (s_q) return ESP_OK;
    speaker_amp_off();
    esp_register_shutdown_handler(amp_low);
    for (size_t i = 0; i < sizeof CODEC_TAGS / sizeof *CODEC_TAGS; i++) esp_log_level_set(CODEC_TAGS[i], ESP_LOG_WARN);
    fb_player_init(&s_player, RATE);
    fb_retry_init(&s_retry);
    s_q = xQueueCreate(QUEUE_LEN, sizeof(req_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreateWithCaps(spk_task, "speaker", STACK, NULL, PRIO, NULL, MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool speaker_play(fb_event_t ev, int volume)
{
    if (!s_q || atomic_load(&s_state) == SPK_DEAD || (unsigned)ev >= FB_EV_COUNT) return false;
    const req_t r = {(uint8_t)ev, (uint8_t)(volume < 0 ? 0 : volume > 100 ? 100 : volume)};
    return xQueueSend(s_q, &r, 0) == pdTRUE;
}

const char *speaker_error(void)
{
    switch (atomic_load(&s_state)) {
    case SPK_OK: return NULL;
    case SPK_STARTING: return "starting";
    default: return atomic_load(&s_why);
    }
}
