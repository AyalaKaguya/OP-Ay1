// SPDX-License-Identifier: Apache-2.0
#include "project_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(ESP_PLATFORM)
#include "app_nand.h"
#endif

const char *app_project_error_name(app_project_error_t error)
{
    static const char *const names[] = {"OK", "BUSY", "INVALID", "NO MEMORY", "STORAGE UNAVAILABLE",
        "NOT FOUND", "CORRUPT PROJECT", "NO SPACE", "IO ERROR", "UNSAVED CHANGES", "UNSUPPORTED", "TIMEOUT"};
    return error >= APP_PROJECT_OK && error <= APP_PROJECT_TIMEOUT ? names[error] : "UNKNOWN ERROR";
}

static void message(app_project_t *p, app_project_error_t error, const char *text)
{
    project_platform_lock(&p->platform);
    p->snapshot.error = error;
    p->snapshot.io_error = error == APP_PROJECT_IO_ERROR || error == APP_PROJECT_NO_SPACE ? errno : p->snapshot.io_error;
    snprintf(p->snapshot.message, sizeof(p->snapshot.message), "%s", text ? text : app_project_error_name(error));
    project_platform_unlock(&p->platform);
}

static bool is_replace(app_project_action_t action)
{
    return action == APP_PROJECT_NEW_BLANK || action == APP_PROJECT_IMPORT_ARYX ||
           action == APP_PROJECT_OPEN || action == APP_PROJECT_IMPORT_MODULE;
}

static app_project_error_t check_capture_idle(app_project_t *p)
{
    app_sampler_record_status_t status;
    memset(&status, 0, sizeof(status));
    app_sampler_record_get_status(p->config.sampler, &status);
    return status.state == APP_SAMPLER_RECORD_RECORDING || status.state == APP_SAMPLER_RECORD_STOPPING ||
           status.state == APP_SAMPLER_RECORD_READY || status.state == APP_SAMPLER_RECORD_ERROR
           ? APP_PROJECT_BUSY : APP_PROJECT_OK;
}

static app_project_error_t ensure_storage(app_project_t *p)
{
    project_platform_lock(&p->platform);
    const bool ready = p->snapshot.storage_ready;
    project_platform_unlock(&p->platform);
    return ready ? APP_PROJECT_OK : project_prepare_storage(p);
}

static app_project_error_t example_payload(app_project_t *p, project_payload_t *payload)
{
    if (p->config.example_data == NULL || p->config.example_size == 0 ||
        p->config.example_size > PROJECT_MAX_MODULE_BYTES) return APP_PROJECT_UNSUPPORTED;
    payload->module = malloc(p->config.example_size);
    if (!payload->module) return APP_PROJECT_NO_MEMORY;
    memcpy(payload->module, p->config.example_data, p->config.example_size);
    payload->module_size = p->config.example_size;
    payload->revision = 1;
    strcpy(payload->name, "ARYX DEMO");
    project_state_defaults(&payload->state);
    return APP_PROJECT_OK;
}

static app_project_error_t fresh_id(app_project_t *p, char out[APP_PROJECT_ID_BYTES])
{
    char path[PROJECT_PATH_BYTES];
    const uint64_t clock = project_platform_time_ms();
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
        snprintf(out, APP_PROJECT_ID_BYTES, "%016" PRIx64 "%016" PRIx64, clock, ++p->next_token);
        if (!project_path(path, p->projects, out)) return APP_PROJECT_INVALID;
        struct stat st;
        if (stat(path, &st) != 0) return errno == ENOENT ? APP_PROJECT_OK : project_errno_error();
    }
    return APP_PROJECT_IO_ERROR;
}

static app_project_error_t save_session(app_project_t *p, const app_project_request_t *request)
{
    app_project_error_t error = check_capture_idle(p);
    if (error != APP_PROJECT_OK) return error;
    error = ensure_storage(p);
    if (error != APP_PROJECT_OK) return error;
    project_payload_t *payload = calloc(1, sizeof(*payload));
    if (!payload) return APP_PROJECT_NO_MEMORY;
    project_state_defaults(&payload->state);
    error = project_capture(p, payload);
    if (error == APP_PROJECT_OK && (request->action == APP_PROJECT_SAVE_AS || payload->id[0] == 0)) {
        error = fresh_id(p, payload->id);
    }
    if (error == APP_PROJECT_OK && request->name[0]) strcpy(payload->name, request->name);
    if (error == APP_PROJECT_OK) error = project_file_save(p, payload);
    if (error == APP_PROJECT_OK) {
        project_platform_lock(&p->platform);
        strcpy(p->snapshot.id, payload->id); strcpy(p->snapshot.name, payload->name);
        p->snapshot.saved_revision = payload->revision;
        p->snapshot.dirty = p->snapshot.revision != p->snapshot.saved_revision;
        project_platform_unlock(&p->platform);
    }
    project_payload_destroy(payload); free(payload);
    if (error == APP_PROJECT_OK) (void)project_refresh(p);
    return error;
}

static app_project_error_t replace_session(app_project_t *p, const app_project_request_t *request)
{
    if (check_capture_idle(p) != APP_PROJECT_OK) return APP_PROJECT_BUSY;
    project_payload_t *payload = calloc(1, sizeof(*payload));
    if (!payload) return APP_PROJECT_NO_MEMORY;
    project_state_defaults(&payload->state);
    app_project_error_t error = APP_PROJECT_OK;
    switch (request->action) {
    case APP_PROJECT_NEW_BLANK:
        error = project_blank_payload(payload);
        break;
    case APP_PROJECT_IMPORT_ARYX:
        error = example_payload(p, payload);
        break;
    case APP_PROJECT_OPEN:
        error = ensure_storage(p);
        if (error == APP_PROJECT_OK) error = project_file_load(p, request->id, payload);
        break;
    case APP_PROJECT_IMPORT_MODULE:
        error = ensure_storage(p);
        if (error == APP_PROJECT_OK) error = project_read_inbox(p, request->filename,
                    &payload->module, &payload->module_size, PROJECT_MAX_MODULE_BYTES);
        payload->revision = 1;
        snprintf(payload->name, sizeof(payload->name), "Imported %.48s", request->filename);
        break;
    default:
        error = APP_PROJECT_INVALID;
        break;
    }
    if (error == APP_PROJECT_OK && request->name[0]) strcpy(payload->name, request->name);
    if (error == APP_PROJECT_OK) error = project_prepare_payload(payload);
    if (error == APP_PROJECT_OK) error = project_commit_payload(p, payload);
    project_payload_destroy(payload); free(payload);
    return error;
}

static app_project_error_t sampler_job(app_project_t *p, const app_project_request_t *request)
{
    app_sampler_result_t result = APP_SAMPLER_OK;
    if (request->action == APP_PROJECT_RECORD_START) {
        return project_sampler_error(app_sampler_record_start(p->config.sampler));
    }
    if (request->action == APP_PROJECT_RECORD_KEEP) {
        unsigned index = 0;
        result = app_sampler_record_keep(p->config.sampler, request->name[0] ? request->name : "TAKE", &index);
    } else if (request->action == APP_PROJECT_IMPORT_SAMPLE) {
        app_project_error_t error = ensure_storage(p);
        if (error != APP_PROJECT_OK) return error;
        char *path = malloc(PROJECT_PATH_BYTES);
        if (!path) return APP_PROJECT_NO_MEMORY;
        if (!project_path(path, p->inbox, request->filename)) { free(path); return APP_PROJECT_INVALID; }
        unsigned index = 0;
        result = app_sampler_import_wav_file(p->config.sampler, path, &index);
        free(path);
    } else {
        if (!app_audio_bus_begin_edit(p->config.bus, 1000)) return APP_PROJECT_TIMEOUT;
        if (request->action == APP_PROJECT_ASSIGN_SAMPLE) {
            result = app_sampler_assign_slot(p->config.sampler, request->sample_index,
                                             p->config.tracker, request->target_slot);
        } else if (request->action == APP_PROJECT_APPLY_SAMPLE) result = app_sampler_edit_apply(p->config.sampler);
        else if (request->action == APP_PROJECT_UNDO_SAMPLE) result = app_sampler_undo(p->config.sampler);
        else result = APP_SAMPLER_INVALID;
        app_audio_bus_end_edit(p->config.bus);
    }
    if (result == APP_SAMPLER_OK) {
        p->sampler_revision = app_sampler_revision(p->config.sampler);
        project_platform_lock(&p->platform);
        ++p->snapshot.revision;
        p->snapshot.dirty = true;
        p->snapshot.sample_count = app_sampler_count(p->config.sampler);
        project_platform_unlock(&p->platform);
    }
    return project_sampler_error(result);
}

static void observe_sampler(app_project_t *p)
{
    const uint64_t revision = app_sampler_revision(p->config.sampler);
    if (revision == p->sampler_revision) return;
    project_platform_lock(&p->platform);
    if (!p->snapshot.busy && !p->editing) {
        p->sampler_revision = revision;
        ++p->snapshot.revision;
        p->snapshot.dirty = true;
        p->snapshot.sample_count = app_sampler_count(p->config.sampler);
    }
    project_platform_unlock(&p->platform);
}

static void run_request(app_project_t *p, const app_project_request_t *request)
{
    app_project_error_t error;
    if (is_replace(request->action)) error = replace_session(p, request);
    else if (request->action == APP_PROJECT_SAVE || request->action == APP_PROJECT_SAVE_AS) error = save_session(p, request);
    else if (request->action == APP_PROJECT_REFRESH) {
        error = ensure_storage(p);
        if (error == APP_PROJECT_OK) error = project_refresh(p);
    } else error = sampler_job(p, request);
    message(p, error, error == APP_PROJECT_OK ? request->action == APP_PROJECT_SAVE || request->action == APP_PROJECT_SAVE_AS
             ? "Project saved" : "Ready" : NULL);
    project_platform_lock(&p->platform);
    p->snapshot.busy = false;
    project_platform_unlock(&p->platform);
}

static void project_worker(void *argument)
{
    app_project_t *p = argument;
    /* Initialization seeds an unsaved real demo/blank bank but never writes a
     * saved project or automatically opens an unknown previous session. */
    app_project_error_t storage = project_prepare_storage(p);
    project_payload_t *initial = calloc(1, sizeof(*initial));
    app_project_error_t error = initial ? APP_PROJECT_OK : APP_PROJECT_NO_MEMORY;
    if (initial) {
        error = p->config.example_data && p->config.example_size ? example_payload(p, initial) : project_blank_payload(initial);
        if (error == APP_PROJECT_OK) error = project_prepare_payload(initial);
        if (error == APP_PROJECT_OK) error = project_commit_payload(p, initial);
        project_payload_destroy(initial); free(initial);
    }
    if (storage == APP_PROJECT_OK) (void)project_refresh(p);
    message(p, error != APP_PROJECT_OK ? error : storage, error == APP_PROJECT_OK && storage == APP_PROJECT_OK ? "Unsaved session ready" : NULL);
    project_platform_lock(&p->platform); p->snapshot.busy = false; project_platform_unlock(&p->platform);
    while (true) {
        project_platform_wait(&p->platform, 100);
        project_platform_lock(&p->platform);
        const bool stop = p->stopping;
        const bool pending = p->pending;
        app_project_request_t *request = pending ? malloc(sizeof(*request)) : NULL;
        if (pending && request) { *request = p->request; p->pending = false; }
        project_platform_unlock(&p->platform);
        if (stop && !pending) break;
        if (pending && !request) {
            message(p, APP_PROJECT_NO_MEMORY, NULL);
            project_platform_lock(&p->platform); p->pending = false; p->snapshot.busy = false; project_platform_unlock(&p->platform);
        } else if (request) { run_request(p, request); free(request); }
        observe_sampler(p);
    }
    /* The composition root drains Iris inbox users before shutdown. If NAND
     * teardown fails, retain the service and retry rather than orphaning SPI. */
#if defined(ESP_PLATFORM)
    if (p->owns_nand) {
        while (app_nand_unmount() != ESP_OK) project_platform_wait(&p->platform, 100);
    }
#endif
}

app_project_error_t app_project_create(const app_project_config_t *config, app_project_t **out_project)
{
    if (!out_project) return APP_PROJECT_INVALID;
    *out_project = NULL;
    if (!config || !config->tracker || !config->synth || !config->bus || !config->sampler) return APP_PROJECT_INVALID;
    const char *root = config->root_path;
#if defined(ESP_PLATFORM)
    if (root && strcmp(root, APP_NAND_PROJECT_ROOT)) return APP_PROJECT_INVALID;
    root = APP_NAND_PROJECT_ROOT;
#else
    if (!root || !root[0] || !strcmp(root, "/")) return APP_PROJECT_INVALID;
#endif
    if (strlen(root) >= PROJECT_PATH_BYTES - 64U) return APP_PROJECT_INVALID;
    app_project_t *p = calloc(1, sizeof(*p));
    if (!p) return APP_PROJECT_NO_MEMORY;
    p->config = *config;
    strcpy(p->root, root);
    const size_t root_length = strlen(p->root);
    if (root_length > 1 && (p->root[root_length - 1] == '/' || p->root[root_length - 1] == '\\')) p->root[root_length - 1] = 0;
    if (!project_path(p->projects, p->root, "projects") || !project_path(p->inbox, p->root, "inbox") ||
        !project_platform_init(&p->platform)) { free(p); return APP_PROJECT_NO_MEMORY; }
    p->snapshot.busy = true; p->snapshot.opened = true; p->snapshot.dirty = true;
    p->snapshot.revision = 1; p->snapshot.master_volume = 100;
    strcpy(p->snapshot.name, config->example_data && config->example_size ? "ARYX DEMO" : "Untitled");
    for (int pad = 0; pad < APP_PROJECT_PAD_COUNT; ++pad) p->snapshot.pad_patterns[pad] = pad;
    p->next_token = project_platform_time_ms() << 16;
    p->sampler_revision = app_sampler_revision(config->sampler);
    if (!project_platform_start(&p->platform, project_worker, p)) {
        project_platform_destroy(&p->platform); free(p); return APP_PROJECT_NO_MEMORY;
    }
    *out_project = p;
    return APP_PROJECT_OK;
}

app_project_error_t app_project_request(app_project_t *p, const app_project_request_t *request)
{
    if (!p || !request || request->action < APP_PROJECT_REFRESH || request->action > APP_PROJECT_RECORD_KEEP ||
        memchr(request->id, 0, sizeof(request->id)) == NULL ||
        memchr(request->name, 0, sizeof(request->name)) == NULL ||
        memchr(request->filename, 0, sizeof(request->filename)) == NULL ||
        (request->name[0] && !project_name_valid(request->name))) return APP_PROJECT_INVALID;
    if (request->action == APP_PROJECT_OPEN && !project_id_valid(request->id)) return APP_PROJECT_INVALID;
    if ((request->action == APP_PROJECT_IMPORT_MODULE || request->action == APP_PROJECT_IMPORT_SAMPLE) &&
        !project_filename_valid(request->filename)) return APP_PROJECT_INVALID;
    if (!project_platform_trylock(&p->platform)) return APP_PROJECT_BUSY;
    app_project_error_t result = APP_PROJECT_OK;
    if (p->stopping || p->snapshot.busy || p->editing) result = APP_PROJECT_BUSY;
    else if (request->action == APP_PROJECT_OPEN && !strcmp(request->id, p->snapshot.id)) {
        /* Preserve dirty edits, sounding notes and playback on repeated open. */
        result = APP_PROJECT_OK;
    } else if (is_replace(request->action) && p->snapshot.dirty && !request->discard_dirty) {
        result = APP_PROJECT_DIRTY_CONFLICT;
    } else {
        p->request = *request; p->pending = true;
        p->snapshot.busy = true; p->snapshot.action = request->action;
        p->snapshot.error = APP_PROJECT_OK; p->snapshot.io_error = 0;
        ++p->snapshot.operation_generation;
        strcpy(p->snapshot.message, "Working...");
        project_platform_wake(&p->platform);
    }
    project_platform_unlock(&p->platform);
    return result;
}

bool app_project_begin_edit(app_project_t *p)
{
    if (!p || !project_platform_trylock(&p->platform)) return false;
    const bool allowed = !p->stopping && !p->snapshot.busy && !p->editing;
    if (allowed) p->editing = true;
    project_platform_unlock(&p->platform);
    return allowed;
}

void app_project_end_edit(app_project_t *p, bool changed)
{
    if (!p) return;
    project_platform_lock(&p->platform);
    if (p->editing) {
        if (changed) { ++p->snapshot.revision; p->snapshot.dirty = true; }
        p->editing = false;
    }
    project_platform_unlock(&p->platform);
}

bool app_project_note_edit(app_project_t *p)
{
    if (!app_project_begin_edit(p)) return false;
    app_project_end_edit(p, true); return true;
}

bool app_project_set_controls(app_project_t *p, int master_volume, const int pad_patterns[APP_PROJECT_PAD_COUNT])
{
    if (!p || master_volume < 0 || master_volume > 100 || !pad_patterns) return false;
    for (unsigned pad = 0; pad < APP_PROJECT_PAD_COUNT; ++pad) if (pad_patterns[pad] < -1 || pad_patterns[pad] > 255) return false;
    project_platform_lock(&p->platform);
    const bool allowed = p->editing && !p->stopping && !p->snapshot.busy;
    if (allowed) { p->snapshot.master_volume = master_volume; memcpy(p->snapshot.pad_patterns, pad_patterns, sizeof(p->snapshot.pad_patterns)); }
    project_platform_unlock(&p->platform);
    return allowed;
}

bool app_project_inbox_ready(app_project_t *p)
{
    if (!p || !project_platform_trylock(&p->platform)) return false;
    const bool ready = p->inbox_ready && !p->stopping;
    project_platform_unlock(&p->platform); return ready;
}

app_project_error_t app_project_read(app_project_t *p, app_project_snapshot_t *out_snapshot)
{
    if (!p || !out_snapshot) return APP_PROJECT_INVALID;
    if (!project_platform_trylock(&p->platform)) return APP_PROJECT_BUSY;
    *out_snapshot = p->snapshot;
    project_platform_unlock(&p->platform); return APP_PROJECT_OK;
}

app_project_error_t app_project_shutdown(app_project_t **project, uint32_t timeout_ms)
{
    if (!project) return APP_PROJECT_INVALID;
    if (!*project) return APP_PROJECT_OK;
    app_project_t *p = *project;
    project_platform_lock(&p->platform);
    if (p->editing) { project_platform_unlock(&p->platform); return APP_PROJECT_BUSY; }
    p->stopping = true;
    project_platform_unlock(&p->platform);
    project_platform_wake(&p->platform);
    if (!project_platform_join(&p->platform, timeout_ms)) return APP_PROJECT_TIMEOUT;
    project_platform_destroy(&p->platform);
    free(p); *project = NULL; return APP_PROJECT_OK;
}
