idf_component_register(
    SRCS "app_sampler.c" "sampler_bank.c" "sampler_formats.c" "sampler_record.c"
    INCLUDE_DIRS "include"
    REQUIRES app_tracker_engine
    PRIV_REQUIRES esp-mosaico-bsp espressif__esp_codec_dev esp_timer)
