// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app_project.h"
#include "app_sampler.h"
#include "tracker_model.h"
#include "tracker_settings.h"

#define TRACKER_WORKSPACE_HEADER 78
#define TRACKER_WORKSPACE_ROWS 4
#define TRACKER_WORKSPACE_WAVE_SAMPLES 216
#define TRACKER_WORKSPACE_TYPED_BYTES 25

typedef enum {
    TRACKER_WORKSPACE_HOME = 0,
    TRACKER_WORKSPACE_PROJECTS,
    TRACKER_WORKSPACE_NEW,
    TRACKER_WORKSPACE_IMPORT,
    TRACKER_WORKSPACE_NAME,
    TRACKER_WORKSPACE_SAVE,
    TRACKER_WORKSPACE_SWITCH,
    TRACKER_WORKSPACE_ERROR,
    TRACKER_WORKSPACE_STORAGE,
    TRACKER_WORKSPACE_SETTINGS,
    TRACKER_WORKSPACE_SAMPLER,
    TRACKER_WORKSPACE_LEGACY,
} tracker_workspace_screen_t;

typedef enum {
    TRACKER_SAMPLE_LIBRARY = 0,
    TRACKER_SAMPLE_EDIT,
    TRACKER_SAMPLE_RECORD,
    TRACKER_SAMPLE_ASSIGN,
} tracker_sampler_tab_t;

typedef enum {
    TRACKER_SAMPLE_DRAG_NONE = 0,
    TRACKER_SAMPLE_DRAG_TRIM_START,
    TRACKER_SAMPLE_DRAG_TRIM_END,
    TRACKER_SAMPLE_DRAG_LOOP_START,
    TRACKER_SAMPLE_DRAG_LOOP_END,
} tracker_sampler_drag_t;

/* One GSP UI owner. Snapshots, request storage and waveform scratch are owned
 * here rather than on small callback stacks. Only public service calls cross
 * components; filesystem and capture setup run on the project worker. */
typedef struct {
    app_project_t *project;
    app_sampler_t *sampler;
    tracker_workspace_screen_t screen;
    tracker_workspace_screen_t return_screen;
    tracker_workspace_screen_t name_return;
    app_project_snapshot_t project_view;
    app_project_request_t request;
    app_project_request_t retry;
    app_project_request_t pending_switch;
    app_project_request_t after_save;
    bool switch_after_save;
    bool job_waiting;
    bool job_observed_busy;
    bool snapshot_valid;
    bool dirty;
    bool pointer_down;
    bool moved;
    bool editing_name;
    bool numeric_keys;
    bool import_wav;
    bool show_recent;
    int home_page;
    int start_x;
    int start_y;
    int selected_project;
    int project_first;
    unsigned recent_count;
    char recent_ids[TRACKER_WORKSPACE_ROWS][APP_PROJECT_ID_BYTES];
    char recent_names[TRACKER_WORKSPACE_ROWS][APP_PROJECT_NAME_BYTES];
    int selected_inbox;
    int inbox_first;
    uint64_t session_generation;
    uint64_t waiting_operation;
    bool source_edit_held;
    uint32_t musical_revision;
    app_project_error_t ui_error;
    char typed_name[TRACKER_WORKSPACE_TYPED_BYTES];
    char message[96];

    tracker_sampler_tab_t sampler_tab;
    unsigned sample;
    unsigned sample_first;
    unsigned sample_count;
    unsigned assign_slot;
    bool sample_valid;
    bool sample_edit_active;
    bool sample_edit_dirty;
    bool sampler_pointer;
    bool loop_handles;
    bool auditioning;
    bool mic_monitor;
    float mic_gain;
    tracker_sampler_drag_t sample_drag;
    app_sampler_sample_info_t sample_info;
    app_sampler_sample_info_t library_rows[TRACKER_WORKSPACE_ROWS];
    bool library_valid[TRACKER_WORKSPACE_ROWS];
    app_sampler_edit_t edit;
    app_sampler_edit_t original_edit;
    app_sampler_record_status_t record;
    app_sampler_result_t sampler_error;
    uint64_t sampler_revision;
    int16_t waveform[TRACKER_WORKSPACE_WAVE_SAMPLES];
    unsigned waveform_count;
} tracker_workspace_t;

void tracker_workspace_init(tracker_workspace_t *workspace,
                             app_project_t *project, app_sampler_t *sampler);
void tracker_workspace_poll(tracker_workspace_t *workspace, tracker_model_t *model);
void tracker_workspace_cancel_input(tracker_workspace_t *workspace);
bool tracker_workspace_pointer(tracker_workspace_t *workspace, tracker_model_t *model,
                                 tracker_settings_t *settings, int x, int y,
                                 bool pressed);
void tracker_workspace_render(uint16_t *frame, const tracker_workspace_t *workspace,
                               const tracker_model_t *model);
void tracker_workspace_render_header(uint16_t *frame,
                                      const tracker_workspace_t *workspace,
                                      const tracker_model_t *model);
/* Original tracker page band 40..431 is mapped below the project strip. */
void tracker_workspace_fit_legacy(uint16_t *frame);
int tracker_workspace_legacy_y(int y);
int tracker_workspace_route(const tracker_workspace_t *workspace,
                             const tracker_model_t *model);
bool tracker_workspace_busy(const tracker_workspace_t *workspace);
unsigned tracker_workspace_project_count(const tracker_workspace_t *workspace);
const app_project_entry_t *tracker_workspace_project_entry(const tracker_workspace_t *workspace,
                                                           unsigned row);
bool tracker_workspace_begin_source_edit(tracker_workspace_t *workspace);
void tracker_workspace_end_source_edit(tracker_workspace_t *workspace, bool changed);

void tracker_sampler_ui_poll(tracker_workspace_t *workspace);
void tracker_sampler_ui_cancel(tracker_workspace_t *workspace);
void tracker_sampler_ui_pointer(tracker_workspace_t *workspace, tracker_model_t *model,
                                 int x, int y, bool pressed);
void tracker_sampler_ui_render(uint16_t *frame, const tracker_workspace_t *workspace);
void tracker_workspace_request(tracker_workspace_t *workspace,
                                app_project_action_t action);
void tracker_workspace_message(tracker_workspace_t *workspace, const char *message);
void tracker_workspace_pixel_name(char *out, size_t capacity, const char *input,
                                   unsigned maximum_characters);
