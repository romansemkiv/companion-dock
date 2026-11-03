# breadcrumb_logic.c is pure (also built by the host tests); breadcrumb.c is ESP.
idf_component_register(SRCS "breadcrumb_logic.c" "breadcrumb.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_system esp_hw_support freertos log soc)
