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
 *   hostname is set before DHCP can start. */
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_log.h"
#include "esp_netif.h"
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

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;

esp_netif_t *netmgr_eth_netif(void) { return s_netif; }

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
    ESP_RETURN_ON_ERROR(netmgr_eth_apply_ip(cfg), TAG, "eth: ip config");
    return esp_eth_start(s_eth);
}
