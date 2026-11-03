# espressif/mdns is pulled into the build by main/idf_component.yml.
idf_component_register(SRCS "select.c" "discovery.c"
                       INCLUDE_DIRS "include"
                       REQUIRES boxconfig
                       PRIV_REQUIRES espressif__mdns esp_netif esp_timer log freertos)
