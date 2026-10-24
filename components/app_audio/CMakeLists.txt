idf_component_register(
    SRCS "app_audio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp-mosaico-bsp espressif__esp_codec_dev tracker_engine log esp_timer)
