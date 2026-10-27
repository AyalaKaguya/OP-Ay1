// SPDX-License-Identifier: Apache-2.0
#include "tracker_workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tracker_graphics.h"

#define WORKSPACE_TAP_SLOP 12
#define WORKSPACE_SWIPE_DISTANCE 64

static bool inside(int x, int y, int left, int top, int width, int height)
{
    return x >= left && x < left + width && y >= top && y < top + height;
}

static void copy_string(char *out, size_t capacity, const char *input)
{
    if (capacity == 0) {
        return;
    }
    snprintf(out, capacity, "%s", input ? input : "");
}

void tracker_workspace_pixel_name(char *out, size_t capacity, const char *input,
                                  unsigned maximum_characters)
{
    if (capacity == 0) {
        return;
    }
    unsigned count = 0;
    const unsigned char *cursor = (const unsigned char *)(input ? input : "");
    while (*cursor && count + 1U < capacity && count < maximum_characters) {
        unsigned char value = *cursor++;
        if (value >= 0x80U) {
            while ((*cursor & 0xc0U) == 0x80U) {
                ++cursor;
            }
            value = '?'; /* Stored UTF-8 is retained; pixel labels never invent transliterations. */
        }
        if (value >= 'a' && value <= 'z') {
            value = (unsigned char)(value - 'a' + 'A');
        }
        out[count++] = value >= 32 && value <= 126 ? (char)value : '?';
    }
    out[count] = '\0';
    if (*cursor && count >= 3U) {
        memcpy(out + count - 3U, "...", 3U);
    }
}

void tracker_workspace_message(tracker_workspace_t *w, const char *message)
{
    copy_string(w->message, sizeof(w->message), message);
    w->dirty = true;
}

bool tracker_workspace_busy(const tracker_workspace_t *w)
{
    return w->job_waiting || (w->snapshot_valid && w->project_view.busy);
}

unsigned tracker_workspace_project_count(const tracker_workspace_t *w)
{
    return w->show_recent ? w->recent_count : w->project_view.entry_count < APP_PROJECT_LIST_MAX ?
           w->project_view.entry_count : APP_PROJECT_LIST_MAX;
}

const app_project_entry_t *tracker_workspace_project_entry(const tracker_workspace_t *w, unsigned row)
{
    if (!w->show_recent)
        return row < w->project_view.entry_count && row < APP_PROJECT_LIST_MAX ?
               &w->project_view.entries[row] : NULL;
    if (row >= w->recent_count) {
        return NULL;
    }
    for (unsigned i = 0; i < w->project_view.entry_count && i < APP_PROJECT_LIST_MAX; ++i)
        if (strcmp(w->project_view.entries[i].id, w->recent_ids[row]) == 0) {
            return &w->project_view.entries[i];
        }
    return NULL;
}

static void remember_recent(tracker_workspace_t *w)
{
    if (!w->project_view.id[0]) {
        return;
    }
    unsigned found = w->recent_count;
    for (unsigned i = 0; i < w->recent_count; ++i)
        if (strcmp(w->recent_ids[i], w->project_view.id) == 0) {
            found = i;
            break;
        }
    const bool added = found == w->recent_count;
    if (found >= TRACKER_WORKSPACE_ROWS) {
        found = TRACKER_WORKSPACE_ROWS - 1U;
    }
    for (unsigned i = found; i > 0; --i) {
        memcpy(w->recent_ids[i], w->recent_ids[i - 1U], APP_PROJECT_ID_BYTES);
        memcpy(w->recent_names[i], w->recent_names[i - 1U], APP_PROJECT_NAME_BYTES);
    }
    copy_string(w->recent_ids[0], APP_PROJECT_ID_BYTES, w->project_view.id);
    copy_string(w->recent_names[0], APP_PROJECT_NAME_BYTES, w->project_view.name);
    if (added && w->recent_count < TRACKER_WORKSPACE_ROWS) {
        ++w->recent_count;
    }
}

bool tracker_workspace_begin_source_edit(tracker_workspace_t *w)
{
    if (tracker_workspace_busy(w)) {
        return false;
    }
    if (w->project && !app_project_begin_edit(w->project)) {
        return false;
    }
    w->source_edit_held = w->project != NULL;
    return true;
}

void tracker_workspace_end_source_edit(tracker_workspace_t *w, bool changed)
{
    if (w->source_edit_held) {
        app_project_end_edit(w->project, changed);
        w->source_edit_held = false;
    }
}

static bool recording_pending(const tracker_workspace_t *w)
{
    return w->record.state == APP_SAMPLER_RECORD_RECORDING ||
           w->record.state == APP_SAMPLER_RECORD_STOPPING ||
           w->record.state == APP_SAMPLER_RECORD_READY ||
           w->record.state == APP_SAMPLER_RECORD_ERROR;
}

static void show(tracker_workspace_t *w, tracker_workspace_screen_t screen)
{
    w->screen = screen;
    w->dirty = true;
}

static bool replaces_session(app_project_action_t action)
{
    return action == APP_PROJECT_NEW_BLANK || action == APP_PROJECT_IMPORT_ARYX ||
           action == APP_PROJECT_OPEN || action == APP_PROJECT_IMPORT_MODULE;
}

static bool request_ready(tracker_workspace_t *w)
{
    if (!w->project) {
        w->ui_error = APP_PROJECT_STORAGE_UNAVAILABLE;
        tracker_workspace_message(w, "PROJECT SERVICE UNAVAILABLE");
        return false;
    }
    if (tracker_workspace_busy(w)) {
        tracker_workspace_message(w, "PROJECT JOB IN PROGRESS");
        return false;
    }
    return true;
}

static void submit_request(tracker_workspace_t *w)
{
    if (!request_ready(w)) {
        return;
    }
    w->retry = w->request;
    w->waiting_operation = w->project_view.operation_generation;
    const app_project_error_t result = app_project_request(w->project, &w->request);
    w->ui_error = result;
    if (result != APP_PROJECT_OK) {
        tracker_workspace_message(w, app_project_error_name(result));
        if (result != APP_PROJECT_BUSY) {
            show(w, TRACKER_WORKSPACE_ERROR);
        }
        return;
    }
    w->job_waiting = true;
    w->job_observed_busy = false;
    w->job_background = false;
    w->dirty = true;
    tracker_workspace_message(w, "");
}

void tracker_workspace_request(tracker_workspace_t *w, app_project_action_t action)
{
    w->request.action = action;
    if (replaces_session(action)) {
        const bool same = action == APP_PROJECT_OPEN && w->project_view.id[0] &&
                          strcmp(w->request.id, w->project_view.id) == 0;
        if (same) {
            remember_recent(w);
            tracker_workspace_message(w, "");
            show(w, TRACKER_WORKSPACE_HOME);
            return;
        }
        if (recording_pending(w)) {
            tracker_workspace_message(w, "TAKE PENDING");
            w->sampler_tab = TRACKER_SAMPLE_RECORD;
            show(w, TRACKER_WORKSPACE_SAMPLER);
            return;
        }
        /* Leave an unapplied edit/audition intact while the worker prepares a
         * candidate. Only a successful bank commit clears that preview. */
        if (w->project_view.dirty && !w->request.discard_dirty) {
            w->pending_switch = w->request;
            show(w, TRACKER_WORKSPACE_SWITCH);
            return;
        }
    }
    submit_request(w);
}

void tracker_workspace_init(tracker_workspace_t *w,
                            app_project_t *project, app_sampler_t *sampler)
{
    memset(w, 0, sizeof(*w));
    w->project = project;
    w->sampler = sampler;
    w->screen = TRACKER_WORKSPACE_HOME;
    w->return_screen = TRACKER_WORKSPACE_HOME;
    w->assign_slot = 1;
    w->session_generation = UINT64_MAX;
    w->sampler_error = APP_SAMPLER_OK;
    w->dirty = true;
    tracker_workspace_message(w, project ? "" :
                              "PROJECT SERVICE UNAVAILABLE");
}

static void completed_job(tracker_workspace_t *w)
{
    const bool background = w->job_background;
    const tracker_workspace_screen_t current_screen = w->screen;
    w->job_background = false;
    w->job_waiting = false;
    w->ui_error = w->project_view.error;
    if (w->ui_error != APP_PROJECT_OK) {
        tracker_workspace_message(w, w->project_view.message[0] ?
                                  w->project_view.message : app_project_error_name(w->ui_error));
        show(w, TRACKER_WORKSPACE_ERROR);
        return;
    }
    tracker_workspace_message(w, "");
    w->sampler_error = APP_SAMPLER_OK;
    if (replaces_session(w->retry.action) || w->retry.action == APP_PROJECT_SAVE_AS) {
        remember_recent(w);
    }
    if (w->switch_after_save &&
            (w->retry.action == APP_PROJECT_SAVE || w->retry.action == APP_PROJECT_SAVE_AS)) {
        w->switch_after_save = false;
        w->request = w->after_save;
        w->request.discard_dirty = true;
        tracker_workspace_request(w, w->request.action);
        return;
    }
    if (replaces_session(w->retry.action)) {
        show(w, TRACKER_WORKSPACE_HOME);
    } else if (w->retry.action == APP_PROJECT_SAVE || w->retry.action == APP_PROJECT_SAVE_AS) {
        show(w, w->return_screen);
    } else if (w->retry.action == APP_PROJECT_IMPORT_SAMPLE) {
        w->sampler_tab = TRACKER_SAMPLE_LIBRARY;
        const unsigned count = w->sampler ? app_sampler_count(w->sampler) : 0;
        w->sample = count ? count - 1U : 0U;
        tracker_sampler_ui_reveal_sample(w);
        show(w, TRACKER_WORKSPACE_SAMPLER);
    } else if (w->retry.action == APP_PROJECT_ASSIGN_SAMPLE) {
        w->sampler_tab = TRACKER_SAMPLE_LIBRARY;
        show(w, TRACKER_WORKSPACE_SAMPLER);
    } else if (w->retry.action == APP_PROJECT_RECORD_START) {
        w->sampler_tab = TRACKER_SAMPLE_RECORD;
        show(w, TRACKER_WORKSPACE_SAMPLER);
    } else if (w->retry.action == APP_PROJECT_RECORD_KEEP) {
        w->sampler_tab = TRACKER_SAMPLE_LIBRARY;
        const unsigned count = w->sampler ? app_sampler_count(w->sampler) : 0;
        w->sample = count ? count - 1U : 0U;
        tracker_sampler_ui_reveal_sample(w);
        w->auditioning = false;
        show(w, TRACKER_WORKSPACE_SAMPLER);
    } else if (w->retry.action == APP_PROJECT_APPLY_SAMPLE ||
               w->retry.action == APP_PROJECT_UNDO_SAMPLE) {
        w->sample_edit_active = w->sample_edit_dirty = false;
        w->auditioning = false;
        w->sampler_tab = TRACKER_SAMPLE_EDIT;
        show(w, TRACKER_WORKSPACE_SAMPLER);
    }
    if (background) {
        show(w, current_screen);
    }
}

void tracker_workspace_poll(tracker_workspace_t *w, tracker_model_t *model)
{
    if (w->project) {
        const uint64_t before = w->project_view.revision;
        const bool was_busy = w->project_view.busy;
        const bool was_dirty = w->project_view.dirty;
        if (app_project_read(w->project, &w->project_view) == APP_PROJECT_OK) {
            w->snapshot_valid = true;
            if (was_busy != w->project_view.busy || was_dirty != w->project_view.dirty ||
                    before != w->project_view.revision) {
                w->dirty = true;
            }
            if (w->job_waiting && w->project_view.busy) {
                w->job_observed_busy = true;
            }
            if (w->project_view.session_generation != w->session_generation &&
                    !w->project_view.busy && tracker_workspace_begin_source_edit(w)) {
                tracker_model_sync_sources(model);
                model->view.volume = w->project_view.master_volume;
                w->musical_revision = model->edit_revision;
                w->session_generation = w->project_view.session_generation;
                w->sample = w->sample_first = 0;
                tracker_sampler_ui_reveal_sample(w);
                w->sample_edit_active = w->sample_edit_dirty = false;
                w->auditioning = false;
                tracker_workspace_end_source_edit(w, false);
                w->dirty = true;
            }
            if (w->job_waiting && !w->project_view.busy &&
                    (w->job_observed_busy || w->project_view.operation_generation != w->waiting_operation)) {
                completed_job(w);
            }
            const unsigned rows = tracker_workspace_project_count(w);
            if (w->selected_project >= (int)rows) {
                w->selected_project = rows ? (int)rows - 1 : 0;
            }
            if (w->project_first > w->selected_project) {
                w->project_first = (w->selected_project / TRACKER_WORKSPACE_ROWS) * TRACKER_WORKSPACE_ROWS;
            }
        }
    }
    if (!tracker_workspace_busy(w)) {
        tracker_sampler_ui_poll(w);
    }
}

void tracker_workspace_cancel_input(tracker_workspace_t *w)
{
    w->pointer_down = false;
    w->moved = false;
    w->sample_drag = TRACKER_SAMPLE_DRAG_NONE;
    w->sampler_pointer = false;
    w->library_list_pointer = false;
    w->library_scrolling = false;
    w->dirty = true;
}

static void request_replace(tracker_workspace_t *w, app_project_action_t action,
                            const char *name, const char *id, const char *filename)
{
    memset(&w->request, 0, sizeof(w->request));
    copy_string(w->request.name, sizeof(w->request.name), name);
    copy_string(w->request.id, sizeof(w->request.id), id);
    copy_string(w->request.filename, sizeof(w->request.filename), filename);
    tracker_workspace_request(w, action);
}

static void start_save(tracker_workspace_t *w, bool copy)
{
    if (!request_ready(w)) {
        return;
    }
    if (!w->project_view.opened) {
        tracker_workspace_message(w, "NO PROJECT");
        show(w, TRACKER_WORKSPACE_PROJECTS);
        return;
    }
    if (recording_pending(w)) {
        tracker_workspace_message(w, "TAKE PENDING");
        w->sampler_tab = TRACKER_SAMPLE_RECORD;
        show(w, TRACKER_WORKSPACE_SAMPLER);
        return;
    }
    w->return_screen = w->screen;
    if (copy || !w->project_view.id[0]) {
        tracker_workspace_pixel_name(w->typed_name, sizeof(w->typed_name),
                                     w->project_view.name, 20);
        if (!w->typed_name[0]) {
            copy_string(w->typed_name, sizeof(w->typed_name), "UNTITLED");
        }
        w->name_return = w->screen;
        w->editing_name = true;
        show(w, TRACKER_WORKSPACE_NAME);
    } else if (w->project_view.dirty) {
        show(w, TRACKER_WORKSPACE_SAVE);
    } else {
        tracker_workspace_message(w, "");
    }
}

static void page_home(tracker_workspace_t *w, tracker_model_t *model, int page)
{
    tracker_model_cancel_input(model);
    if (w->sampler) {
        app_sampler_audition_stop(w->sampler);
    }
    w->auditioning = false;
    w->home_page = (page + 3) % 3;
    show(w, TRACKER_WORKSPACE_HOME);
}

void tracker_workspace_home(tracker_workspace_t *w, tracker_model_t *model)
{
    page_home(w, model, w->home_page);
    /* An admitted save still completes, without pulling the user back into
     * the screen they just left when its asynchronous completion arrives. */
    w->return_screen = TRACKER_WORKSPACE_HOME;
    w->job_background = tracker_workspace_busy(w);
}

void tracker_workspace_swipe_home(tracker_workspace_t *w, tracker_model_t *model,
                                  int direction)
{
    page_home(w, model, w->home_page + (direction < 0 ? 2 : 1));
}

static void home_open(tracker_workspace_t *w, tracker_model_t *model, int index)
{
    static const int modules[] = {-1, -2, TRACKER_PAGE_PLAYBACK, TRACKER_PAGE_MIXER,
                                  TRACKER_PAGE_PADS, TRACKER_PAGE_SYNTH, -3,
                                  TRACKER_PAGE_PATTERN, TRACKER_PAGE_WAVES
                                 };
    if (index < 0 || index >= (int)(sizeof(modules) / sizeof(modules[0]))) {
        return;
    }
    const int destination = modules[index];
    if (destination == -1) {
        show(w, TRACKER_WORKSPACE_PROJECTS);
    } else if (destination == -2) {
        show(w, TRACKER_WORKSPACE_SETTINGS);
    } else if (!w->project_view.opened && w->project) {
        tracker_workspace_message(w, "NO PROJECT");
        show(w, TRACKER_WORKSPACE_PROJECTS);
    } else if (destination == -3) {
        show(w, TRACKER_WORKSPACE_SAMPLER);
    } else {
        tracker_model_set_page(model, destination);
        show(w, TRACKER_WORKSPACE_LEGACY);
    }
}

static int inbox_at(const tracker_workspace_t *w, int filtered)
{
    for (uint32_t i = 0; i < w->project_view.inbox_count && i < APP_PROJECT_LIST_MAX; ++i) {
        const bool wav = w->project_view.inbox[i].kind == APP_PROJECT_INBOX_SAMPLE;
        if (wav == w->import_wav && filtered-- == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void name_key(tracker_workspace_t *w, int x, int y)
{
    static const char *const letters[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const char *const numbers[] = {"1234567890", "-_.", ""};
    if (y >= 196 && y < 337) {
        const int row = (y - 196) / 47;
        const int column = (x - 24) / 43;
        const char *keys = w->numeric_keys ? numbers[row] : letters[row];
        const size_t length = strlen(w->typed_name);
        if (x >= 24 && row < 3 && column >= 0 && column < (int)strlen(keys) &&
                (x - 24) % 43 < 38 && length + 1U < sizeof(w->typed_name)) {
            w->typed_name[length] = keys[column];
            w->typed_name[length + 1U] = '\0';
        } else if (row == 2 && x >= 325 && length) {
            w->typed_name[length - 1U] = '\0';
        }
    } else if (inside(x, y, 24, 342, 100, 28)) {
        w->numeric_keys = !w->numeric_keys;
    } else if (inside(x, y, 135, 342, 190, 28)) {
        const size_t length = strlen(w->typed_name);
        if (length + 1U < sizeof(w->typed_name)) {
            w->typed_name[length] = ' ';
            w->typed_name[length + 1U] = '\0';
        }
    }
    w->dirty = true;
}

static void tap(tracker_workspace_t *w, tracker_model_t *model,
                tracker_settings_t *settings, int x, int y)
{
    if (w->screen == TRACKER_WORKSPACE_HOME && y >= 432) {
        return;
    }
    if (w->screen == TRACKER_WORKSPACE_SETTINGS && inside(x, y, 336, 0, 144, 40)) {
        show(w, TRACKER_WORKSPACE_STORAGE);
        return;
    }
    if (w->screen == TRACKER_WORKSPACE_HOME && inside(x, y, 420, 0, 60, 40)) {
        show(w, TRACKER_WORKSPACE_PROJECTS);
        return;
    }
    if (w->screen == TRACKER_WORKSPACE_HOME && inside(x, y, 16, 40, 328, 38)) {
        show(w, TRACKER_WORKSPACE_PROJECTS);
        return;
    }
    if (w->screen == TRACKER_WORKSPACE_HOME && inside(x, y, 348, 40, 132, 38)) {
        start_save(w, false);
        return;
    }
    if (tracker_workspace_busy(w)) {
        return;
    }
    if (w->screen == TRACKER_WORKSPACE_HOME) {
        const int column = x < 240 ? 0 : 1;
        const int row = y < 256 ? 0 : 1;
        if (inside(x, y, column ? 248 : 24, row ? 262 : 96, 208, 152)) {
            home_open(w, model, w->home_page * 4 + row * 2 + column);
        }
    } else if (w->screen == TRACKER_WORKSPACE_PROJECTS) {
        if (inside(x, y, 24, 92, 124, 36)) {
            show(w, TRACKER_WORKSPACE_NEW);
        } else if (inside(x, y, 156, 92, 124, 36)) {
            w->import_wav = false;
            show(w, TRACKER_WORKSPACE_IMPORT);
        } else if (inside(x, y, 288, 92, 80, 36)) {
            w->show_recent = !w->show_recent;
            w->selected_project = w->project_first = 0;
            w->dirty = true;
        } else if (inside(x, y, 376, 92, 80, 36)) {
            memset(&w->request, 0, sizeof(w->request));
            tracker_workspace_request(w, APP_PROJECT_REFRESH);
        } else if (inside(x, y, 24, 140, 432, 200)) {
            const int index = w->project_first + (y - 140) / 50;
            if (index < (int)tracker_workspace_project_count(w)) {
                w->selected_project = index;
                w->dirty = true;
            }
        } else if (inside(x, y, 24, 342, 76, 28)) {
            w->project_first = w->project_first >= 4 ? w->project_first - 4 : 0;
            w->selected_project = w->project_first;
            w->dirty = true;
        } else if (inside(x, y, 380, 342, 76, 28)) {
            if (w->project_first + 4 < (int)tracker_workspace_project_count(w)) {
                w->project_first += 4;
            }
            w->selected_project = w->project_first;
            w->dirty = true;
        } else if (inside(x, y, 24, 378, 208, 44)) {
            const app_project_entry_t *entry = tracker_workspace_project_entry(w, (unsigned)w->selected_project);
            if (entry) {
                request_replace(w, APP_PROJECT_OPEN, entry->name, entry->id, NULL);
            } else {
                tracker_workspace_message(w, "UNAVAILABLE");
            }
        } else if (inside(x, y, 248, 378, 208, 44)) {
            start_save(w, true);
        }
    } else if (w->screen == TRACKER_WORKSPACE_NEW) {
        if (inside(x, y, 24, 132, 432, 96)) {
            request_replace(w, APP_PROJECT_NEW_BLANK, "UNTITLED", NULL, NULL);
        } else if (inside(x, y, 24, 248, 432, 96)) {
            request_replace(w, APP_PROJECT_IMPORT_ARYX, "ARYX IMPORT", NULL, NULL);
        }
    } else if (w->screen == TRACKER_WORKSPACE_IMPORT) {
        if (inside(x, y, 24, 92, 208, 36) || inside(x, y, 248, 92, 208, 36)) {
            w->import_wav = x >= 240;
            w->selected_inbox = w->inbox_first = 0;
            w->dirty = true;
        } else if (!w->import_wav && inside(x, y, 24, 138, 432, 42)) {
            request_replace(w, APP_PROJECT_IMPORT_ARYX, "ARYX IMPORT", NULL, NULL);
        } else if (inside(x, y, 24, 190, 432, 150)) {
            w->selected_inbox = w->inbox_first + (y - 190) / 50;
            w->dirty = true;
        } else if (inside(x, y, 24, 342, 76, 28)) {
            w->inbox_first = w->inbox_first >= 3 ? w->inbox_first - 3 : 0;
            w->selected_inbox = w->inbox_first;
            w->dirty = true;
        } else if (inside(x, y, 380, 342, 76, 28)) {
            if (inbox_at(w, w->inbox_first + 3) >= 0) {
                w->inbox_first += 3;
            }
            w->selected_inbox = w->inbox_first;
            w->dirty = true;
        } else if (inside(x, y, 24, 378, 432, 44)) {
            const int index = inbox_at(w, w->selected_inbox);
            if (index >= 0) {
                const app_project_inbox_entry_t *entry = &w->project_view.inbox[index];
                request_replace(w, w->import_wav ? APP_PROJECT_IMPORT_SAMPLE : APP_PROJECT_IMPORT_MODULE,
                                entry->filename, NULL, entry->filename);
            } else {
                tracker_workspace_message(w, "NO FILES");
            }
        }
    } else if (w->screen == TRACKER_WORKSPACE_NAME) {
        if (inside(x, y, 24, 378, 208, 44)) {
            w->switch_after_save = false;
            show(w, w->name_return);
        } else if (inside(x, y, 248, 378, 208, 44)) {
            bool has_name = false;
            for (const char *p = w->typed_name; *p; ++p) if (*p != ' ') {
                    has_name = true;
                }
            if (!has_name) {
                tracker_workspace_message(w, "ENTER A PROJECT NAME");
            } else {
                memset(&w->request, 0, sizeof(w->request));
                copy_string(w->request.name, sizeof(w->request.name), w->typed_name);
                tracker_workspace_request(w, APP_PROJECT_SAVE_AS);
            }
        } else {
            name_key(w, x, y);
        }
    } else if (w->screen == TRACKER_WORKSPACE_SAVE) {
        if (inside(x, y, 24, 292, 208, 48)) {
            show(w, w->return_screen);
        } else if (inside(x, y, 248, 292, 208, 48)) {
            memset(&w->request, 0, sizeof(w->request));
            tracker_workspace_request(w, APP_PROJECT_SAVE);
        }
    } else if (w->screen == TRACKER_WORKSPACE_SWITCH) {
        if (inside(x, y, 24, 250, 432, 48)) {
            w->after_save = w->pending_switch;
            w->switch_after_save = true;
            if (w->project_view.id[0]) {
                memset(&w->request, 0, sizeof(w->request));
                tracker_workspace_request(w, APP_PROJECT_SAVE);
            } else {
                start_save(w, false);
            }
        } else if (inside(x, y, 24, 310, 432, 48)) {
            w->request = w->pending_switch;
            w->request.discard_dirty = true;
            tracker_workspace_request(w, w->request.action);
        } else if (inside(x, y, 24, 370, 432, 48)) {
            w->switch_after_save = false;
            show(w, TRACKER_WORKSPACE_PROJECTS);
        }
    } else if (w->screen == TRACKER_WORKSPACE_ERROR) {
        if (inside(x, y, 24, 310, 208, 48)) {
            w->request = w->retry;
            submit_request(w); /* Keep SAVE AS / SAVE+SWITCH intent on retry. */
        } else if (inside(x, y, 248, 310, 208, 48)) {
            w->switch_after_save = false;
            show(w, TRACKER_WORKSPACE_PROJECTS);
        }
    } else if (w->screen == TRACKER_WORKSPACE_STORAGE && inside(x, y, 24, 374, 432, 44)) {
        show(w, TRACKER_WORKSPACE_SETTINGS);
    }
    (void)settings;
}

bool tracker_workspace_pointer(tracker_workspace_t *w, tracker_model_t *model,
                               tracker_settings_t *settings, int x, int y,
                               bool pressed)
{
    if (w->screen == TRACKER_WORKSPACE_LEGACY && model->pointer_down) {
        return false;
    }
    if (w->screen == TRACKER_WORKSPACE_SAMPLER &&
            (w->sampler_pointer || (y >= TRACKER_WORKSPACE_HEADER && y < 432 && !w->pointer_down)) &&
            !tracker_workspace_busy(w)) {
        tracker_sampler_ui_pointer(w, model, x, y, pressed);
        return true;
    }
    if (w->sample_drag != TRACKER_SAMPLE_DRAG_NONE) {
        tracker_sampler_ui_pointer(w, model, x, y, pressed);
        return true;
    }
    if (w->screen == TRACKER_WORKSPACE_LEGACY && !w->pointer_down &&
            !tracker_workspace_busy(w)) {
        return false;
    }
    const bool settings_storage = inside(x, y, 336, 0, 144, 40);
    if (w->screen == TRACKER_WORKSPACE_SETTINGS && !w->pointer_down &&
            (settings->pointer_down || (!settings_storage && y >= 0 && y < 432))) {
        settings->open = true;
        settings->page_pointer_down = false;
        (void)tracker_settings_page_pointer(settings, x, y, pressed);
        settings->open = false;
        w->dirty = true;
        return true;
    }
    if (pressed && !w->pointer_down) {
        w->pointer_down = true;
        w->moved = false;
        w->start_x = x;
        w->start_y = y;
        return true;
    }
    if (!w->pointer_down) {
        return w->screen != TRACKER_WORKSPACE_LEGACY;
    }
    const int dx = x - w->start_x;
    const int dy = y - w->start_y;
    if (abs(dx) > WORKSPACE_TAP_SLOP || abs(dy) > WORKSPACE_TAP_SLOP) {
        w->moved = true;
    }
    if (!pressed) {
        w->pointer_down = false;
        if (w->screen == TRACKER_WORKSPACE_HOME && abs(dx) >= WORKSPACE_SWIPE_DISTANCE && abs(dx) > abs(dy) * 2) {
            page_home(w, model, w->home_page + (dx < 0 ? 1 : 2));
        } else if (!w->moved) {
            tap(w, model, settings, x, y);
        }
    }
    return true;
}

int tracker_workspace_legacy_y(int y)
{
    if (y < 0 || y >= TRACKER_WORKSPACE_CONTENT_HEIGHT) {
        return y;
    }
    return y * 432 / TRACKER_WORKSPACE_CONTENT_HEIGHT;
}

int tracker_workspace_content_y(const tracker_workspace_t *w, int y)
{
    if (w->screen == TRACKER_WORKSPACE_HOME || y < 0 ||
            y >= TRACKER_WORKSPACE_CONTENT_HEIGHT) {
        return y;
    }
    if (w->screen == TRACKER_WORKSPACE_SETTINGS || w->screen == TRACKER_WORKSPACE_LEGACY) {
        return tracker_workspace_legacy_y(y);
    }
    return TRACKER_WORKSPACE_HEADER + y * (432 - TRACKER_WORKSPACE_HEADER) /
           TRACKER_WORKSPACE_CONTENT_HEIGHT;
}

static void fit_band(uint16_t *frame, int first_row)
{
    const int rows = 432 - first_row;
    if (first_row) {
        memmove(frame, frame + (size_t)first_row * 480U,
                (size_t)rows * 480U * sizeof(*frame));
    }
    /* Expand bottom-up only after removing the old shared-header band. This
     * keeps all unread source rows intact with no temporary frame allocation. */
    for (int y = TRACKER_WORKSPACE_CONTENT_HEIGHT - 1; y >= 0; --y) {
        const int source = y * rows / TRACKER_WORKSPACE_CONTENT_HEIGHT;
        memmove(frame + (size_t)y * 480U, frame + (size_t)source * 480U,
                480U * sizeof(*frame));
    }
    tracker_graphics_t g;
    tracker_gfx_init(&g, frame);
    tracker_gfx_clip(&g, 0, 0, 480, 480);
    tracker_gfx_rect(&g, 0, TRACKER_WORKSPACE_CONTENT_HEIGHT, 480,
                     480 - TRACKER_WORKSPACE_CONTENT_HEIGHT, C_BG);
}

void tracker_workspace_fit_legacy(uint16_t *frame)
{
    fit_band(frame, 0);
}

void tracker_workspace_fit_content(uint16_t *frame, const tracker_workspace_t *w)
{
    if (w->screen != TRACKER_WORKSPACE_HOME) {
        fit_band(frame, w->screen == TRACKER_WORKSPACE_LEGACY ||
                 w->screen == TRACKER_WORKSPACE_SETTINGS ? 0 : TRACKER_WORKSPACE_HEADER);
    }
}

int tracker_workspace_route(const tracker_workspace_t *w, const tracker_model_t *model)
{
    return w->screen == TRACKER_WORKSPACE_LEGACY ? 100 + model->view.page :
           w->screen == TRACKER_WORKSPACE_HOME ? w->home_page : 10 + w->screen;
}
