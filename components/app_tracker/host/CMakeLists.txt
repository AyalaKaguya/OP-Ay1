cmake_minimum_required(VERSION 3.20)
project(op_ay1_backend LANGUAGES C)

if(NOT ESP_GSP_COMPONENT_DIR AND DEFINED ENV{ESP_GSP_COMPONENT_DIR})
    set(ESP_GSP_COMPONENT_DIR "$ENV{ESP_GSP_COMPONENT_DIR}")
endif()
if(NOT ESP_GSP_COMPONENT_DIR)
    get_filename_component(ESP_GSP_COMPONENT_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../../managed_components/espressif__esp-gsp" ABSOLUTE)
endif()
if(NOT EXISTS "${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge/CMakeLists.txt")
    message(FATAL_ERROR "Resolve espressif/esp-gsp 1.5.1 before building the native UI")
endif()

set(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
add_subdirectory("${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge" sim_bridge)
gsp_add_backend(op_ay1_backend
    SOURCES
        ../tracker_font.c ../tracker_graphics.c ../tracker_render.c
        ../tracker_pages.c ../tracker_model.c ../tracker_synth.c ../tracker_ui.c
        ../../app_tracker_engine/tracker_engine.c
        ../../app_tracker_engine/tracker_module.c
        ../../app_tracker_engine/tracker_mixer.c
        ../../app_audio/app_audio_bus.c
        ../../app_synth/app_synth.c
        platform_pc.c audio_pc.c backend_main.c
    INCLUDE_DIRECTORIES
        ../include .. ../../app_tracker_engine/include ../../app_audio/include
        ../../app_synth/include
    SCENES ../ui/main.json)

# The standard main closes the bridge before application teardown can report
# a fence failure. This owner performs normal app cleanup before close.
get_target_property(backend_sources op_ay1_backend SOURCES)
list(FILTER backend_sources EXCLUDE REGEX "tools/sim_bridge/src/main\\.c$")
set_property(TARGET op_ay1_backend PROPERTY SOURCES "${backend_sources}")

target_compile_features(op_ay1_backend PRIVATE c_std_11)
target_compile_definitions(op_ay1_backend PRIVATE
    OP_AY1_MODULE_PATH="${APP_ROOT}/main/assets/aryx.s3m")
if(MSVC)
    target_compile_options(op_ay1_backend PRIVATE /W4 /WX /utf-8 /experimental:c11atomics)
    target_compile_definitions(op_ay1_backend PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(op_ay1_backend PRIVATE winmm)
else()
    target_compile_options(op_ay1_backend PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(op_ay1_backend PRIVATE m)
endif()
