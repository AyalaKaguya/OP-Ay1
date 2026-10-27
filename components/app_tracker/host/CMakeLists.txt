cmake_minimum_required(VERSION 3.20)
# Keep the diagnostic language stable between detection and compilation. The
# installed compiler can fall back to another language when English is absent.
if(CMAKE_HOST_WIN32)
    set(ENV{VSLANG} 1033)
endif()
project(op_ay1_backend LANGUAGES C)

if(MSVC)
    # CMake's AUTO decoding uses the legacy Windows code page here, while the
    # compiler writes UTF-8. Probe the actual prefix using the correct encoding;
    # a mismatched prefix makes Ninja silently record zero header dependencies.
    set(opay_showdir "${CMAKE_BINARY_DIR}/CMakeFiles/OpayShowIncludes")
    file(WRITE "${opay_showdir}/opay_includes.h" "\n")
    file(WRITE "${opay_showdir}/main.c" "#include \"opay_includes.h\"\n")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" /nologo /showIncludes /c main.c
        WORKING_DIRECTORY "${opay_showdir}" OUTPUT_VARIABLE opay_includes
        ERROR_VARIABLE opay_includes_error RESULT_VARIABLE opay_includes_result
        ENCODING UTF-8)
    if(opay_includes_result EQUAL 0 AND
       "${opay_includes}" MATCHES "(^|\n)([^:\n][^:\n]+:[^:\n]*[^: \n][^: \n]:?[ \t]+)([A-Za-z]:\\\\|\\./|\\.\\\\|/)")
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    else()
        message(FATAL_ERROR "Cannot detect the native MSVC header-dependency prefix")
    endif()
    set(opay_native_launcher "${CMAKE_COMMAND}" -E env VSLANG=1033)
    if(CMAKE_C_COMPILER_LAUNCHER)
        list(APPEND opay_native_launcher ${CMAKE_C_COMPILER_LAUNCHER})
    endif()
    set(CMAKE_C_COMPILER_LAUNCHER ${opay_native_launcher})
endif()

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
        ../tracker_pages.c ../tracker_model.c ../tracker_synth.c ../tracker_ui.c ../tracker_settings.c ../tracker_transition.c
        ../tracker_workspace.c ../tracker_workspace_render.c ../tracker_sampler_ui.c
        ../../app_tracker_engine/tracker_engine.c
        ../../app_tracker_engine/tracker_module.c
        ../../app_tracker_engine/tracker_mixer.c
        ../../app_audio/app_audio_bus.c
        ../../app_synth/app_synth.c
        ../../app_note_router/app_note_router.c
        ../../app_usb_midi/usb_midi_native.c
        ../../app_usb_midi/midi_packets.c
        ../../app_usb_midi/midi_descriptor.c
        ../../app_settings/app_settings.c
        ../../app_sampler/app_sampler.c
        ../../app_sampler/sampler_bank.c
        ../../app_sampler/sampler_formats.c
        ../../app_sampler/sampler_record.c
        ../../app_project/app_project.c
        ../../app_project/project_platform.c
        ../../app_project/project_files.c
        ../../app_project/project_session.c
        platform_pc.c audio_pc.c backend_main.c
    INCLUDE_DIRECTORIES
        ../include .. ../../app_tracker_engine/include ../../app_audio/include
        ../../app_synth/include ../../app_note_router/include ../../app_usb_midi/include ../../app_settings/include
        ../../app_sampler/include ../../app_project/include
    SCENES ../ui/main.json)

# The standard main closes the bridge before application teardown can report
# a fence failure. This owner performs normal app cleanup before close.
get_target_property(backend_sources op_ay1_backend SOURCES)
list(FILTER backend_sources EXCLUDE REGEX "tools/sim_bridge/src/main\\.c$")
set_property(TARGET op_ay1_backend PROPERTY SOURCES "${backend_sources}")

target_compile_features(op_ay1_backend PRIVATE c_std_11)
target_compile_definitions(op_ay1_backend PRIVATE
    OP_AY1_MODULE_PATH="${APP_ROOT}/main/assets/aryx.s3m")
target_compile_definitions(op_ay1_backend PRIVATE
    OP_AY1_PROJECT_ROOT_DEFAULT="${APP_ROOT}/build-sim/project-data")
if(MSVC)
    target_compile_options(op_ay1_backend PRIVATE /W4 /WX /utf-8 /experimental:c11atomics)
    target_compile_definitions(op_ay1_backend PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(op_ay1_backend PRIVATE winmm)
else()
    target_compile_options(op_ay1_backend PRIVATE -Wall -Wextra -Werror)
    find_package(Threads REQUIRED)
    target_link_libraries(op_ay1_backend PRIVATE m Threads::Threads)
endif()
