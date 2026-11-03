# fbstatus.c, fbsound.c, tones.c and tonegen.c are pure (also built by the host tests); feedback.c,
# speaker.c and led.c are ESP. esp_codec_dev (the ES8311) comes in through main/idf_component.yml.
idf_component_register(SRCS "fbstatus.c" "fbsound.c" "tones.c" "tonegen.c" "feedback.c" "speaker.c" "led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES boxconfig esp_common
                       PRIV_REQUIRES espressif__esp_codec_dev esp_driver_i2c esp_driver_i2s esp_driver_rmt
                                     esp_driver_gpio esp_timer freertos log heap)
