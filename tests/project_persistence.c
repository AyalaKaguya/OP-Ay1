// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_project.h"
#include "app_sampler.h"
#include "project_io_faults.h"
#undef fwrite
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void pause_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void pause_ms(unsigned ms) { const struct timespec t = {ms / 1000U, (long)(ms % 1000U) * 1000000L}; nanosleep(&t, NULL); }
#endif

static void check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "project_persistence: FAIL: %s\n", message); exit(EXIT_FAILURE); }
}
static uint8_t *read_bytes(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb"); check(file != NULL, "open fixture");
    check(!fseek(file, 0, SEEK_END), "seek fixture"); long length = ftell(file); check(length > 0, "fixture length");
    check(!fseek(file, 0, SEEK_SET), "rewind fixture");
    uint8_t *bytes = malloc((size_t)length); check(bytes != NULL, "allocate fixture");
    check(fread(bytes, 1, (size_t)length, file) == (size_t)length && !fclose(file), "read fixture");
    *size = (size_t)length; return bytes;
}
static void idle(app_project_t *project, app_project_snapshot_t *snapshot)
{
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        if (app_project_read(project, snapshot) == APP_PROJECT_OK && !snapshot->busy) return;
        pause_ms(5);
    }
    check(false, "worker completion timeout");
}

#define PATH_BYTES 768
#define WAV_FRAMES 1024U

typedef struct {
    tracker_engine_t *tracker;
    app_synth_t *synth;
    app_sampler_t *sampler;
    app_note_router_t *router;
    app_audio_bus_t *bus;
    app_project_t *project;
    app_project_snapshot_t *snapshot;
    const uint8_t *module;
    size_t module_size;
    const char *root;
    app_synth_patch_t expected_patch;
    app_sampler_sample_info_t expected_sample;
    unsigned selected_sample;
    int16_t *expected_wave;
    unsigned expected_wave_count;
    uint8_t *expected_module;
    size_t expected_module_size;
    int pads[16];
    char original_id[33];
    char copy_id[33];
} fixture_t;

static void path_join(char out[PATH_BYTES], const char *a, const char *b)
{
    const int n = snprintf(out, PATH_BYTES, "%s/%s", a, b);
    check(n > 0 && n < PATH_BYTES, "test path overflow");
}
static void write_bytes(const char *path, const void *bytes, size_t size)
{
    FILE *file = fopen(path, "wb"); check(file != NULL, "open output artifact");
    check(fwrite(bytes, 1, size, file) == size && !fclose(file), "write output artifact");
}
static uint32_t u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t crc(const uint8_t *bytes, size_t size)
{
    /* Independent byte-wise remainder implementation; do not call production
     * CRC/state decoder while checking persisted bytes and manifest lengths. */
    uint32_t c = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; ++i) {
        uint32_t value = (c ^ bytes[i]) & 255U;
        for (unsigned j = 0; j < 8; ++j) value = value & 1U ? (value >> 1) ^ UINT32_C(0xedb88320) : value >> 1;
        c = (c >> 8) ^ value;
    }
    return c ^ UINT32_C(0xffffffff);
}
static bool close_float(float a, float b) { return isfinite(a) && isfinite(b) && fabsf(a - b) < 0.00001f; }
static void current_generation(const fixture_t *f, const char *id, const char *which, char out[PATH_BYTES])
{
    char base[PATH_BYTES], path[PATH_BYTES];
    path_join(base, f->root, "projects"); path_join(path, base, id); strcpy(base, path);
    path_join(path, base, which);
    size_t size = 0; uint8_t *pointer = read_bytes(path, &size);
    check(size == 19 && pointer[0] == 'g' && pointer[1] == '-' && pointer[18] == '\n', "CURRENT pointer format");
    pointer[18] = 0; path_join(out, base, (const char *)pointer); free(pointer);
}
static void request_done(fixture_t *f, const app_project_request_t *request, app_project_error_t expected)
{
    const app_project_error_t admitted = app_project_request(f->project, request);
    if (admitted != APP_PROJECT_OK) fprintf(stderr, "admission=%s action=%d\n", app_project_error_name(admitted), request->action);
    check(admitted == APP_PROJECT_OK, "admit worker job");
    idle(f->project, f->snapshot);
    if (f->snapshot->error != expected) fprintf(stderr, "action=%d error=%s wanted=%s\n", request->action,
        app_project_error_name(f->snapshot->error), app_project_error_name(expected));
    check(f->snapshot->error == expected, "worker job result");
}
static void action(fixture_t *f, app_project_action_t which, const char *name)
{
    app_project_request_t request = {0}; request.action = which; request.discard_dirty = true;
    if (name) snprintf(request.name, sizeof(request.name), "%s", name);
    request_done(f, &request, APP_PROJECT_OK);
}
static void render(fixture_t *f)
{
    int16_t *pcm = calloc(1024, sizeof(*pcm)); check(pcm != NULL, "render buffer");
    app_audio_bus_render(f->bus, pcm, 512); free(pcm);
}
static void fixture_start(fixture_t *f)
{
    f->tracker = tracker_engine_create(f->module, f->module_size);
    f->synth = app_synth_create(48000);
    f->sampler = app_sampler_create(); f->router = app_note_router_create(f->synth);
    f->bus = app_audio_bus_create(f->tracker, f->synth);
    check(f->tracker && f->synth && f->sampler && f->router && f->bus, "create real sources");
    check(app_sampler_bind_engine(f->sampler, f->tracker) && app_audio_bus_set_sampler(f->bus, f->sampler), "wire sampler");
    const app_project_config_t config = {f->tracker, f->synth, f->bus, f->sampler, f->router,
        f->module, f->module_size, f->root};
    check(app_project_create(&config, &f->project) == APP_PROJECT_OK, "create service");
    idle(f->project, f->snapshot);
    check(f->snapshot->error == APP_PROJECT_OK && f->snapshot->dirty && f->snapshot->opened &&
          f->snapshot->storage_ready && !f->snapshot->storage_usage_valid && !f->snapshot->id[0], "unsaved ARYX boot");
    check(app_project_inbox_ready(f->project) && f->snapshot->sample_count == app_sampler_count(f->sampler), "inbox and sampler readiness");
    render(f);
}
static void fixture_stop(fixture_t *f)
{
    check(app_project_shutdown(&f->project, 10000) == APP_PROJECT_OK && !f->project, "drain and shutdown worker");
    app_audio_bus_destroy(f->bus); app_note_router_destroy(f->router);
    app_sampler_destroy(f->sampler); app_synth_destroy(f->synth); tracker_engine_destroy(f->tracker);
    f->bus = NULL; f->router = NULL; f->sampler = NULL; f->synth = NULL; f->tracker = NULL;
}
static void edit_complete_state(fixture_t *f)
{
    check(app_project_begin_edit(f->project), "begin state edit");
    f->expected_patch = app_synth_patch_for_preset(APP_SYNTH_PATCH_LEAD);
    f->expected_patch.waveform = APP_SYNTH_WAVE_WAVETABLE;
    f->expected_patch.wavetable_position = 0.321f; f->expected_patch.pan = -0.35f;
    f->expected_patch.detune_cents = 14; f->expected_patch.attack_s = 0.123f;
    f->expected_patch.release_s = 0.456f; f->expected_patch.lfo_target = APP_SYNTH_LFO_TARGET_CUTOFF;
    f->expected_patch.lfo_depth = 0.6f;
    check(app_synth_set_patch(f->synth, &f->expected_patch), "set edited patch");
    for (int ch = 0; ch < 16; ++ch) {
        tracker_engine_set_channel_gain(f->tracker, ch, (float)ch / 10.0f);
        tracker_engine_set_channel_mute(f->tracker, ch, ch % 3 == 0);
        tracker_engine_set_channel_solo(f->tracker, ch, ch == 1);
        f->pads[ch] = ch < 12 ? 11 - ch : -1;
    }
    for (int source = 0; source < APP_AUDIO_SOURCE_COUNT; ++source) {
        check(app_audio_bus_set_source_gain(f->bus, (app_audio_source_t)source, 0.5f + 0.25f * source), "set source gain");
    }
    tracker_engine_set_loop(f->tracker, true); tracker_engine_set_speed_multiplier(f->tracker, 1.5f);
    tracker_engine_set_pattern_at(f->tracker, 1, 2);
    check(app_project_set_controls(f->project, 65, f->pads), "persist UI controls inside edit admission");
    app_project_end_edit(f->project, true);
    render(f);
    app_sampler_sample_info_t info = {0};
    f->selected_sample = UINT32_MAX;
    for (unsigned i = 0; i < app_sampler_count(f->sampler); ++i) {
        check(app_sampler_get_info(f->sampler, i, &info), "read sample metadata");
        if (info.frames > 128) { f->selected_sample = i; break; }
    }
    check(f->selected_sample != UINT32_MAX, "fixture has an editable real sample");
    check(app_project_begin_edit(f->project), "begin sampler preview edit");
    check(app_sampler_edit_begin(f->sampler, f->selected_sample) == APP_SAMPLER_OK, "select sample edit");
    app_sampler_edit_t edit = {10, info.frames - 10, 20, info.frames - 20, 11025, 0.65f, true};
    check(app_sampler_edit_set(f->sampler, &edit) == APP_SAMPLER_OK, "stage sample trim/loop/gain/rate");
    app_project_end_edit(f->project, false);
    action(f, APP_PROJECT_APPLY_SAMPLE, NULL);
    check(app_sampler_get_info(f->sampler, f->selected_sample, &f->expected_sample), "read applied sample");
    f->expected_wave = malloc(512 * sizeof(*f->expected_wave)); check(f->expected_wave != NULL, "wave evidence allocation");
    f->expected_wave_count = app_sampler_waveform(f->sampler, f->selected_sample, f->expected_wave, 512);
    check(f->expected_wave_count > 0, "actual sample PCM evidence");
    check(tracker_engine_copy_blob(f->tracker, &f->expected_module, &f->expected_module_size), "copy edited order overlay");
}
static void compare_patch(fixture_t *f)
{
    app_synth_patch_t patch; check(app_synth_get_patch(f->synth, &patch), "read restored patch");
    check(patch.waveform == f->expected_patch.waveform && patch.lfo_target == f->expected_patch.lfo_target,
          "synth enums roundtrip");
    const float actual[] = {patch.wavetable_position,patch.detune_cents,patch.gain,patch.pan,patch.filter_cutoff_hz,
        patch.filter_resonance,patch.attack_s,patch.decay_s,patch.sustain,patch.release_s,patch.lfo_hz,patch.lfo_depth};
    const app_synth_patch_t *e = &f->expected_patch;
    const float expected[] = {e->wavetable_position,e->detune_cents,e->gain,e->pan,e->filter_cutoff_hz,
        e->filter_resonance,e->attack_s,e->decay_s,e->sustain,e->release_s,e->lfo_hz,e->lfo_depth};
    for (unsigned i = 0; i < 12; ++i) check(close_float(actual[i], expected[i]), "synth scalar roundtrip");
}
static void compare_restored(fixture_t *f)
{
    compare_patch(f);
    check(f->snapshot->master_volume == 65 && !memcmp(f->snapshot->pad_patterns, f->pads, sizeof(f->pads)), "master and pads roundtrip");
    check(close_float(tracker_engine_speed_multiplier(f->tracker), 1.5f) && tracker_engine_loop_enabled(f->tracker), "loop/speed roundtrip");
    check(tracker_engine_pattern_at(f->tracker, 1) == 2 && !tracker_engine_is_playing(f->tracker), "orders persisted; load stays paused");
    for (int ch = 0; ch < 16; ++ch) {
        check(close_float(tracker_engine_channel_gain(f->tracker, ch), (float)ch / 10.0f) &&
              tracker_engine_channel_mute(f->tracker, ch) == (ch % 3 == 0) &&
              tracker_engine_channel_solo(f->tracker, ch) == (ch == 1), "mixer gain/mute/solo roundtrip");
    }
    for (int source = 0; source < APP_AUDIO_SOURCE_COUNT; ++source) {
        check(close_float(app_audio_bus_source_gain(f->bus, (app_audio_source_t)source), 0.5f + 0.25f * source), "source gain roundtrip");
    }
    app_sampler_sample_info_t info = {0}; check(app_sampler_get_info(f->sampler, f->selected_sample, &info), "restored sample exists");
    check(info.frames == f->expected_sample.frames && info.trim_start == 10 &&
          info.trim_end == f->expected_sample.trim_end && info.loop_enabled && info.loop_start == 20 &&
          info.loop_end == f->expected_sample.loop_end && info.c2_speed == 11025 &&
          close_float(info.gain, 0.65f) && !strcmp(info.name, f->expected_sample.name) &&
          info.assigned_slot == f->expected_sample.assigned_slot, "sample metadata/slot roundtrip");
    int16_t *wave = malloc(512 * sizeof(*wave)); check(wave != NULL, "wave buffer");
    check(app_sampler_waveform(f->sampler, f->selected_sample, wave, 512) == f->expected_wave_count &&
          !memcmp(wave, f->expected_wave, f->expected_wave_count * sizeof(*wave)), "actual PCM roundtrip"); free(wave);
    uint8_t *module = NULL; size_t size = 0; check(tracker_engine_copy_blob(f->tracker, &module, &size), "restored module export");
    check(size == f->expected_module_size && !memcmp(module, f->expected_module, size), "contained module/order bytes roundtrip"); free(module);
}
static void independent_artifacts(fixture_t *f, const char *id)
{
    char generation[PATH_BYTES], path[PATH_BYTES]; current_generation(f, id, "CURRENT", generation);
    path_join(path, generation, "manifest.bin"); size_t size = 0; uint8_t *manifest = read_bytes(path, &size);
    check(size == 192 && !memcmp(manifest, "OPAYPRJ1", 8) && u32(manifest + 8) == 1 &&
          crc(manifest, 188) == u32(manifest + 188) && !strcmp((const char *)manifest + 120, id), "independent manifest CRC/ID");
    const char *names[3] = {"module.s3m", "samples.bank", "state.bin"};
    const uint64_t lengths[3] = {u64(manifest + 28),u64(manifest + 36),384};
    for (unsigned i = 0; i < 3; ++i) {
        path_join(path, generation, names[i]); uint8_t *data = read_bytes(path, &size);
        check(size == lengths[i] && crc(data, size) == u32(manifest + 44 + i * 4), "independent asset length/CRC");
        if (i == 0) check(size == f->expected_module_size && !memcmp(data, f->expected_module, size), "actual saved module bytes");
        if (i == 1) {
            check(size >= 16 && !memcmp(data, "AYSB", 4) && u32(data + 4) == 1 && u32(data + 12) == size, "contained PCM bank format");
            size_t cursor = 16; bool saw_pcm = false;
            for (unsigned index = 0; index < u32(data + 8); ++index) {
                check(cursor <= size && 120 <= size - cursor, "bank metadata bounds");
                const uint8_t *meta = data + cursor; const uint32_t frames = u32(meta + 68); cursor += 120;
                check(frames <= (size - cursor) / 2, "bank PCM bounds");
                if (index == f->selected_sample) {
                    check(!strcmp((const char *)meta, f->expected_sample.name) && u32(meta + 80) == 10 &&
                          u32(meta + 76) == 11025 && u32(meta + 108) == f->expected_sample.assigned_slot,
                          "independent sample metadata values");
                    for (unsigned point = 0; point < f->expected_wave_count; ++point) {
                        const uint32_t at = (uint32_t)((uint64_t)point * frames / f->expected_wave_count);
                        const int16_t value = (int16_t)((uint16_t)data[cursor + at * 2] | (uint16_t)data[cursor + at * 2 + 1] << 8);
                        check(value == f->expected_wave[point], "independently reread saved PCM values");
                    }
                    saw_pcm = true;
                }
                cursor += (size_t)frames * 2;
            }
            check(cursor == size && saw_pcm, "complete contained bank");
        }
        if (i == 2) check(!memcmp(data, "OPSTATE1", 8) && u32(data + 72) == 65 && u32(data + 192) == 11,
                          "independent saved controls");
        free(data);
    }
    free(manifest);
}
static void save_and_reopen(fixture_t *f)
{
    edit_complete_state(f); action(f, APP_PROJECT_SAVE, "Roundtrip");
    strcpy(f->original_id, f->snapshot->id); independent_artifacts(f, f->original_id);
    check(!f->snapshot->dirty && f->snapshot->entry_count == 1, "save clean/catalog");
    const uint64_t generation = f->snapshot->session_generation;
    const uint64_t saved_revision = f->snapshot->revision;
    tracker_engine_play(f->tracker); render(f);
    pause_ms(150); idle(f->project, f->snapshot);
    check(!f->snapshot->dirty && f->snapshot->revision == saved_revision,
          "transport/audio/meter activity does not dirty musical project");
    check(app_project_note_edit(f->project), "dirty mark");
    app_project_request_t request = {0}; request.action = APP_PROJECT_OPEN; strcpy(request.id, f->original_id);
    check(app_project_request(f->project, &request) == APP_PROJECT_OK, "same ID open no-op admission");
    idle(f->project, f->snapshot);
    check(f->snapshot->dirty && f->snapshot->session_generation == generation && tracker_engine_is_playing(f->tracker),
          "same ID open preserved dirty session and playing transport");
    request.action = APP_PROJECT_NEW_BLANK;
    check(app_project_request(f->project, &request) == APP_PROJECT_DIRTY_CONFLICT, "new requires dirty confirmation");
    action(f, APP_PROJECT_SAVE_AS, "Copy"); strcpy(f->copy_id, f->snapshot->id);
    check(strcmp(f->copy_id, f->original_id) && f->snapshot->entry_count == 2, "Save As new ID preserves original");
    action(f, APP_PROJECT_NEW_BLANK, "Blank");
    check(f->snapshot->dirty && !f->snapshot->id[0] && f->snapshot->sample_count == 16 && tracker_engine_patterns(f->tracker) == 1,
          "blank session structure");
    for (unsigned index = 0; index < 16; ++index) {
        app_sampler_sample_info_t info = {0}; check(app_sampler_get_info(f->sampler, index, &info) && !info.frames && !info.assigned_slot,
                                               "blank has sixteen actual EMPTY slots");
    }
    tracker_engine_play(f->tracker);
    int16_t *audio = calloc(1024, sizeof(*audio)); check(audio != NULL, "blank audio buffer");
    app_audio_bus_render(f->bus, audio, 512);
    for (unsigned i = 0; i < 1024; ++i) check(audio[i] == 0, "blank project contains no demo sound"); free(audio);
    memset(&request, 0, sizeof(request)); request.action = APP_PROJECT_OPEN; strcpy(request.id, f->original_id);
    check(app_project_request(f->project, &request) == APP_PROJECT_DIRTY_CONFLICT, "open requires dirty confirmation");
    request.discard_dirty = true; request_done(f, &request, APP_PROJECT_OK); compare_restored(f);
    puts("project_persistence: full musical state/PCM roundtrip, blank, same-ID and Save As PASS");
}
static void inbox_imports(fixture_t *f)
{
    char inbox[PATH_BYTES], path[PATH_BYTES]; path_join(inbox, f->root, "inbox");
    path_join(path, inbox, "import.s3m"); write_bytes(path, f->module, f->module_size);
    action(f, APP_PROJECT_REFRESH, NULL); check(f->snapshot->inbox_count == 1, "actual S3M inbox catalog");
    app_project_request_t request = {0}; request.action = APP_PROJECT_IMPORT_MODULE; request.discard_dirty = true;
    strcpy(request.filename, "../bad.s3m"); check(app_project_request(f->project, &request) == APP_PROJECT_INVALID, "reject path traversal");
    strcpy(request.filename, "C:\\bad.s3m"); check(app_project_request(f->project, &request) == APP_PROJECT_INVALID, "reject external device path");
    strcpy(request.filename, "import.s3m"); request_done(f, &request, APP_PROJECT_OK);
    check(f->snapshot->dirty && !f->snapshot->id[0] && f->snapshot->sample_count > 0, "S3M creates contained unsaved session");
    path_join(path, inbox, "import.s3m"); check(remove(path) == 0, "remove imported inbox source");
    action(f, APP_PROJECT_SAVE_AS, "Imported module");
    char imported_id[33]; strcpy(imported_id, f->snapshot->id);
    action(f, APP_PROJECT_NEW_BLANK, "WAV bank");
    uint8_t *wav = calloc(1, 44U + WAV_FRAMES * 2U); check(wav != NULL, "WAV fixture");
    memcpy(wav, "RIFF", 4); put32(wav + 4, 36U + WAV_FRAMES * 2U); memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16); put16(wav + 20, 1); put16(wav + 22, 1); put32(wav + 24, 48000);
    put32(wav + 28, 96000); put16(wav + 32, 2); put16(wav + 34, 16); memcpy(wav + 36, "data", 4);
    put32(wav + 40, WAV_FRAMES * 2U);
    for (unsigned frame = 0; frame < WAV_FRAMES; ++frame) put16(wav + 44 + frame * 2U, (uint16_t)(int16_t)((int)(frame % 97) * 200 - 9600));
    path_join(path, inbox, "actual.wav"); write_bytes(path, wav, 44U + WAV_FRAMES * 2U); free(wav);
    memset(&request, 0, sizeof(request)); request.action = APP_PROJECT_IMPORT_SAMPLE; strcpy(request.filename, "actual.wav");
    request_done(f, &request, APP_PROJECT_OK);
    check(app_sampler_count(f->sampler) == 17 && f->snapshot->dirty, "WAV appended real PCM");
    app_sampler_sample_info_t info = {0}; check(app_sampler_get_info(f->sampler, 16, &info) && info.frames == WAV_FRAMES &&
        info.source == APP_SAMPLER_SOURCE_WAV && info.sample_rate == 48000, "WAV format preserved");
    int16_t *wave = malloc(WAV_FRAMES * sizeof(*wave)); check(wave != NULL, "PCM verifier");
    check(app_sampler_waveform(f->sampler, 16, wave, WAV_FRAMES) == WAV_FRAMES, "WAV complete PCM readable");
    for (unsigned frame = 0; frame < WAV_FRAMES; ++frame) check(wave[frame] == (int)(frame % 97) * 200 - 9600, "WAV decoded PCM expected");
    request.action = APP_PROJECT_ASSIGN_SAMPLE; request.sample_index = 16; request.target_slot = 1;
    request_done(f, &request, APP_PROJECT_OK); check(app_sampler_get_info(f->sampler, 16, &info) && info.assigned_slot == 1, "blank slot assignment");
    action(f, APP_PROJECT_SAVE_AS, "Contained WAV"); char wav_id[33]; strcpy(wav_id, f->snapshot->id);
    check(remove(path) == 0, "delete WAV inbox source after save");
    action(f, APP_PROJECT_IMPORT_ARYX, "Fresh demo");
    memset(&request, 0, sizeof(request)); request.action = APP_PROJECT_OPEN; request.discard_dirty = true; strcpy(request.id, wav_id);
    request_done(f, &request, APP_PROJECT_OK);
    check(app_sampler_waveform(f->sampler, 16, wave, WAV_FRAMES) == WAV_FRAMES, "contained WAV reopens without inbox");
    for (unsigned frame = 0; frame < WAV_FRAMES; ++frame) check(wave[frame] == (int)(frame % 97) * 200 - 9600, "reopened WAV PCM expected");
    free(wave);
    action(f, APP_PROJECT_IMPORT_ARYX, NULL); strcpy(request.id, imported_id); request_done(f, &request, APP_PROJECT_OK);
    check(app_sampler_count(f->sampler) > 0, "contained S3M reopens without inbox");
    puts("project_persistence: bounded inbox S3M/WAV imports, EMPTY-slot assignment and offline containment PASS");
}
static void failure_protection(fixture_t *f)
{
    app_project_request_t request = {0}; request.action = APP_PROJECT_OPEN; request.discard_dirty = true;
    strcpy(request.id, f->original_id); request_done(f, &request, APP_PROJECT_OK);
    check(app_project_begin_edit(f->project), "edit admission");
    app_project_request_t save = {0}; save.action = APP_PROJECT_SAVE;
    check(app_project_request(f->project, &save) == APP_PROJECT_BUSY && !app_project_begin_edit(f->project), "edit blocks job/second edit");
    app_project_end_edit(f->project, true); idle(f->project, f->snapshot);
    char before_generation[PATH_BYTES], after_generation[PATH_BYTES]; current_generation(f, f->original_id, "CURRENT", before_generation);
    const uint64_t session = f->snapshot->session_generation;
    project_test_fail_next_write(); request_done(f, &save, APP_PROJECT_NO_SPACE);
    current_generation(f, f->original_id, "CURRENT", after_generation);
    check(!strcmp(before_generation, after_generation) && f->snapshot->dirty && f->snapshot->session_generation == session,
          "no-space save retained old committed files/current session/dirty edits");
    action(f, APP_PROJECT_SAVE, NULL);
    current_generation(f, f->original_id, "PREVIOUS", before_generation);
    current_generation(f, f->original_id, "CURRENT", after_generation);
    check(strcmp(before_generation, after_generation), "successful second generation retained previous");
    char bad_path[PATH_BYTES]; path_join(bad_path, after_generation, "module.s3m");
    size_t length = 0; uint8_t *old = read_bytes(bad_path, &length); old[0] ^= 0x55; write_bytes(bad_path, old, length); free(old);
    action(f, APP_PROJECT_NEW_BLANK, NULL);
    request_done(f, &request, APP_PROJECT_OK); compare_restored(f);
    path_join(bad_path, before_generation, "module.s3m"); old = read_bytes(bad_path, &length); old[1] ^= 0x11; write_bytes(bad_path, old, length); free(old);
    action(f, APP_PROJECT_NEW_BLANK, "Retain me");
    idle(f->project, f->snapshot); const uint64_t blank_generation = f->snapshot->session_generation;
    request_done(f, &request, APP_PROJECT_CORRUPT);
    check(f->snapshot->session_generation == blank_generation && !strcmp(f->snapshot->name, "Retain me") &&
          f->snapshot->dirty && tracker_engine_patterns(f->tracker) == 1, "two corrupt generations preserved current session");
    strcpy(request.id, "ffffffffffffffffffffffffffffffff"); request_done(f, &request, APP_PROJECT_NOT_FOUND);
    check(f->snapshot->session_generation == blank_generation, "missing project preserved current session");
    request.id[0] = 'x'; check(app_project_request(f->project, &request) == APP_PROJECT_INVALID, "invalid ID rejected before IO");
    action(f, APP_PROJECT_SAVE_AS, "Drain pending job");
    char first_id[33]; strcpy(first_id, f->snapshot->id);
    char first_generation[PATH_BYTES]; current_generation(f, first_id, "CURRENT", first_generation);
    path_join(bad_path, first_generation, "samples.bank"); old = read_bytes(bad_path, &length);
    old[0] ^= 0x01; write_bytes(bad_path, old, length); free(old);
    action(f, APP_PROJECT_NEW_BLANK, "Still active");
    memset(&request, 0, sizeof(request)); request.action = APP_PROJECT_OPEN; request.discard_dirty = true;
    strcpy(request.id, first_id); request_done(f, &request, APP_PROJECT_CORRUPT);
    check(!strcmp(f->snapshot->name, "Still active") && f->snapshot->dirty,
          "corrupt first generation reported distinctly and preserved active project");
    action(f, APP_PROJECT_SAVE_AS, "Drain pending job");
    project_test_hold_writes(true); check(app_project_request(f->project, &save) == APP_PROJECT_OK, "pending save admission");
    for (unsigned attempt = 0; attempt < 2000 && !project_test_write_is_held(); ++attempt) pause_ms(1);
    check(project_test_write_is_held(), "IO hold entered");
    check(!app_project_begin_edit(f->project) && app_project_request(f->project, &save) == APP_PROJECT_BUSY, "in-flight save blocks edits/jobs");
    check(app_project_shutdown(&f->project, 0) == APP_PROJECT_TIMEOUT && f->project != NULL, "shutdown timeout retains owner and sources");
    project_test_hold_writes(false);
    check(app_project_shutdown(&f->project, 10000) == APP_PROJECT_OK && f->project == NULL, "shutdown drains admitted save");
    puts("project_persistence: edit admission, no-space fault, CRC fallback, load failure and drain/timeout ownership PASS");
}
static void restart_and_pointer_recovery(fixture_t *f)
{
    fixture_start(f);
    app_project_request_t request = {0}; request.action = APP_PROJECT_OPEN;
    request.discard_dirty = true; strcpy(request.id, f->copy_id);
    request_done(f, &request, APP_PROJECT_OK); compare_restored(f);
    check(!f->snapshot->dirty && !strcmp(f->snapshot->name, "Copy"), "independent service restart restored saved copy");
    action(f, APP_PROJECT_SAVE, NULL);
    char previous[PATH_BYTES], base[PATH_BYTES], pointer[PATH_BYTES];
    current_generation(f, f->copy_id, "PREVIOUS", previous);
    path_join(base, f->root, "projects"); path_join(pointer, base, f->copy_id); strcpy(base, pointer);
    path_join(pointer, base, "CURRENT"); write_bytes(pointer, "damaged pointer", 15);
    action(f, APP_PROJECT_NEW_BLANK, NULL);
    request_done(f, &request, APP_PROJECT_OK); compare_restored(f);
    const uint64_t revision = f->snapshot->revision;
    check(app_project_begin_edit(f->project), "begin autonomous sample mutation");
    check(app_sampler_edit_begin(f->sampler, f->selected_sample) == APP_SAMPLER_OK, "select autonomous sample edit");
    app_sampler_edit_t edit;
    check(app_sampler_edit_get(f->sampler, &edit), "read sample edit"); edit.gain = 0.4f;
    check(app_sampler_edit_set(f->sampler, &edit) == APP_SAMPLER_OK &&
          app_sampler_edit_apply(f->sampler) == APP_SAMPLER_OK, "apply autonomous persistent sample mutation");
    app_project_end_edit(f->project, false);
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        idle(f->project, f->snapshot);
        if (f->snapshot->revision > revision) break;
        pause_ms(5);
    }
    check(f->snapshot->dirty && f->snapshot->revision > revision,
          "sampler generation automatically dirtied project without explicit note_edit");
    puts("project_persistence: new service restart, corrupt CURRENT fallback and autonomous bank revision PASS");
}
int main(int argc, char **argv)
{
    check(argc == 3, "usage fixture root");
    fixture_t *f = calloc(1, sizeof(*f)); check(f != NULL, "test context allocation");
    f->snapshot = calloc(1, sizeof(*f->snapshot)); check(f->snapshot != NULL, "snapshot allocation");
    f->module = read_bytes(argv[1], &f->module_size); f->root = argv[2];
    fixture_start(f); save_and_reopen(f); inbox_imports(f); failure_protection(f);
    /* failure_protection already drained and stopped the worker; public
     * idempotent shutdown permits the common owned-source cleanup path. */
    fixture_stop(f);
    restart_and_pointer_recovery(f); fixture_stop(f);
    free(f->expected_module); free(f->expected_wave); free((void *)f->module); free(f->snapshot); free(f);
    puts("project_persistence: PASS"); return EXIT_SUCCESS;
}
