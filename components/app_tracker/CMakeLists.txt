idf_component_register(
    SRCS "tracker_font.c" "tracker_graphics.c" "tracker_render.c"
         "tracker_pages.c" "tracker_synth.c" "tracker_model.c" "tracker_settings.c"
         "tracker_ui.c" "tracker_device.c" "tracker_transition.c"
         "tracker_workspace.c" "tracker_workspace_render.c" "tracker_sampler_ui.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp-gsp app_tracker_engine app_audio app_synth app_settings app_usb_midi app_note_router app_project app_sampler
    PRIV_REQUIRES esp-mosaico-bsp esp_iris heap log esp_timer)

include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "ui/main.json")
