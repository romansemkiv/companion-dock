# cora.c, dockproto.c and ingate.c are pure (also built by the host tests); elgdock.c and elgdock_deck.c are ESP.
# espressif/mdns is pulled into the build by main/idf_component.yml.
idf_component_register(SRCS "cora.c" "dockproto.c" "ingate.c" "elgdock.c" "elgdock_deck.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES deckmodel deckproto usbdeck esp_common
                       PRIV_REQUIRES espressif__mdns lwip nvs_flash esp_timer log freertos heap)
