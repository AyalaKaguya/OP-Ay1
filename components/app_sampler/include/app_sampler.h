// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tracker_engine.h"

#define APP_SAMPLER_MAX_SAMPLES 64U
#define APP_SAMPLER_NAME_BYTES 64U
#define APP_SAMPLER_MAX_SECONDS 60U
#define APP_SAMPLER_CAPTURE_RATE 48000U
#define APP_SAMPLER_MAX_FRAMES (APP_SAMPLER_CAPTURE_RATE * APP_SAMPLER_MAX_SECONDS)
#define APP_SAMPLER_BANK_PCM_BYTES (16U * 1024U * 1024U)

typedef struct app_sampler app_sampler_t;
typedef struct app_sampler_bank app_sampler_bank_t;

typedef enum {
    APP_SAMPLER_OK = 0, APP_SAMPLER_INVALID, APP_SAMPLER_FORMAT,
    APP_SAMPLER_RANGE, APP_SAMPLER_NO_MEMORY, APP_SAMPLER_FULL,
    APP_SAMPLER_BUSY, APP_SAMPLER_IO, APP_SAMPLER_UNSUPPORTED,
    APP_SAMPLER_BAD_STATE
} app_sampler_result_t;
typedef enum {
    APP_SAMPLER_SOURCE_S3M = 0, APP_SAMPLER_SOURCE_WAV,
    APP_SAMPLER_SOURCE_RECORDING
} app_sampler_source_t;

/* Indices are zero based; original_slot/assigned_slot are 1-based S3M slots,
 * zero means no slot. Empty S3M slots stay visible with frames == 0. PCM is
 * immutable signed 16-bit mono. All bounds are PCM frame offsets, end exclusive.
 * sample_rate preserves imported WAV/recorded rate. S3M has no independent
 * recording rate, so sample_rate is its original C2 reference playback rate
 * (which may be below 8 kHz); c2_speed is the current editable playback rate. */
typedef struct {
    char name[APP_SAMPLER_NAME_BYTES];
    app_sampler_source_t source;
    uint32_t frames, sample_rate, c2_speed;
    uint32_t trim_start, trim_end, loop_start, loop_end;
    float gain;                         /* 0..2, nondestructive. */
    float default_volume;               /* Original instrument volume, 0..1. */
    bool loop_enabled;
    uint16_t original_slot, assigned_slot;
} app_sampler_sample_info_t;
typedef struct {
    uint32_t trim_start, trim_end, loop_start, loop_end, c2_speed;
    float gain;
    bool loop_enabled;
} app_sampler_edit_t;

typedef enum {
    APP_SAMPLER_RECORD_IDLE = 0, APP_SAMPLER_RECORD_RECORDING,
    APP_SAMPLER_RECORD_STOPPING, APP_SAMPLER_RECORD_READY,
    APP_SAMPLER_RECORD_ERROR, APP_SAMPLER_RECORD_UNSUPPORTED
} app_sampler_record_state_t;
typedef struct {
    app_sampler_record_state_t state;
    app_sampler_result_t result;
    uint32_t frames, sample_rate, max_frames;
    float peak, gain_db;
    bool monitor;
} app_sampler_record_status_t;

/* Called by main; borrows no engine. Destroy only after audio/capture callers
 * stop. Device capture is a real BSP microphone worker; hosts return UNSUPPORTED.
 * One UI/controller owns edits. Public calls synchronize with render/capture. */
app_sampler_t *app_sampler_create(void);
/* Startup only: borrows the stable engine for applying edits of assigned slots. */
bool app_sampler_bind_engine(app_sampler_t *sampler, tracker_engine_t *engine);
void app_sampler_destroy(app_sampler_t *sampler);
/* Stops capture and frees only after its worker has exited. On timeout false
 * leaves *sampler valid, retained and stopping; retry later. */
bool app_sampler_shutdown(app_sampler_t **sampler, uint32_t timeout_ms);
const char *app_sampler_result_name(app_sampler_result_t result);
unsigned app_sampler_count(app_sampler_t *sampler);
uint64_t app_sampler_revision(app_sampler_t *sampler);
bool app_sampler_get_info(app_sampler_t *sampler, unsigned index,
                          app_sampler_sample_info_t *info);
/* Copies downsampled signed PCM for waveform display; gain/trim are not baked. */
unsigned app_sampler_waveform(app_sampler_t *sampler, unsigned index,
                              int16_t *out, unsigned capacity);

/* Import is blocking worker I/O/decoding, never call from a UI/audio callback.
 * WAV accepts RIFF PCM 8/16-bit mono/stereo, 8..192 kHz, <=60 seconds; stereo is
 * averaged in int32. It appends one sample and writes index only on success. */
app_sampler_result_t app_sampler_import_wav(app_sampler_t *sampler,
                                            const uint8_t *bytes, size_t size, const char *name, unsigned *index);
app_sampler_result_t app_sampler_import_wav_file(app_sampler_t *sampler,
                                                 const char *path, unsigned *index);
app_sampler_result_t app_sampler_edit_begin(app_sampler_t *sampler, unsigned index);
bool app_sampler_edit_get(app_sampler_t *sampler, app_sampler_edit_t *edit);
app_sampler_result_t app_sampler_edit_set(app_sampler_t *sampler,
                                          const app_sampler_edit_t *edit);
app_sampler_result_t app_sampler_edit_apply(app_sampler_t *sampler);
void app_sampler_edit_cancel(app_sampler_t *sampler);
app_sampler_result_t app_sampler_undo(app_sampler_t *sampler);
/* Explicit replacement only; deep-copies trimmed PCM into engine and preserves
 * every pattern cell/effect. assigned_slot is persisted for later project load. */
app_sampler_result_t app_sampler_assign_slot(app_sampler_t *sampler,
                                             unsigned index, tracker_engine_t *engine, unsigned slot);
app_sampler_result_t app_sampler_audition(app_sampler_t *sampler, unsigned index);
void app_sampler_audition_stop(app_sampler_t *sampler);
/* Sole audio owner: interleaved stereo float at 48 kHz, no render allocation. */
void app_sampler_render(app_sampler_t *sampler, float *stereo, unsigned frames);

app_sampler_result_t app_sampler_record_start(app_sampler_t *sampler);
void app_sampler_record_stop(app_sampler_t *sampler);
void app_sampler_record_get_status(app_sampler_t *sampler,
                                   app_sampler_record_status_t *status);
app_sampler_result_t app_sampler_record_set_gain(app_sampler_t *sampler, float db);
void app_sampler_record_set_monitor(app_sampler_t *sampler, bool enabled);
app_sampler_result_t app_sampler_record_audition(app_sampler_t *sampler);
app_sampler_result_t app_sampler_record_keep(app_sampler_t *sampler,
                                             const char *name, unsigned *index);
void app_sampler_record_discard(app_sampler_t *sampler);

/* Project worker transaction. Snapshot is immutable and retains shared PCM, so
 * encode happens without holding the audio lock. Decode/extract fully stage all
 * allocations. commit consumes bank and cannot fail; caller serializes project
 * switching against user mutations. It clears audition/edit/undo, never records.
 * A recording/take must be stopped and kept/discarded before switching projects.
 * Returned bytes are malloc-owned and released with free(). */
app_sampler_bank_t *app_sampler_bank_snapshot(app_sampler_t *sampler);
app_sampler_bank_t *app_sampler_bank_prepare(const uint8_t *bytes, size_t size,
                                             app_sampler_result_t *result);
app_sampler_bank_t *app_sampler_bank_from_s3m(const uint8_t *bytes, size_t size,
                                              app_sampler_result_t *result);
app_sampler_result_t app_sampler_bank_encode(const app_sampler_bank_t *bank,
                                             uint8_t **bytes, size_t *size);
void app_sampler_bank_destroy(app_sampler_bank_t *bank);
void app_sampler_bank_commit(app_sampler_t *sampler, app_sampler_bank_t *bank);
unsigned app_sampler_bank_count(const app_sampler_bank_t *bank);
bool app_sampler_bank_get_info(const app_sampler_bank_t *bank, unsigned index,
                               app_sampler_sample_info_t *info);
/* Apply saved explicit slot assignments to a staged engine before either commit. */
app_sampler_result_t app_sampler_bank_apply_slots(const app_sampler_bank_t *bank,
                                                  tracker_engine_prepared_t *song);
