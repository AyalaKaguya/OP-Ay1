idf_component_register(SRCS "midi_descriptor.c" "midi_packets.c" "usb_midi_host.c"
    INCLUDE_DIRS "include"
    REQUIRES app_note_router
    PRIV_REQUIRES espressif__usb esp_iris freertos esp_timer esp_hw_support esp_system esp_hal_usb)
