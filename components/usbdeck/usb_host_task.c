/* USB host library (hubs on), daemon + client tasks, triage of every new device. Both tasks are on
 * the task watchdog: their event waits are bounded (EVENT_WAIT_MS) so they feed it at least once a
 * second; a hung host library or client reboots the box. */
#include <stdatomic.h>
#include <stdio.h>
#include "breadcrumb/breadcrumb.h"
#include "esp_bit_defs.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usbdeck_policy.h"
#include "usbdeck_priv.h"

#define TAG USBDECK_TAG

#define DAEMON_STACK 4096
#define DAEMON_PRIO 8
#define CLIENT_STACK 4096
#define CLIENT_PRIO 7
#define CLIENT_EVENT_MSGS 16
#define EVENT_WAIT_MS 1000 /* bounded waits: the task watchdog is fed every loop */
#define PENDING_MAX 32
#define HUB_DEPTH_MAX 7

static usb_host_client_handle_t s_client;
static uint8_t s_pending[PENDING_MAX]; /* new device addresses; client task only */
static int s_npending;
static atomic_bool s_started;

usb_host_client_handle_t usbdeck_client(void) { return s_client; }

/* ---- usbdeck service task: deferred IN sweeps, power-on after a bus reset ---- */

#define SVC_STACK 3072
#define SVC_PRIO 6
#define RESET_GONE_WAIT_MS 5000 /* the culprit's DEV_GONE: the hub driver handled the disconnect */
#define RESET_POWER_TRY_MS 100
#define RESET_POWER_MAX_MS 10000 /* the port recovers once every device on it was closed */
#define RESTART_STACK 8192 /* esp_restart and its log lines run on it, not on the 3 KB service task */

static TaskHandle_t s_svc;
static atomic_bool s_resetting, s_reset_gone;
static usb_device_handle_t _Atomic s_reset_dev;
static _Atomic int64_t s_last_enum_us, s_power_on_us; /* NEW_DEV; our last root-port power-on */

bool usbdeck_root_settled(void)
{
    return upol_root_settled(esp_timer_get_time(), atomic_load(&s_power_on_us), atomic_load(&s_last_enum_us));
}

void usbdeck_svc_request(usbdeck_svc_t what)
{
    if (s_svc) xTaskNotify(s_svc, (uint32_t)what, eSetBits);
}

bool usbdeck_bus_resetting(void) { return atomic_load(&s_resetting); }

bool usbdeck_bus_reset(usb_device_handle_t culprit)
{
    bool idle = false;
    if (!s_svc || !atomic_compare_exchange_strong(&s_resetting, &idle, true)) return false;
    atomic_store(&s_reset_gone, false);
    atomic_store(&s_reset_dev, culprit);
    ESP_LOGW(TAG, "resetting the USB port");
    esp_err_t err = usb_host_lib_set_root_port_power(false); /* see the caveat in usbdeck_priv.h */
    if (err != ESP_OK) ESP_LOGW(TAG, "root port power off: %s", esp_err_to_name(err));
    usbdeck_svc_request(USBDECK_SVC_RESET);
    return true;
}

static void usb_restart_task(void *arg)
{
    (void)arg;
    breadcrumb_restart(BC_WHY_USB_POWER);
}

/* Service task. USB must never stay off until someone acts: restart if it can't. */
static void bus_reset_finish(void)
{
    /* Power on only after the disconnect was handled (an early power-on races it, and IDF's hub
     * driver aborts). The culprit's DEV_GONE shows that; if it doesn't come, the culprit had left
     * just before the reset (its DEV_GONE came first), so the disconnect is long over. */
    for (int i = 0; i < RESET_GONE_WAIT_MS / 10 && !atomic_load(&s_reset_gone); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!atomic_load(&s_reset_gone)) ESP_LOGW(TAG, "bus reset: no DEV_GONE for the culprit, powering on anyway");
    esp_err_t err = ESP_FAIL;
    for (int t = 0; t < RESET_POWER_MAX_MS; t += RESET_POWER_TRY_MS) {
        err = usb_host_lib_set_root_port_power(true);
        if (err != ESP_ERR_INVALID_STATE) break; /* still recovering: devices not all closed */
        vTaskDelay(pdMS_TO_TICKS(RESET_POWER_TRY_MS));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB port powered again");
        atomic_store(&s_power_on_us, esp_timer_get_time()); /* before resetting clears: see escalate */
        atomic_store(&s_reset_dev, NULL);
        atomic_store(&s_resetting, false);
        return;
    }
    usbdeck_problem("USB didn't come back after a reset (%s) — restarting the box", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(500)); /* let the log and the problem go out */
    /* s_resetting stays set: no other reset starts meanwhile */
    if (xTaskCreate(usb_restart_task, "usb_restart", RESTART_STACK, NULL, SVC_PRIO, NULL) == pdPASS) return;
    breadcrumb_restart(BC_WHY_USB_POWER); /* no memory for the task: from here */
}

static void svc_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, USBDECK_SVC_SWEEP | USBDECK_SVC_RESET, &bits, portMAX_DELAY);
        if (bits & USBDECK_SVC_RESET) bus_reset_finish();
        if (bits & USBDECK_SVC_SWEEP) hid_raw_sweep_deferred();
    }
}

bool usbdeck_running(void) { return atomic_load(&s_started); }

/* With CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y a missing filter rejects every device.
 * Accept everything; triage happens after enumeration. Runs in the daemon task, must not block. */
static bool enum_filter(const usb_device_desc_t *dd, uint8_t *bConfigurationValue)
{
    (void)bConfigurationValue; /* keep the default configuration (1) */
    ESP_LOGD(TAG, "enumerating %04x:%04x", dd->idVendor, dd->idProduct);
    return true;
}

static void daemon_task(void *arg)
{
    (void)arg;
    if (esp_task_wdt_add(NULL) != ESP_OK) ESP_LOGW(TAG, "usb_daemon: not on the task watchdog");
    for (;;) {
        esp_task_wdt_reset();
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(EVENT_WAIT_MS), &flags);
        /* The client is never deregistered: NO_CLIENTS / ALL_FREE need no action. */
    }
}

/* The one place that picks the controller / root port / PHY. IDF 5.5's host library
 * drives a single root port; a second (full-speed) socket will change only this function. */
static esp_err_t host_install(usbdeck_ctrl_t ctrl)
{
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,     /* library sets up the controller's own PHY (UTMI on HS) */
        .root_port_unpowered = true, /* powered after our client is registered */
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = enum_filter,
        /* FIFO split (overrides CONFIG_USB_HOST_HW_BUFFER_BIAS_*), 896 lines in all.
         * The periodic-OUT bias leaves RX 130 lines (one 512-byte packet + 2 status lines, IDF's IN
         * MPS limit exactly 512): with it the core closed the decks' 512-byte HS interrupt-IN
         * descriptor about every 250 us without a usable transaction (raw HCINT ChHltd only, no
         * XferCompl, qTD 06000200, ~40 000 a second), so no key report ever arrived (bench, Module
         * 32). RX 260 holds two IN packets plus status: the IN then stays queued
         * until the deck has data and key reports arrive (HCINT XferCompl|ChHltd, qTD 06000000).
         * Periodic TX keeps 572 lines (2288 bytes): two 1024-byte interrupt-OUT packets (XL,
         * Original v2, MK.2) still fit. */
        .fifo_settings_custom = {.nptx_fifo_lines = 64, .ptx_fifo_lines = 572, .rx_fifo_lines = 260},
        .peripheral_map = ctrl == USBDECK_CTRL_FS ? BIT1 : BIT0, /* P4: 0 = HS OTG, 1 = FS OTG */
    };
    esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK) return err;
    if (xTaskCreate(daemon_task, "usb_daemon", DAEMON_STACK, NULL, DAEMON_PRIO, NULL) != pdPASS) {
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Runs inside usb_host_client_handle_events: record / flag only, never block on user code. */
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: /* not raised for hubs: their children's are */
        atomic_store(&s_last_enum_us, esp_timer_get_time());
        if (s_npending < PENDING_MAX)
            s_pending[s_npending++] = msg->new_dev.address;
        else
            ESP_LOGE(TAG, "too many new devices at once, address %u ignored", msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        deck_dev_gone(msg->dev_gone.dev_hdl);
        if (atomic_load(&s_resetting) && msg->dev_gone.dev_hdl == atomic_load(&s_reset_dev))
            atomic_store(&s_reset_gone, true);
        break;
    default:
        break;
    }
}

/* Hubs between the device and the root port. Parent hubs are held open by the host library's
 * hub driver while their child is attached; read them right after the child was opened. */
static uint8_t hub_depth(const usb_device_info_t *info, usb_speed_t *parent_speed, char *path, size_t cap)
{
    uint8_t depth = 0, ports[HUB_DEPTH_MAX + 1];
    ports[0] = info->parent.port_num;
    usb_device_handle_t p = info->parent.dev_hdl;
    while (p && depth < HUB_DEPTH_MAX) {
        usb_device_info_t pi;
        if (usb_host_device_info(p, &pi) != ESP_OK) break;
        if (depth == 0) *parent_speed = pi.speed;
        depth++;
        ports[depth] = pi.parent.port_num;
        p = pi.parent.dev_hdl;
    }
    /* Port path from the root: the same deck in the same socket gets the same path, whatever USB
     * address it was given. */
    size_t n = 0;
    path[0] = 0;
    for (int i = depth; i >= 0 && n + 4 < cap; i--)
        n += (size_t)snprintf(path + n, cap - n, i == depth ? "%u" : ".%u", ports[i]);
    return depth;
}

static const char *speed_name(usb_speed_t sp)
{
    return sp == USB_SPEED_HIGH ? "high" : sp == USB_SPEED_FULL ? "full" : "low";
}

/* Returns true when a deck task took ownership of the opened `dev`. */
static bool triage(uint8_t addr, usb_device_handle_t dev)
{
    const usb_device_desc_t *dd = NULL;
    usb_device_info_t info;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK || usb_host_device_info(dev, &info) != ESP_OK)
        return false;
    usb_speed_t parent_speed = USB_SPEED_HIGH;
    char path[USBDECK_PATH_MAX];
    const uint8_t depth = hub_depth(&info, &parent_speed, path, sizeof path);
    ESP_LOGI(TAG, "device %u: %04x:%04x %s-speed, hub depth %u, port %u", addr, dd->idVendor, dd->idProduct,
             speed_name(info.speed), depth, info.parent.port_num);

    /* No transaction translator: a full/low-speed device can't talk through a high-speed hub. */
    if (depth > 0 && info.speed != USB_SPEED_HIGH && parent_speed == USB_SPEED_HIGH) {
        usbdeck_problem("%s", USBDECK_MSG_FS_ON_HUB);
        return false;
    }
    if (dd->idVendor != DECK_VID_ELGATO) return false;
    const deck_model_t *m = deck_model_find(dd->idVendor, dd->idProduct);
    if (!m) {
        usbdeck_problem("Unknown Elgato device 0x%04x", dd->idProduct);
        return false;
    }
    deck_link_t link = {
        .addr = addr,
        .dev = dev,
        .high_speed = info.speed == USB_SPEED_HIGH,
        .hub_depth = depth,
        .port = info.parent.port_num,
    };
    snprintf(link.path, sizeof link.path, "%s", path);
    return deck_dev_spawn(&link, m) == ESP_OK;
}

static void probe(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) return; /* already gone */
    if (!triage(addr, dev)) usb_host_device_close(s_client, dev);
}

static void client_task(void *arg)
{
    (void)arg;
    if (esp_task_wdt_add(NULL) != ESP_OK) ESP_LOGW(TAG, "usb_client: not on the task watchdog");
    for (;;) {
        esp_task_wdt_reset();
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(EVENT_WAIT_MS));
        /* Outside the library callback: open, triage, hand Elgato decks to their own task. */
        for (int i = 0; i < s_npending; i++) probe(s_pending[i]);
        s_npending = 0;
    }
}

esp_err_t usbdeck_start(const usbdeck_cb_t *cb)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s_started)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = deck_dev_init(cb);
    if (err != ESP_OK) return err;

    if (!s_svc && xTaskCreate(svc_task, "usbdeck_svc", SVC_STACK, NULL, SVC_PRIO, &s_svc) != pdPASS)
        return ESP_ERR_NO_MEM;

    err = host_install(USBDECK_CTRL_HS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(err));
        return err;
    }
    const usb_host_client_config_t ccfg = {
        .is_synchronous = false,
        .max_num_event_msg = CLIENT_EVENT_MSGS,
        .async = {.client_event_callback = client_event_cb, .callback_arg = NULL},
    };
    err = usb_host_client_register(&ccfg, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB client register failed: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(client_task, "usb_client", CLIENT_STACK, NULL, CLIENT_PRIO, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    err = usb_host_lib_set_root_port_power(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "root port power on failed: %s", esp_err_to_name(err));
        return err;
    }
    atomic_store(&s_power_on_us, esp_timer_get_time());
    atomic_store(&s_started, true);
    ESP_LOGI(TAG, "USB host started (hubs on, max %d decks)", USBDECK_MAX);
    return ESP_OK;
}
