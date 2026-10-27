// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app_project.h"
#include "app_sampler.h"
#include "project_platform.h"

#define PROJECT_PATH_BYTES 384
#define PROJECT_MAX_MODULE_BYTES (8U * 1024U * 1024U)
#define PROJECT_MAX_BANK_BYTES (20U * 1024U * 1024U)
#define PROJECT_STATE_BYTES 384U
#define PROJECT_MANIFEST_BYTES 192U

typedef struct {
    app_synth_patch_t patch;
    float channel_gain[TRACKER_MAX_CHANNELS];
    bool mute[TRACKER_MAX_CHANNELS];
    bool solo[TRACKER_MAX_CHANNELS];
    float source_gain[4];
    float speed_multiplier;
    bool loop;
    int master_volume;
    int pad_patterns[APP_PROJECT_PAD_COUNT];
} project_state_t;

typedef struct {
    uint8_t *module;
    size_t module_size;
    uint8_t *bank_bytes;
    size_t bank_size;
    app_sampler_bank_t *bank;
    tracker_engine_prepared_t *song;
    project_state_t state;
    char name[APP_PROJECT_NAME_BYTES];
    char id[APP_PROJECT_ID_BYTES];
    uint64_t generation;
    uint64_t revision;
} project_payload_t;

struct app_project {
    app_project_config_t config;
    project_platform_t platform;
    app_project_snapshot_t snapshot;
    app_project_snapshot_t catalog;
    app_project_request_t request;
    bool pending;
    bool stopping;
    bool editing;
    bool inbox_ready;
    bool owns_nand;
    bool host_root;
    char root[PROJECT_PATH_BYTES];
    char projects[PROJECT_PATH_BYTES];
    char inbox[PROJECT_PATH_BYTES];
    uint64_t next_token;
    uint64_t sampler_revision;
};

bool project_name_valid(const char *name);
bool project_id_valid(const char *id);
bool project_filename_valid(const char *name);
app_project_error_t project_errno_error(void);
bool project_path(char out[PROJECT_PATH_BYTES], const char *base, const char *name);
app_project_error_t project_prepare_storage(app_project_t *project);
app_project_error_t project_refresh(app_project_t *project);
app_project_error_t project_file_load(app_project_t *project, const char *id,
                                      project_payload_t *payload);
app_project_error_t project_file_save(app_project_t *project, project_payload_t *payload);
app_project_error_t project_read_inbox(app_project_t *project, const char *filename,
                                       uint8_t **bytes, size_t *size, size_t maximum);
app_project_error_t project_capture(app_project_t *project, project_payload_t *payload);
app_project_error_t project_prepare_payload(project_payload_t *payload);
app_project_error_t project_commit_payload(app_project_t *project, project_payload_t *payload);
app_project_error_t project_blank_payload(project_payload_t *payload);
void project_payload_destroy(project_payload_t *payload);
app_project_error_t project_sampler_error(app_sampler_result_t result);
uint32_t project_crc32(const void *bytes, size_t size);
bool project_state_encode(const project_state_t *state, uint8_t bytes[PROJECT_STATE_BYTES]);
bool project_state_decode(const uint8_t bytes[PROJECT_STATE_BYTES], project_state_t *state);
void project_state_defaults(project_state_t *state);
