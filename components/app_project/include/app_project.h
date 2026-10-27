// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_audio_bus.h"
#include "app_note_router.h"
#include "app_synth.h"
#include "tracker_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PROJECT_NAME_BYTES 64
#define APP_PROJECT_ID_BYTES 33
#define APP_PROJECT_FILENAME_BYTES 96
#define APP_PROJECT_LIST_MAX 32
#define APP_PROJECT_PAD_COUNT 16

typedef struct app_sampler app_sampler_t;
typedef struct app_project app_project_t;

typedef enum {
    APP_PROJECT_OK = 0,
    APP_PROJECT_BUSY,
    APP_PROJECT_INVALID,
    APP_PROJECT_NO_MEMORY,
    APP_PROJECT_STORAGE_UNAVAILABLE,
    APP_PROJECT_NOT_FOUND,
    APP_PROJECT_CORRUPT,
    APP_PROJECT_NO_SPACE,
    APP_PROJECT_IO_ERROR,
    APP_PROJECT_DIRTY_CONFLICT,
    APP_PROJECT_UNSUPPORTED,
    APP_PROJECT_TIMEOUT,
} app_project_error_t;

typedef enum {
    APP_PROJECT_REFRESH = 0,
    APP_PROJECT_NEW_BLANK,
    APP_PROJECT_IMPORT_ARYX,
    APP_PROJECT_OPEN,
    APP_PROJECT_SAVE,
    APP_PROJECT_SAVE_AS,
    APP_PROJECT_IMPORT_MODULE,
    APP_PROJECT_IMPORT_SAMPLE,
    APP_PROJECT_ASSIGN_SAMPLE,
    APP_PROJECT_RECORD_START,
    APP_PROJECT_APPLY_SAMPLE,
    APP_PROJECT_UNDO_SAMPLE,
    APP_PROJECT_RECORD_KEEP,
} app_project_action_t;

typedef enum {
    APP_PROJECT_INBOX_MODULE,
    APP_PROJECT_INBOX_SAMPLE,
} app_project_inbox_kind_t;

typedef struct {
    char id[APP_PROJECT_ID_BYTES];
    char name[APP_PROJECT_NAME_BYTES];
    uint64_t saved_revision;
    uint64_t generation;
    uint64_t bytes;
} app_project_entry_t;

typedef struct {
    char filename[APP_PROJECT_FILENAME_BYTES];
    uint64_t bytes;
    app_project_inbox_kind_t kind;
} app_project_inbox_entry_t;

/* Allocate snapshots in the UI owner's heap/context, not on a small task
 * stack. Catalog values describe actual verified files, never fabricated rows. */
typedef struct {
    char id[APP_PROJECT_ID_BYTES];     /* Empty means an unsaved session. */
    char name[APP_PROJECT_NAME_BYTES];
    bool opened;
    bool dirty;
    bool busy;
    bool storage_ready;
    bool storage_usage_valid;
    app_project_error_t error;
    int io_error;                    /* errno or device esp_err_t. */
    app_project_action_t action;
    uint64_t revision;
    uint64_t saved_revision;
    uint64_t session_generation;      /* Changes only on replacement/new. */
    uint64_t storage_total_bytes;
    uint64_t storage_used_bytes;
    uint32_t sample_count;
    int master_volume;
    int pad_patterns[APP_PROJECT_PAD_COUNT];
    uint64_t operation_generation;
    uint32_t entry_count;
    uint32_t inbox_count;
    bool entries_truncated;
    bool inbox_truncated;
    char message[96];
    app_project_entry_t entries[APP_PROJECT_LIST_MAX];
    app_project_inbox_entry_t inbox[APP_PROJECT_LIST_MAX];
} app_project_snapshot_t;

typedef struct {
    app_project_action_t action;
    char id[APP_PROJECT_ID_BYTES];
    char name[APP_PROJECT_NAME_BYTES];
    char filename[APP_PROJECT_FILENAME_BYTES]; /* Basename in owned inbox only. */
    int sample_slot;                 /* 0..15 for IMPORT_SAMPLE. */
    unsigned sample_index;           /* Library entry for ASSIGN_SAMPLE. */
    unsigned target_slot;            /* 1-based tracker slot for ASSIGN_SAMPLE. */
    bool discard_dirty;             /* UI-confirmed replace of unsaved changes. */
} app_project_request_t;

typedef struct {
    tracker_engine_t *tracker;
    app_synth_t *synth;
    app_audio_bus_t *bus;
    app_sampler_t *sampler;
    app_note_router_t *router;        /* Optional; reset only on committed switch. */
    const uint8_t *example_data;      /* Borrowed built-in ARYX until shutdown. */
    size_t example_size;
    /* NULL selects /nand/apps/op-ay1 on ESP. Host callers must provide a real
     * local root (for example build-sim/projects). The worker creates only
     * projects/ and inbox/ below this root, never other applications' files. */
    const char *root_path;
} app_project_config_t;

/* Sources have stable caller-owned handles. Creation starts one IO worker;
 * mounting/catalog IO are asynchronous. No formatting, erase or SDK writes. */
app_project_error_t app_project_create(const app_project_config_t *config,
                                       app_project_t **out_project);
/* One in-flight command, no hidden queue. Returns BUSY immediately if occupied.
 * OPEN of the current saved ID is a no-op preserving its unsaved edits/transport.
 * NEW/OPEN/import reject dirty replacement unless discard_dirty is true. */
app_project_error_t app_project_request(app_project_t *project,
                                        const app_project_request_t *request);
/* Call on successful musical edits only, not playback/navigation/meters.
 * Returns false while an IO/mutation job owns the session: UI must not edit. */
bool app_project_note_edit(app_project_t *project);
/* Try before a UI mutation and always pair with end_edit. While held,
 * background job admission returns BUSY. No IO/wait occurs in this pair. */
bool app_project_begin_edit(app_project_t *project);
void app_project_end_edit(app_project_t *project, bool changed);
/* Small UI-owned session controls travel with a project, separately from
 * device preferences. Call only inside an admitted begin/end edit pair. */
bool app_project_set_controls(app_project_t *project, int master_volume,
                              const int pad_patterns[APP_PROJECT_PAD_COUNT]);
/* Small nonblocking readiness query for the deferred Iris inbox volume. */
bool app_project_inbox_ready(app_project_t *project);
/* Zero-wait coherent read; BUSY leaves the previous caller snapshot unchanged. */
app_project_error_t app_project_read(app_project_t *project,
                                     app_project_snapshot_t *out_snapshot);
/* Stop all request/read/edit callers first. Timeout retains *project for retry;
 * the sources and example blob remain borrowed until successful shutdown. */
app_project_error_t app_project_shutdown(app_project_t **project, uint32_t timeout_ms);
const char *app_project_error_name(app_project_error_t error);

#ifdef __cplusplus
}
#endif
