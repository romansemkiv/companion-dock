# esp_wifi.h resolves to esp_wifi_remote's injected headers: esp_wifi_remote
# links itself into the esp_wifi target. The managed components are pulled
# into the build by main/idf_component.yml.
idf_component_register(SRCS "policy.c" "scan_logic.c" "wififo_logic.c" "netmgr.c" "eth.c" "wifi.c" "wifi_failover.c" "hosted.c" "captive_dns.c"
                       INCLUDE_DIRS "include"
                       REQUIRES boxconfig esp_event
                       PRIV_REQUIRES esp_eth esp_netif esp_wifi esp_timer esp_hw_support lwip log
                                     espressif__esp_hosted c6fw)
