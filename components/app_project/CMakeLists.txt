idf_component_register(SRCS "app_project.c" "project_platform.c" "project_files.c" "project_session.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES app_tracker_engine app_audio app_synth app_sampler app_note_router
    PRIV_REQUIRES app_nand esp_timer)
