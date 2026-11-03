# registry.c, coalesce.c, inputmap.c, message.c, problems.c, dockcache.c and maintmap.c are pure (also built
# by the host tests); surfaces.c, maintui.c (the maintenance screens) and dockmode.c are ESP.
idf_component_register(SRCS "registry.c" "coalesce.c" "inputmap.c" "message.c" "problems.c" "dockcache.c"
                            "maintmap.c" "surfaces.c" "maintui.c" "dockmode.c"
                       INCLUDE_DIRS "include"
                       REQUIRES deckmodel deckproto satproto discovery esp_common
                       PRIV_REQUIRES satclient usbdeck elgdock imgproc statusui deckui netmgr slotsync feedback freertos
                                     log esp_timer heap esp_system app_update esp_app_format)
