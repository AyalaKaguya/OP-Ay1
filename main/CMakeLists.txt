idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_display app_tracker app_audio app_synth app_tracker_engine app_settings app_usb_midi app_note_router app_project app_sampler
        esp-mosaico-bsp esp_iris esp_mosaico_app_recovery esp_mosaico_gsp_bundle
        esp_mosaico_gsp_iris nvs_flash log
    EMBED_FILES "assets/aryx.s3m")
