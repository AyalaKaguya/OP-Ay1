idf_component_register(
    SRCS "tracker_font.c" "tracker_render.c" "tracker_ui.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp-gsp tracker_engine
    PRIV_REQUIRES app_audio esp-mosaico-bsp heap log)

include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "ui/tracker.json")
