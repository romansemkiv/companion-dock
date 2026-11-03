/* The last 16 KB of log output in PSRAM, for Log.Get.
 * Readers pass the sequence number they got last time and receive only newer text.
 *
 * The box prints its hotspot password on the serial console by design, so the ring holds
 * it too. Log.Get is therefore not a public method, like Ap.GetConfig, which returns that password:
 * both need the settings password when one is set. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "webui/webui.h"
#include "webui_priv.h"

#define LOG_SIZE (16 * 1024)
#define LINE_MAX_LEN 256 /* longer lines are cut in the ring (the console gets them whole) */
#define READ_PIECE 512  /* bytes copied per critical section */

static char *s_buf;
static uint32_t s_written; /* bytes ever written: the sequence */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig;

static int hook(const char *fmt, va_list ap)
{
    char line[LINE_MAX_LEN];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (n > 0 && s_buf) {
        if (n >= (int)sizeof line) n = sizeof line - 1;
        taskENTER_CRITICAL(&s_mux);
        const size_t len = (size_t)n, at = s_written % LOG_SIZE, first = LOG_SIZE - at < len ? LOG_SIZE - at : len;
        memcpy(s_buf + at, line, first);          /* up to the end of the ring... */
        memcpy(s_buf, line + first, len - first); /* ...and the rest from its start */
        s_written += (uint32_t)len;
        taskEXIT_CRITICAL(&s_mux);
    }
    return s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
}

/* Copies in small pieces so interrupts stay off for a few hundred bytes at a time. A piece the
 * writers overwrote meanwhile ends the read there; *seq says where to go on. */
size_t logbuf_read(char *out, size_t max, uint32_t *seq)
{
    if (!s_buf || !out || !max) return 0;
    taskENTER_CRITICAL(&s_mux);
    const uint32_t end = s_written;
    taskEXIT_CRITICAL(&s_mux);
    uint32_t start = *seq;
    if (start > end || end - start > LOG_SIZE) start = end > LOG_SIZE ? end - LOG_SIZE : 0;
    if (end - start > max) start = end - (uint32_t)max;
    uint32_t pos = start;
    size_t got = 0;
    while (pos < end) {
        const size_t n = end - pos < READ_PIECE ? end - pos : READ_PIECE;
        bool lost;
        taskENTER_CRITICAL(&s_mux);
        lost = s_written - pos > LOG_SIZE - n; /* the ring came round over this piece */
        if (!lost) {
            const size_t at = pos % LOG_SIZE, first = LOG_SIZE - at < n ? LOG_SIZE - at : n;
            memcpy(out + got, s_buf + at, first);
            memcpy(out + got + first, s_buf, n - first);
        }
        taskEXIT_CRITICAL(&s_mux);
        if (lost) break;
        pos += (uint32_t)n;
        got += n;
    }
    *seq = pos;
    return got;
}

void webui_log_capture_start(void)
{
    if (s_buf) return;
    char *b = heap_caps_calloc(1, LOG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
        ESP_LOGW("logbuf", "no PSRAM for the log ring: Log.Get stays empty");
        return;
    }
    s_buf = b;
    s_orig = esp_log_set_vprintf(hook);
}
