# slotsync_logic.c is pure (also built by the host tests); slotsync.c is ESP.
idf_component_register(SRCS "slotsync.c" "slotsync_logic.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES app_update bootloader_support esp_app_format esp_partition esp_timer heap
                                     freertos log breadcrumb rpc)
