/* The restart breadcrumb (breadcrumb.h): the record lives in RTC no-init memory, which a software
 * restart, a panic and the watchdogs keep and a power-on leaves undefined (bc_valid and the reset
 * reason sort that out, breadcrumb_logic.c). */
#include "breadcrumb/breadcrumb.h"

#include <stdbool.h>
#include <string.h>

#include "breadcrumb_logic.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"

#if !SOC_RTC_MEM_SUPPORTED
#error "the restart breadcrumb lives in RTC memory (RTC_NOINIT_ATTR)"
#endif

static const char *TAG = "breadcrumb";

#define SUMMARY_MAX 96

static RTC_NOINIT_ATTR bc_record_t s_rtc;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED; /* s_rtc: two restarts may race */

/* Read once by breadcrumb_boot; never written afterwards. */
static const char *s_reset = "unknown";
static char s_last[BC_REASON_MAX];
static char s_summary[SUMMARY_MAX] = "unknown";

static void record(const char *why, bc_stage_t stage)
{
    portENTER_CRITICAL(&s_mux);
    bc_encode(&s_rtc, why, stage);
    portEXIT_CRITICAL(&s_mux);
}

void breadcrumb_scheduled(const char *why) { record(why, BC_STAGE_SCHEDULED); }

void breadcrumb_restart(const char *why)
{
    record(why, BC_STAGE_RESTARTING);
    esp_restart();
}

void breadcrumb_note(const char *why) { record(why, BC_STAGE_RESTARTING); }

void breadcrumb_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    bc_clear(&s_rtc);
    portEXIT_CRITICAL(&s_mux);
}

static const char *reset_name(esp_reset_reason_t r, bc_reset_kind_t *kind)
{
    *kind = BC_RESET_OTHER;
    switch (r) {
    case ESP_RST_POWERON: *kind = BC_RESET_POWERON; return "power-on";
    case ESP_RST_SW: *kind = BC_RESET_DELIBERATE; return "software restart";
    case ESP_RST_PANIC: *kind = BC_RESET_CRASH; return "panic";
    case ESP_RST_INT_WDT: *kind = BC_RESET_CRASH; return "interrupt watchdog";
    case ESP_RST_TASK_WDT: *kind = BC_RESET_CRASH; return "task watchdog";
    case ESP_RST_WDT: *kind = BC_RESET_CRASH; return "watchdog";
    case ESP_RST_BROWNOUT: *kind = BC_RESET_CRASH; return "brownout";
    case ESP_RST_CPU_LOCKUP: *kind = BC_RESET_CRASH; return "CPU lockup";
    case ESP_RST_PWR_GLITCH: *kind = BC_RESET_CRASH; return "power glitch";
    case ESP_RST_EXT: return "RESET pin";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    default: return "unknown";
    }
}

void breadcrumb_boot(void)
{
    bc_reset_kind_t kind;
    s_reset = reset_name(esp_reset_reason(), &kind);
    portENTER_CRITICAL(&s_mux);
    const bc_record_t r = s_rtc;
    bc_clear(&s_rtc);
    portEXIT_CRITICAL(&s_mux);
    if (kind != BC_RESET_POWERON && bc_valid(&r)) strlcpy(s_last, r.reason, sizeof s_last);
    bc_summary(s_summary, sizeof s_summary, s_reset, kind, &r);
    if (kind == BC_RESET_CRASH)
        ESP_LOGE(TAG, "reset: %s (a core dump may be in flash: idf.py coredump-info)", s_summary);
    else
        ESP_LOGI(TAG, "reset: %s", s_summary);
}

const char *breadcrumb_reset_reason(void) { return s_reset; }
const char *breadcrumb_last(void) { return s_last; }
const char *breadcrumb_summary(void) { return s_summary; }
