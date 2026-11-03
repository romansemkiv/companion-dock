idf_component_register(SRCS config_json.c config_nvs.c led_pins.c
                       INCLUDE_DIRS include
                       REQUIRES json
                       PRIV_REQUIRES nvs_flash esp_hw_support)
