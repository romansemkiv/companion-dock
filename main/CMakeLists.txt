# quickreset_logic.c, svcbtn_logic.c and supervisor_logic.c are pure (also built by the host tests); the rest is ESP.
# The managed components (mdns, esp_hosted, esp_wifi_remote, esp_codec_dev) come in through idf_component.yml.
idf_component_register(SRCS "main.c" "supervisor.c" "svcbtn.c" "quickreset_logic.c" "svcbtn_logic.c" "supervisor_logic.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES boxconfig breadcrumb netmgr discovery satclient usbdeck surfaces imgproc rpc webui slotsync json
                                     feedback nvs_flash esp_netif esp_event esp_timer esp_system esp_app_format
                                     app_update esp_driver_gpio esp_hw_support heap freertos log)
