# satclient_logic.c is pure (also built by the host tests); satclient.c is ESP.
idf_component_register(SRCS "satclient.c" "satclient_logic.c"
                       INCLUDE_DIRS "include"
                       REQUIRES satproto
                       PRIV_REQUIRES esp_timer lwip log freertos esp_common esp_system vfs)
