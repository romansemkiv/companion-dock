idf_component_register(SRCS "screen.c" "plan.c" "align.c" "text.c" "raster.c" "icons.c" "phosphor.c" "draw.c" "qr.c"
                            "screens.c" "nav.c" "fp.c" "pad.c" "pin.c" "ipedit.c" "qrcodegen/qrcodegen.c"
                       INCLUDE_DIRS "include"
                       REQUIRES statusui deckmodel
                       PRIV_REQUIRES rpc)
