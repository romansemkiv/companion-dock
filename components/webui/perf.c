/* Sys.GetPerf: how the box is doing, for the web page's Performance page. The CPU shares are over the
 * time since the previous call (any client's), from FreeRTOS's run-time counters
 * (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, 64-bit, from esp_timer: they never wrap); perf_logic.c does
 * the arithmetic. A core's load is what its idle task did not get.
 *
 * Cost: each call takes one snapshot of every task (uxTaskGetSystemState), about 1 ms inside a kernel critical
 * section. Fine every 2 s while the Performance page is open; not something to poll fast. */
#include <stdlib.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "netmgr/netmgr.h"
#include "perf_logic.h"
#include "webui_priv.h"

#define TAG "perf"

_Static_assert(configTASKLIST_INCLUDE_COREID == 1, "TaskStatus_t.xCoreID: CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID");
#define MAX_TASKS 64

static SemaphoreHandle_t s_mtx;
static perf_sample_t s_prev[MAX_TASKS];
static size_t s_n_prev;
static bool s_have_prev;
static int64_t s_prev_us;
static temperature_sensor_handle_t s_tsens;
static bool s_tsens_tried;

static const char *state_name(eTaskState s)
{
    switch (s) {
    case eRunning: return "running";
    case eReady: return "ready";
    case eBlocked: return "blocked";
    case eSuspended: return "suspended";
    case eDeleted: return "deleted";
    default: return "?";
    }
}

/* the chip's temperature, the sensor started on first use; false when it can't be read */
static bool temperature(float *c)
{
    if (!s_tsens_tried) {
        s_tsens_tried = true;
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) {
            ESP_LOGW(TAG, "temperature sensor: not available");
            s_tsens = NULL;
        }
    }
    return s_tsens && temperature_sensor_get_celsius(s_tsens, c) == ESP_OK;
}

static void heap_json(cJSON *parent, const char *name, uint32_t caps)
{
    cJSON *o = cJSON_AddObjectToObject(parent, name);
    cJSON_AddNumberToObject(o, "total", (double)heap_caps_get_total_size(caps));
    cJSON_AddNumberToObject(o, "free", (double)heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(o, "minFree", (double)heap_caps_get_minimum_free_size(caps));
    cJSON_AddNumberToObject(o, "largest", (double)heap_caps_get_largest_free_block(caps));
}

/* tenths of a percent as a number with one decimal; null when unknown */
static void add_share(cJSON *o, const char *k, int pm)
{
    if (pm < 0) cJSON_AddNullToObject(o, k);
    else cJSON_AddNumberToObject(o, k, pm / 10.0);
}

/* Sys.GetPerf -> {windowMs, cpuMhz, tempC, cores: [load % | null], heap: {internal, psram: {total, free,
 * minFree, largest}}, tasks: [{name, core (-1: either), prio, state, cpu (% of one core | null), stackFree
 * (bytes never used)}], eth: {unicast, multi, unwanted, rate, lostBuf, lostFifo, storm, storms} | null}. cpu and cores are
 * null on the first call after a start (no earlier sample yet); windowMs: the time they cover. */
cJSON *webui_perf_json(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex(); /* the HTTP server's task: no race on the first call */
    if (!s_mtx) return NULL;
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4; /* room for a few made meanwhile */
    if (n > MAX_TASKS) n = MAX_TASKS;
    TaskStatus_t *ts = heap_caps_malloc(n * sizeof *ts, MALLOC_CAP_SPIRAM);
    perf_sample_t *cur = heap_caps_malloc(n * sizeof *cur, MALLOC_CAP_SPIRAM);
    char (*names)[configMAX_TASK_NAME_LEN] = heap_caps_malloc(n * sizeof *names, MALLOC_CAP_SPIRAM);
    if (!ts || !cur || !names) {
        free(ts);
        free(cur);
        free(names);
        return NULL;
    }
    cJSON *r = cJSON_CreateObject();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    /* about 1 ms with interrupts off on this core (measured: 34 tasks, 89 KB of stack never used, which it scans
     * for the high-water marks) and the kernel lock held: the reason the page asks no more often than every 2 s */
    UBaseType_t got = uxTaskGetSystemState(ts, n, NULL);
    const int64_t now = esp_timer_get_time();
    /* The names at once: pcTaskName points into the task's own control block, which the idle task frees once the
     * task has deleted itself (the short-lived ones: sta_work, svcbtn_act, ota). A task already deleted in the
     * snapshot is left out, and nothing else of a task is read through its handle (the core is the snapshot's:
     * CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID). Printable ASCII only: the answer stays valid JSON whatever a name
     * read in that last instant holds. */
    for (UBaseType_t i = 0; i < got; i++) {
        if (ts[i].eCurrentState == eDeleted) continue;
        strlcpy(names[i], ts[i].pcTaskName, sizeof names[i]);
        for (char *c = names[i]; *c; c++)
            if (*c < 0x20 || *c > 0x7e) *c = '?';
    }
    const uint64_t dt = s_have_prev && now > s_prev_us ? (uint64_t)(now - s_prev_us) : 0;
    cJSON_AddNumberToObject(r, "windowMs", (double)(dt / 1000));

    uint32_t hz = 0;
    if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz) == ESP_OK)
        cJSON_AddNumberToObject(r, "cpuMhz", hz / 1000000);
    float c;
    if (temperature(&c)) cJSON_AddNumberToObject(r, "tempC", (int)(c * 10 + (c < 0 ? -0.5f : 0.5f)) / 10.0);
    else cJSON_AddNullToObject(r, "tempC");

    cJSON *cores = cJSON_AddArrayToObject(r, "cores");
    cJSON *tasks = cJSON_AddArrayToObject(r, "tasks");
    TaskHandle_t idle[portNUM_PROCESSORS];
    for (int i = 0; i < portNUM_PROCESSORS; i++) idle[i] = xTaskGetIdleTaskHandleForCore(i);
    int idle_pm[portNUM_PROCESSORS];
    for (int i = 0; i < portNUM_PROCESSORS; i++) idle_pm[i] = -1;
    size_t n_cur = 0;
    for (UBaseType_t i = 0; i < got; i++) {
        if (ts[i].eCurrentState == eDeleted) continue;
        const uintptr_t id = (uintptr_t)ts[i].xHandle;
        const uint64_t run = ts[i].ulRunTimeCounter;
        cur[n_cur++] = (perf_sample_t){id, run};
        const int pm = perf_share_permille(s_prev, s_n_prev, s_have_prev, id, run, dt);
        for (int k = 0; k < portNUM_PROCESSORS; k++)
            if (ts[i].xHandle == idle[k]) idle_pm[k] = pm;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", names[i]);
        const BaseType_t core = ts[i].xCoreID;
        cJSON_AddNumberToObject(o, "core", core == tskNO_AFFINITY ? -1 : (double)core);
        cJSON_AddNumberToObject(o, "prio", (double)ts[i].uxCurrentPriority);
        cJSON_AddStringToObject(o, "state", state_name(ts[i].eCurrentState));
        add_share(o, "cpu", pm);
        cJSON_AddNumberToObject(o, "stackFree", (double)ts[i].usStackHighWaterMark); /* bytes on ESP-IDF */
        cJSON_AddItemToArray(tasks, o);
    }
    for (int k = 0; k < portNUM_PROCESSORS; k++) {
        if (idle_pm[k] < 0) cJSON_AddItemToArray(cores, cJSON_CreateNull());
        else cJSON_AddItemToArray(cores, cJSON_CreateNumber((1000 - idle_pm[k]) / 10.0));
    }
    if (got) { /* 0: more tasks than the list holds (some made meanwhile): the earlier sample stays for the next call */
        memcpy(s_prev, cur, n_cur * sizeof *cur);
        s_n_prev = n_cur;
        s_have_prev = true;
        s_prev_us = now;
    }
    xSemaphoreGive(s_mtx);
    free(ts);
    free(cur);
    free(names);

    cJSON *heap = cJSON_AddObjectToObject(r, "heap");
    heap_json(heap, "internal", MALLOC_CAP_INTERNAL);
    heap_json(heap, "psram", MALLOC_CAP_SPIRAM);

    netmgr_eth_rx_t e;
    netmgr_eth_rx_stats(&e);
    if (!e.started) {
        cJSON_AddNullToObject(r, "eth");
    } else {
        cJSON *eo = cJSON_AddObjectToObject(r, "eth");
        cJSON_AddNumberToObject(eo, "unicast", e.unicast);
        cJSON_AddNumberToObject(eo, "multi", e.multi);
        cJSON_AddNumberToObject(eo, "unwanted", e.unwanted);
        cJSON_AddNumberToObject(eo, "rate", e.rate);
        cJSON_AddNumberToObject(eo, "lostBuf", e.lost_buf);
        cJSON_AddNumberToObject(eo, "lostFifo", e.lost_fifo);
        cJSON_AddBoolToObject(eo, "storm", e.storm);
        cJSON_AddNumberToObject(eo, "storms", e.storms);
    }
    return r;
}
