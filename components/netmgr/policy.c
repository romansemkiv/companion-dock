#include "netmgr/netmgr.h"

#include <stdio.h>
#include <string.h>

void netpolicy_ap_ip(bool ap_on, char out[16])
{
    static const int oct[4] = {NETMGR_AP_OCTETS};
    if (ap_on) snprintf(out, 16, "%d.%d.%d.%d", oct[0], oct[1], oct[2], oct[3]);
    else out[0] = '\0';
}

static void if_addr(bool has, const char *ip, char out[16])
{
    snprintf(out, 16, "%s", has && ip && ip[0] && strcmp(ip, "0.0.0.0") ? ip : "");
}

void netpolicy_addrs(net_route_t route, bool eth_ip, const char *eth, bool sta_connected, bool sta_ip, const char *sta,
                     char ip[16], char eth_addr[16], char sta_addr[16])
{
    const char *r = route == NET_ROUTE_ETH ? eth : route == NET_ROUTE_WIFI ? sta : NULL;
    snprintf(ip, 16, "%s", r ? r : "");
    if_addr(eth_ip, eth, eth_addr);
    if_addr(sta_connected && sta_ip, sta, sta_addr);
}

net_route_t netpolicy_route(const net_state_t *s)
{
    if (s->eth_link && s->eth_ip) return NET_ROUTE_ETH;
    if (s->wifi_configured && s->wifi_ip) return NET_ROUTE_WIFI;
    return NET_ROUTE_NONE;
}

bool netpolicy_wifi_changed(const box_config_t *a, const box_config_t *b)
{
    if (a->ap_secured != b->ap_secured) return true;
    if (a->ap_secured && strcmp(a->ap_pass, b->ap_pass)) return true;
    return netpolicy_sta_changed(a, b);
}

bool netpolicy_sta_changed(const box_config_t *a, const box_config_t *b)
{
    return a->wifi_enabled != b->wifi_enabled || strcmp(a->wifi_ssid, b->wifi_ssid) || strcmp(a->wifi_pass, b->wifi_pass) ||
           strcmp(a->wifi_backup_ssid, b->wifi_backup_ssid) || strcmp(a->wifi_backup_pass, b->wifi_backup_pass);
}

bool netpolicy_sta_on(const box_config_t *c) { return c->wifi_enabled && c->wifi_ssid[0] != '\0'; }
