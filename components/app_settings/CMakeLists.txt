idf_component_register(
    SRCS "app_settings.c" "app_settings_device.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp esp_display_present
    PRIV_REQUIRES esp-mosaico-bsp esp_timer nvs_flash log)
