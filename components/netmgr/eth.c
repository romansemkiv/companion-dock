/* The P4-ETH's Ethernet: the P4's EMAC with the IP101GRI PHY over RMII, the
 * 50 MHz reference clock from the PHY (schematic ESP32-P4-WIFI6-POE-ETH,
 * Espressif's BSP). The MAC address is the chip's own for
 * Ethernet (eFuse).
 *
 * Invariants:
 * - The netif's route_prio (NETMGR_ROUTE_PRIO_ETH, 150) is above the
 *   station's (100): lwIP makes Ethernet the default route whenever it is up
 *   with an address (netmgr.c's header explains how that relates to the
 *   reported route).
 * - The PHY address is found by probing (ESP_ETH_PHY_ADDR_AUTO); the
 *   hostname is set before DHCP can start.
 * - Every received frame goes through the receive filter (rxfilter.h) on the
 *   EMAC's receive task, read in its DMA buffer (the patched esp_eth,
 *   components/esp_eth/COMPANION_DOCK.md): broadcast and multicast the box has
 *   no use for is given back to the DMA there, never copied, allocated or
 *   queued for lwIP. Broadcast it needs (ARP, DHCP) passes, so the box stays
 *   reachable and discoverable through a flood.
 * - Only a flood beyond what the receive task keeps up with (the EMAC losing
 *   frames) is blocked in the EMAC itself (storm.h, every STORM_TICK_MS on
 *   esp_timer's task), so the DMA buffers stay free for Companion's frames. */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "soc/emac_dma_struct.h"
#include "soc/emac_mac_struct.h"
#include "netmgr/rxfilter.h"
#include "netmgr/storm.h"
#include "netmgr_priv.h"

static const char *TAG = "netmgr";

#define PIN_MDC 31
#define PIN_MDIO 52
#define PIN_REF_CLK 50 /* in: from the PHY */
#define PIN_TX_EN 49
#define PIN_TXD0 34
#define PIN_TXD1 35 /* also the BOOT button */
#define PIN_CRS_DV 28
#define PIN_RXD0 29
#define PIN_RXD1 30
#define PIN_PHY_RST 51
#define RX_TASK_STACK 4096
#define SAY_STACK 4096 /* PSRAM: one log line at a time (about 2 KB: the web log's line, vfprintf) */
#define SAY_PRIO 2     /* a convenience (docs/manual.md, Task priorities) */

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static rxf_t s_rxf; /* the EMAC receive task's alone (eth_rx_filter) */
/* since the start, for netmgr_eth_rx_stats: the counts by the receive task, lost_* and the storm's by the storm
 * timer (each field by one task), read anywhere (one word each) */
static volatile netmgr_eth_rx_t s_tot;
static volatile uint32_t s_bcast; /* broadcast frames received (the receive task), for the storm timer */
static storm_t s_storm;           /* the storm timer's alone */
static esp_timer_handle_t s_storm_timer;
static uint32_t s_bcast_seen, s_rep_lost_buf, s_rep_lost_fifo;
/* the filter's report window, handed from the receive task to the storm timer (which adds the EMAC's losses) */
static rxf_stats_t s_rep;
static atomic_bool s_rep_due;
/* What there is to log, handed from the storm timer to a task of its own on core 1 (say_task). A line written to the
 * console holds its task up for milliseconds: on the receive task, or on the timer's (core 0 too, above it), that
 * stops reception, and at a full link's 148 000 frames a second the ring overflows. */
typedef struct {
    bool report, began, ended;
    rxf_stats_t st;       /* report: the window's counts ... */
    uint32_t ring, fifo;  /* ... and what the EMAC lost in it */
    bool blocked;         /* ... while broadcast was blocked */
    uint32_t began_n;     /* began: the broadcast frames in the tick that blocked */
    uint32_t ended_s, peak; /* ended: how long the storm was, its busiest tick */
} eth_say_t;
static eth_say_t s_say;
static portMUX_TYPE s_say_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_say_task;
/* for the filter, kept by the storm timer: a clock good to STORM_TICK_MS and the box's address, so a frame costs no
 * clock read and no netif lookup (one word each: no lock needed) */
static volatile uint32_t s_now_ms, s_own_ip;

static void say_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL(&s_say_mux);
        const eth_say_t say = s_say;
        s_say.report = s_say.began = s_say.ended = false;
        taskEXIT_CRITICAL(&s_say_mux);
        if (say.began)
            ESP_LOGW(TAG, "eth: broadcast storm (%" PRIu32 " frames in %d ms, frames lost): broadcast is blocked in the "
                     "EMAC, unicast and multicast go on", say.began_n, STORM_TICK_MS);
        if (say.report)
            ESP_LOGW(TAG, "eth: last %d s: %" PRIu32 " frames to the box in; dropped %" PRIu32 " broadcast/multicast"
                     " it has no use for, %" PRIu32 " over budget; lost by the EMAC: %" PRIu32 " (no buffer), %" PRIu32
                     " (FIFO)%s", (int)(RXF_REPORT_US / 1000000), say.st.unicast, say.st.unwanted, say.st.rate, say.ring,
                     say.fifo, say.blocked ? "; broadcast blocked (storm)" : "");
        if (say.ended)
            ESP_LOGW(TAG, "eth: broadcast storm over after %" PRIu32 " s (peak %" PRIu32 " frames in %d ms): broadcast "
                     "received again", say.ended_s, say.peak, STORM_TICK_MS);
    }
}

/* Every STORM_TICK_MS (esp_timer's task, core 0: it must stay short, it runs above the receive task): the EMAC's
 * lost-frame counters added up, and broadcast storm control (storm.h) applied to the EMAC's "disable broadcast
 * frames" filter bit. What it has to say goes to say_task. */
static void storm_cb(void *arg)
{
    (void)arg;
    s_now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const struct netif *nif = s_netif ? esp_netif_get_netif_impl(s_netif) : NULL;
    s_own_ip = nif ? ip4_addr_get_u32(netif_ip4_addr(nif)) : 0;
    /* clear on read; read only here */
    const uint32_t m = EMAC_DMA.dmamissedfr.val;
    const uint32_t lost = (m & 0xffff) + (m & 1u << 16 ? 0x10000 : 0);
    s_tot.lost_buf += lost;
    s_tot.lost_fifo += ((m >> 17) & 0x7ff) + (m & 1u << 28 ? 0x800 : 0);

    const uint32_t b = s_bcast, n = b - s_bcast_seen;
    s_bcast_seen = b;
    storm_ev_t ev;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const storm_act_t a = storm_tick(&s_storm, now_ms, n, lost, &ev);
    if (a == STORM_ACT_BLOCK || storm_blocked(&s_storm)) EMAC_MAC.gmacff.dbf = 1; /* again each tick: a MAC reset clears it */
    else if (a == STORM_ACT_OPEN) EMAC_MAC.gmacff.dbf = 0;
    s_tot.storm = storm_blocked(&s_storm);
    s_tot.storms = s_storm.storms;
    bool report = false;
    rxf_stats_t st = {0};
    uint32_t ring = 0, fifo = 0;
    if (atomic_exchange_explicit(&s_rep_due, false, memory_order_acquire)) {
        st = s_rep;
        const uint32_t lb = s_tot.lost_buf, lf = s_tot.lost_fifo;
        ring = lb - s_rep_lost_buf;
        fifo = lf - s_rep_lost_fifo;
        s_rep_lost_buf = lb;
        s_rep_lost_fifo = lf;
        report = rxf_worth_logging(&st) || ring || fifo;
    }
    if (!s_say_task || !(report || ev != STORM_EV_NONE)) return;
    taskENTER_CRITICAL(&s_say_mux);
    if (report) {
        s_say.report = true;
        s_say.st = st;
        s_say.ring = ring;
        s_say.fifo = fifo;
        s_say.blocked = s_tot.storm;
    }
    if (ev == STORM_EV_BEGAN) {
        s_say.began = true;
        s_say.began_n = n;
    } else if (ev == STORM_EV_ENDED) {
        s_say.ended = true;
        s_say.ended_s = (uint32_t)((now_ms - s_storm.began_ms) / 1000);
        s_say.peak = s_storm.peak;
    }
    taskEXIT_CRITICAL(&s_say_mux);
    xTaskNotifyGive(s_say_task);
}

/* The EMAC receive task's filter (the patched esp_eth's esp_eth_mac_esp_set_rx_filter): each received frame, its
 * header read in its DMA buffer, before it is copied out. false: dropped there, no allocation, no copy (a flood
 * costs a few microseconds a frame); true: on to lwIP as before. hdr_len: 16 bytes of a unicast frame, 44 of a
 * broadcast or multicast one (64 with IPv4 options), the filter's whole view: it reads no further than the IPv4 and
 * UDP headers or the ARP packet. */
static bool eth_rx_filter(const uint8_t *buf, uint32_t hdr_len, uint32_t len, void *ctx)
{
    (void)ctx;
    const int64_t now = (int64_t)s_now_ms * 1000; /* the budgets refill by the tick: fine for them */
    rxf_verdict_t v = rxf_check(&s_rxf, buf, hdr_len, s_own_ip, now);
    rxf_stats_t st;
    if (rxf_report(&s_rxf, now, &st)) { /* never logged here: the storm timer passes it on (s_rep) */
        s_rep = st;
        atomic_store_explicit(&s_rep_due, true, memory_order_release);
    }
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (hdr_len >= 6 && memcmp(buf, bcast, 6) == 0) s_bcast++;
    if (v == RXF_DROP_UNWANTED) s_tot.unwanted++;
    else if (v == RXF_DROP_RATE) s_tot.rate++;
    else if (len && (buf[0] & 1)) s_tot.multi++;
    else s_tot.unicast++;
    return v == RXF_PASS;
}

esp_netif_t *netmgr_eth_netif(void) { return s_netif; }

void netmgr_eth_rx_stats(netmgr_eth_rx_t *out)
{
    out->started = s_eth != NULL;
    out->unicast = s_tot.unicast;
    out->multi = s_tot.multi;
    out->unwanted = s_tot.unwanted;
    out->rate = s_tot.rate;
    out->lost_buf = s_tot.lost_buf;
    out->lost_fifo = s_tot.lost_fifo;
    out->storm = s_tot.storm;
    out->storms = s_tot.storms;
}

bool netmgr_eth_lease(char ip[16], char mask[16], char gw[16], char dns[16])
{
    ip[0] = mask[0] = gw[0] = dns[0] = '\0';
    esp_netif_ip_info_t info;
    if (!s_netif || esp_netif_get_ip_info(s_netif, &info) != ESP_OK || info.ip.addr == 0) return false;
    snprintf(ip, 16, IPSTR, IP2STR(&info.ip));
    snprintf(mask, 16, IPSTR, IP2STR(&info.netmask));
    snprintf(gw, 16, IPSTR, IP2STR(&info.gw));
    esp_netif_dns_info_t d;
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &d) == ESP_OK && d.ip.type == ESP_IPADDR_TYPE_V4 &&
        d.ip.u_addr.ip4.addr != 0)
        snprintf(dns, 16, IPSTR, IP2STR(&d.ip.u_addr.ip4));
    return true;
}

static esp_err_t parse_ip(const char *s, esp_ip4_addr_t *out)
{
    return (s && s[0]) ? esp_netif_str_to_ip4(s, out) : ESP_ERR_INVALID_ARG;
}

static esp_err_t use_dhcp(void)
{
    esp_err_t e = esp_netif_dhcpc_start(s_netif);
    if (e == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) e = ESP_OK;
    if (e == ESP_OK) ESP_LOGI(TAG, "eth: DHCP");
    return e;
}

esp_err_t netmgr_eth_apply_ip(const box_config_t *cfg)
{
    if (!s_netif) return ESP_ERR_INVALID_STATE;
    if (cfg->eth_dhcp) return use_dhcp();

    esp_netif_ip_info_t ip = {0};
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4};
    bool has_dns = cfg->eth_dns[0] != '\0';
    if (parse_ip(cfg->eth_ip, &ip.ip) != ESP_OK || parse_ip(cfg->eth_mask, &ip.netmask) != ESP_OK ||
        parse_ip(cfg->eth_gw, &ip.gw) != ESP_OK ||
        (has_dns && parse_ip(cfg->eth_dns, &dns.ip.u_addr.ip4) != ESP_OK)) {
        ESP_LOGW(TAG, "eth: invalid static address, falling back to DHCP");
        return use_dhcp();
    }

    esp_err_t e = esp_netif_dhcpc_stop(s_netif);
    if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return e;
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif, &ip), TAG, "eth: set ip");
    /* After set_ip_info: it clears the DNS servers. */
    if (has_dns) ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns), TAG, "eth: set dns");
    ESP_LOGI(TAG, "eth: static %s/%s gw %s dns %s", cfg->eth_ip, cfg->eth_mask, cfg->eth_gw,
             has_dns ? cfg->eth_dns : "-");
    return ESP_OK;
}

/* The board's EMAC and IP101 PHY driver. */
static esp_err_t eth_driver_new(esp_eth_handle_t *out)
{
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = PIN_MDC;
    emac.smi_gpio.mdio_num = PIN_MDIO;
    emac.interface = EMAC_DATA_INTERFACE_RMII;
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac.clock_config.rmii.clock_gpio = PIN_REF_CLK;
    emac.emac_dataif_gpio.rmii.tx_en_num = PIN_TX_EN;
    emac.emac_dataif_gpio.rmii.txd0_num = PIN_TXD0;
    emac.emac_dataif_gpio.rmii.txd1_num = PIN_TXD1;
    emac.emac_dataif_gpio.rmii.crs_dv_num = PIN_CRS_DV;
    emac.emac_dataif_gpio.rmii.rxd0_num = PIN_RXD0;
    emac.emac_dataif_gpio.rmii.rxd1_num = PIN_RXD1;
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = RX_TASK_STACK;
    /* the receive task on core 0, the core it is created on (app_main's): a flood keeps it busy there, and the decks,
     * Companion, the web server (pinned to core 1) and mDNS and the speaker (core 1) run on the other */
    mac_cfg.flags |= ETH_MAC_FLAG_PIN_TO_CORE;
    if (esp_cpu_get_core_id() != 0) ESP_LOGW(TAG, "eth: the receive task goes to core %d, not 0", esp_cpu_get_core_id());
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = PIN_PHY_RST;
    phy_cfg.phy_addr = ESP_ETH_PHY_ADDR_AUTO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (!mac || !phy) {
        if (mac) mac->del(mac);
        if (phy) phy->del(phy);
        return ESP_FAIL;
    }
    rxf_init(&s_rxf);
    esp_eth_mac_esp_set_rx_filter(mac, eth_rx_filter, NULL); /* before the start: frames are filtered in place */
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t e = esp_eth_driver_install(&cfg, out);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "eth driver (is the IP101 there?): %s", esp_err_to_name(e));
        mac->del(mac);
        phy->del(phy);
        *out = NULL;
    }
    return e;
}

esp_err_t netmgr_eth_start(const box_config_t *cfg, const char *hostname)
{
    if (s_eth) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(eth_driver_new(&s_eth), TAG, "eth driver");

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = NETMGR_ROUTE_PRIO_ETH;
    esp_netif_config_t ncfg = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    s_netif = esp_netif_new(&ncfg);
    if (!s_netif) return ESP_ERR_NO_MEM;
    esp_err_t e = esp_netif_set_hostname(s_netif, hostname);
    if (e != ESP_OK) ESP_LOGW(TAG, "eth: hostname '%s': %s", hostname, esp_err_to_name(e));
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)), TAG, "eth: attach");
    storm_init(&s_storm);
    if (xTaskCreatePinnedToCoreWithCaps(say_task, "eth_log", SAY_STACK, NULL, SAY_PRIO, &s_say_task, 1, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        s_say_task = NULL; /* storm control goes on, unsaid */
        ESP_LOGW(TAG, "eth: no memory for the log task: floods and storms won't show in the log");
    }
    const esp_timer_create_args_t ta = {.callback = storm_cb, .name = "eth_storm"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_storm_timer), TAG, "eth: storm timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_storm_timer, STORM_TICK_MS * 1000), TAG, "eth: storm timer start");
    ESP_RETURN_ON_ERROR(netmgr_eth_apply_ip(cfg), TAG, "eth: ip config");
    return esp_eth_start(s_eth);
}
