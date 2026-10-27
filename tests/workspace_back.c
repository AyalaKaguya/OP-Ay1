// SPDX-License-Identifier: Apache-2.0
/* Hierarchical navigation preserves the musical session and pending capture. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_workspace.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "workspace_back:%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static void tap(tracker_workspace_t *w, tracker_model_t *m,
                tracker_settings_t *s, int x, int y)
{
    CHECK(tracker_workspace_pointer(w, m, s, x, y, true));
    CHECK(tracker_workspace_pointer(w, m, s, x, y, false));
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb"); CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    const long bytes = ftell(file); CHECK(bytes > 0);
    rewind(file);
    uint8_t *source = malloc((size_t)bytes); CHECK(source != NULL);
    CHECK(fread(source, 1, (size_t)bytes, file) == (size_t)bytes);
    CHECK(fclose(file) == 0);
    tracker_engine_t *engine = tracker_engine_create(source, (size_t)bytes);
    app_synth_t *synth = app_synth_create(48000);
    app_sampler_t *sampler = app_sampler_create();
    app_audio_bus_t *bus = app_audio_bus_create(engine, synth);
    CHECK(engine && synth && sampler && bus);
    tracker_workspace_t *w = calloc(1, sizeof(*w));
    tracker_model_t *model = calloc(1, sizeof(*model));
    tracker_settings_t *settings = calloc(1, sizeof(*settings));
    CHECK(w && model && settings);
    tracker_workspace_init(w, NULL, sampler);
    tracker_model_init(model, engine, synth, bus);
    tracker_settings_init(settings);
    CHECK(!tracker_workspace_has_parent(w));
    CHECK(!tracker_workspace_back(w, model));
    tap(w, model, settings, 128, 176); /* HOME -> PROJECTS. */
    CHECK(w->screen == TRACKER_WORKSPACE_PROJECTS && !tracker_workspace_has_parent(w));
    tap(w, model, settings, 210, 110); /* PROJECTS -> IMPORT. */
    CHECK(w->screen == TRACKER_WORKSPACE_IMPORT && tracker_workspace_has_parent(w));
    CHECK(tracker_workspace_back(w, model));
    CHECK(w->screen == TRACKER_WORKSPACE_PROJECTS);
    tap(w, model, settings, 80, 110); /* PROJECTS -> NEW. */
    CHECK(w->screen == TRACKER_WORKSPACE_NEW && tracker_workspace_has_parent(w));
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_PROJECTS);
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_HOME);

    w->home_page = 1;
    w->project_view.opened = true;
    tap(w, model, settings, 128, 334); /* HOME -> SAMPLER. */
    CHECK(w->screen == TRACKER_WORKSPACE_SAMPLER && !tracker_workspace_has_parent(w));
    w->sample_edit_active = w->sample_edit_dirty = true;
    w->record.state = APP_SAMPLER_RECORD_READY;
    tracker_workspace_open_import(w, true);
    CHECK(w->screen == TRACKER_WORKSPACE_IMPORT && w->import_return == TRACKER_WORKSPACE_SAMPLER);
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_SAMPLER);
    CHECK(w->sample_edit_active && w->sample_edit_dirty && w->record.state == APP_SAMPLER_RECORD_READY);
    w->sampler_tab = TRACKER_SAMPLE_ASSIGN;
    CHECK(tracker_workspace_has_parent(w));
    CHECK(tracker_workspace_back(w, model) && w->sampler_tab == TRACKER_SAMPLE_LIBRARY);

    /* Requesting replacement from a dirty nested page exposes its parent
     * through the same public request path as the import buttons. */
    w->record.state = APP_SAMPLER_RECORD_IDLE;
    tracker_workspace_open_import(w, false);
    w->project_view.dirty = true;
    tracker_workspace_request(w, APP_PROJECT_IMPORT_ARYX);
    CHECK(w->screen == TRACKER_WORKSPACE_SWITCH && w->switch_return == TRACKER_WORKSPACE_IMPORT);
    w->switch_after_save = true;
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_IMPORT);
    CHECK(!w->switch_after_save && w->project_view.dirty && w->sample_edit_active);

    w->screen = TRACKER_WORKSPACE_NAME; w->name_return = TRACKER_WORKSPACE_PROJECTS;
    w->editing_name = true; w->switch_after_save = true;
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_PROJECTS);
    CHECK(!w->editing_name && !w->switch_after_save);
    w->screen = TRACKER_WORKSPACE_SAVE; w->return_screen = TRACKER_WORKSPACE_HOME;
    CHECK(tracker_workspace_has_parent(w));
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_HOME);
    w->screen = TRACKER_WORKSPACE_ERROR; w->operation_return = TRACKER_WORKSPACE_IMPORT;
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_IMPORT);
    w->screen = TRACKER_WORKSPACE_STORAGE;
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_SETTINGS);

    /* Back out of an admitted task retracts only its unsubmitted follow-up.
     * The operation marker and draft/take remain owned by their services. */
    w->screen = TRACKER_WORKSPACE_IMPORT; w->import_return = TRACKER_WORKSPACE_SAMPLER;
    w->job_waiting = true; w->waiting_operation = 37; w->switch_after_save = true;
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_SAMPLER);
    CHECK(w->job_waiting && w->job_background && w->waiting_operation == 37 && !w->switch_after_save);
    CHECK(w->sample_edit_active && w->project_view.dirty);
    w->job_waiting = false;
    tracker_engine_play(engine);
    const uint32_t revision = model->edit_revision;
    CHECK(tracker_workspace_back(w, model) && w->screen == TRACKER_WORKSPACE_HOME);
    CHECK(tracker_engine_is_playing(engine) && model->edit_revision == revision && w->sample_edit_active);

    app_audio_bus_destroy(bus); app_sampler_destroy(sampler);
    app_synth_destroy(synth); tracker_engine_destroy(engine);
    free(settings); free(model); free(w); free(source);
    puts("workspace back: actual import origins, nested cancellation, busy IO and session preservation PASS");
    return 0;
}
