# unit_logic.c is pure (also built by the host tests); unitrec.c reads the sector.
idf_component_register(SRCS "unit_logic.c" "unitrec.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES json spi_flash esp_hw_support log)
