idf_component_register(
    SRCS "app_audio.c" "app_audio_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES app_tracker_engine app_synth
    PRIV_REQUIRES esp-mosaico-bsp espressif__esp_codec_dev log esp_timer)
