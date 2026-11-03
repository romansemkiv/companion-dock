# rpc_core.c is pure (also built by the host tests); rpc.c is ESP.
idf_component_register(SRCS "rpc_core.c" "rpc.c"
                       INCLUDE_DIRS "include"
                       REQUIRES json esp_common
                       PRIV_REQUIRES freertos log esp_hw_support)
