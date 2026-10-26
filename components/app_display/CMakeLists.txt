idf_component_register(SRCS "app_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp esp_display_present
    PRIV_REQUIRES esp-mosaico-bsp esp_lcd log)
