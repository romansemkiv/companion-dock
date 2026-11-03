idf_component_register(SRCS "usb_host_task.c" "hid_raw.c" "deck_dev.c" "usbdeck_policy.c"
                       INCLUDE_DIRS "include"
                       REQUIRES deckmodel deckproto
                       PRIV_REQUIRES breadcrumb usb log freertos heap esp_rom esp_common esp_system esp_timer)
