# unit_logic.c is pure (also built by the host tests).
idf_component_register(SRCS "unit_logic.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES json)
