// SPDX-License-Identifier: Apache-2.0
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_sampler.h"
#include "app_audio_bus.h"
#include "sampler_fixture.h"

static unsigned checks;
#define REQUIRE(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "sampler FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)
static bool near(float a, float b)
{ return isfinite(a) && fabsf(a - b) < 0.00001f; }
extern void sampler_concurrency(const uint8_t *module, size_t size);

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb"); REQUIRE(file != NULL);
    REQUIRE(fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file); REQUIRE(length > 0 && length < 32 * 1024 * 1024);
    REQUIRE(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length); REQUIRE(bytes != NULL);
    REQUIRE(fread(bytes, 1, (size_t)length, file) == (size_t)length); REQUIRE(fclose(file) == 0);
    *size = (size_t)length; return bytes;
}
static app_sampler_t *sampler_from(const uint8_t *module, size_t size)
{
    app_sampler_result_t result = APP_SAMPLER_INVALID;
    app_sampler_bank_t *bank = app_sampler_bank_from_s3m(module, size, &result);
    REQUIRE(bank != NULL && result == APP_SAMPLER_OK);
    app_sampler_t *s = app_sampler_create(); REQUIRE(s != NULL);
    app_sampler_bank_commit(s, bank); return s;
}

static void aryx_metadata(const uint8_t *bytes, size_t size)
{
    /* Constants were independently decoded from the checked-in original file,
     * not produced by app_sampler's parser or serialization. */
    static const uint32_t lengths[] = {174, 173, 873, 950, 946, 942, 900, 173, 0};
    static const uint32_t rates[] = {44444, 44734, 6531, 9363, 14386, 4555, 22054, 44125, 8363};
    static const uint32_t offsets[] = {15616, 15792, 15968, 16848, 17808, 18768, 19712, 20624, 0};
    static const uint32_t loop_starts[] = {7, 7, 0, 0, 7, 0, 10, 7, 0};
    static const unsigned volumes[] = {64, 64, 64, 45, 50, 50, 64, 40, 64};
    static const bool loops[] = {true, true, false, false, true, false, true, true, false};
    REQUIRE(size == 20800);
    app_sampler_t *s = sampler_from(bytes, size);
    REQUIRE(app_sampler_count(s) == 9 && app_sampler_revision(s) == 1);
    int16_t pcm[1024]; unsigned total = 0;
    for (unsigned index = 0; index < 9; ++index) {
        app_sampler_sample_info_t info; REQUIRE(app_sampler_get_info(s, index, &info));
        REQUIRE(info.frames == lengths[index] && info.sample_rate == rates[index] && info.c2_speed == rates[index]);
        REQUIRE(info.source == APP_SAMPLER_SOURCE_S3M && info.original_slot == index + 1U);
        REQUIRE(info.assigned_slot == (index < 8 ? index + 1U : 0));
        REQUIRE(info.trim_start == 0 && info.trim_end == lengths[index]);
        REQUIRE(info.loop_enabled == loops[index] && info.loop_start == loop_starts[index]);
        REQUIRE(near(info.default_volume, volumes[index] / 64.0f) && info.gain == 1.0f);
        REQUIRE(app_sampler_waveform(s, index, pcm, 1024) == lengths[index]);
        for (unsigned frame = 0; frame < lengths[index]; ++frame) {
            REQUIRE(pcm[frame] == ((int)bytes[offsets[index] + frame] - 128) * 256);
        }
        total += info.frames;
    }
    REQUIRE(total == 5131);
    app_sampler_sample_info_t info; REQUIRE(app_sampler_get_info(s, 0, &info));
    REQUIRE(strcmp(info.name, "k.koch 29.03.95") == 0);
    REQUIRE(app_sampler_get_info(s, 2, &info) && strcmp(info.name, "Time: 2:21") == 0);
    REQUIRE(app_sampler_edit_begin(s, 8) == APP_SAMPLER_BAD_STATE);
    REQUIRE(!app_sampler_get_info(s, 9, &info));
    app_sampler_destroy(s);
    puts("sampler: ARYX exact 9 slots / 8 PCM / 5131 frames, loops, rates and original volume PASS");
}

static void wav_cases(void)
{
    const int16_t mono8[] = {-32768, 0, 32512, -16384};
    const int16_t mono16[] = {-32768, -1, 0, 32767};
    const int16_t stereo8[] = {-32768, 32512, 0, 0, -32768, -32768, 32512, 32512};
    const int16_t stereo16[] = {-32768, 32767, -100, 0, 32767, 32767, -32768, -32768};
    const int16_t expected8[] = {-128, 0, -32768, 32512};
    const int16_t expected16[] = {0, -50, 32767, -32768};
    const int16_t *inputs[] = {mono8, mono16, stereo8, stereo16};
    const int16_t *expected[] = {mono8, mono16, expected8, expected16};
    app_sampler_t *s = app_sampler_create(); REQUIRE(s != NULL);
    uint8_t wav[256]; int16_t pcm[8]; unsigned index = 99;
    for (unsigned test = 0; test < 4; ++test) {
        const unsigned bits = (test & 1U) ? 16U : 8U, channels = test < 2 ? 1U : 2U;
        const size_t size = fixture_wav(wav, bits, channels, test & 1U ? 48000U : 44100U, inputs[test], 4);
        REQUIRE(app_sampler_import_wav(s, wav, size, "some/folder/example.wav", &index) == APP_SAMPLER_OK);
        REQUIRE(index == test && app_sampler_waveform(s, index, pcm, 8) == 4);
        REQUIRE(memcmp(pcm, expected[test], 4 * sizeof(int16_t)) == 0);
        app_sampler_sample_info_t info; REQUIRE(app_sampler_get_info(s, index, &info));
        REQUIRE(!strcmp(info.name, "example.wav") && info.source == APP_SAMPLER_SOURCE_WAV);
        REQUIRE(info.default_volume == 1.0f && info.gain == 1.0f && !info.assigned_slot);
    }
    const uint64_t revision = app_sampler_revision(s);
    const size_t size = fixture_wav(wav, 16, 1, 48000, mono16, 4);
    uint8_t bad[256];
    memcpy(bad, wav, size); fixture_u32(bad + 40, 10000);
    REQUIRE(app_sampler_import_wav(s, bad, size, NULL, NULL) == APP_SAMPLER_FORMAT);
    memcpy(bad, wav, size); fixture_u16(bad + 20, 3);
    REQUIRE(app_sampler_import_wav(s, bad, size, NULL, NULL) == APP_SAMPLER_FORMAT);
    memcpy(bad, wav, size); fixture_u16(bad + 32, 4);
    REQUIRE(app_sampler_import_wav(s, bad, size, NULL, NULL) == APP_SAMPLER_FORMAT);
    memcpy(bad, wav, size); fixture_u32(bad + 24, 1000);
    REQUIRE(app_sampler_import_wav(s, bad, size, NULL, NULL) == APP_SAMPLER_FORMAT);
    REQUIRE(app_sampler_import_wav(s, wav, size - 1U, NULL, NULL) == APP_SAMPLER_FORMAT);
    memcpy(bad, wav, size); memcpy(bad + size, "data", 4); fixture_u32(bad + size + 4U, 0);
    fixture_u32(bad + 4, (uint32_t)size);
    REQUIRE(app_sampler_import_wav(s, bad, size + 8U, NULL, NULL) == APP_SAMPLER_FORMAT);
    memcpy(bad, wav, size); bad[size] = 0xff; fixture_u32(bad + 4, (uint32_t)size - 7U);
    REQUIRE(app_sampler_import_wav(s, bad, size + 1U, NULL, NULL) == APP_SAMPLER_FORMAT);
    REQUIRE(app_sampler_count(s) == 4 && app_sampler_revision(s) == revision);
    REQUIRE(app_sampler_import_wav_file(s, "missing-file.wav", NULL) == APP_SAMPLER_IO);
    app_sampler_destroy(s);
    puts("sampler: PCM8/16 mono/stereo conversion and malformed WAV rejection PASS");
}

static void streamed_file(void)
{
    const unsigned frames = 1025;
    int16_t *values = malloc(frames * 2U * sizeof(int16_t));
    uint8_t *wav = malloc(44U + frames * 4U); REQUIRE(values && wav);
    for (unsigned i = 0; i < frames; ++i) { values[i * 2U] = (int16_t)(i * 12); values[i * 2U + 1U] = (int16_t)(i * 4); }
    const size_t size = fixture_wav(wav, 16, 2, 44100, values, frames);
    FILE *file = fopen("streamed.wav", "wb"); REQUIRE(file != NULL);
    REQUIRE(fwrite(wav, 1, size, file) == size && fclose(file) == 0);
    app_sampler_t *s = app_sampler_create(); REQUIRE(s != NULL);
    unsigned index; REQUIRE(app_sampler_import_wav_file(s, "streamed.wav", &index) == APP_SAMPLER_OK);
    int16_t *decoded = malloc(frames * sizeof(int16_t)); REQUIRE(decoded != NULL);
    REQUIRE(app_sampler_waveform(s, index, decoded, frames) == frames);
    for (unsigned i = 0; i < frames; ++i) { REQUIRE(decoded[i] == (int16_t)(i * 8)); }
    app_sampler_destroy(s); free(decoded); free(values); free(wav); REQUIRE(remove("streamed.wav") == 0);
    puts("sampler: streamed stereo file conversion across 256-frame chunks PASS");
}

static void loop_and_limits(void)
{
    app_sampler_t *s = app_sampler_create(); REQUIRE(s != NULL);
    uint8_t wav[64]; const int16_t pcm[] = {0, 8192, 16384, 24576}; unsigned index;
    const size_t size = fixture_wav(wav, 16, 1, 48000, pcm, 4);
    REQUIRE(app_sampler_import_wav(s, wav, size, "ramp", &index) == APP_SAMPLER_OK);
    REQUIRE(app_sampler_audition(s, index) == APP_SAMPLER_OK);
    float output[20]; app_sampler_render(s, output, 10);
    for (unsigned frame = 0; frame < 10; ++frame) {
        const float expected = frame < 4 ? frame * 0.25f : 0;
        REQUIRE(near(output[frame * 2U], expected) && near(output[frame * 2U + 1U], expected));
    }
    REQUIRE(app_sampler_edit_begin(s, index) == APP_SAMPLER_OK);
    app_sampler_edit_t edit; REQUIRE(app_sampler_edit_get(s, &edit));
    edit.c2_speed = 24000; edit.loop_enabled = true; edit.loop_start = 1; edit.loop_end = 3;
    REQUIRE(app_sampler_edit_set(s, &edit) == APP_SAMPLER_OK && app_sampler_edit_apply(s) == APP_SAMPLER_OK);
    REQUIRE(app_sampler_audition(s, index) == APP_SAMPLER_OK); app_sampler_render(s, output, 10);
    const float expected[] = {0, .125f, .25f, .375f, .5f, .375f, .25f, .375f, .5f, .375f};
    for (unsigned frame = 0; frame < 10; ++frame) {
        REQUIRE(near(output[frame * 2U], expected[frame]) && near(output[frame * 2U + 1U], expected[frame]));
    }
    for (unsigned i = 1; i < APP_SAMPLER_MAX_SAMPLES; ++i) {
        REQUIRE(app_sampler_import_wav(s, wav, size, "same PCM, independent slot", NULL) == APP_SAMPLER_OK);
    }
    const uint64_t revision = app_sampler_revision(s);
    REQUIRE(app_sampler_import_wav(s, wav, size, "too many", NULL) == APP_SAMPLER_FULL);
    REQUIRE(app_sampler_count(s) == APP_SAMPLER_MAX_SAMPLES && app_sampler_revision(s) == revision);
    app_sampler_destroy(s);

    const uint32_t frames = APP_SAMPLER_MAX_FRAMES + 1U;
    const size_t bytes = 44U + (size_t)frames * 2U;
    uint8_t *huge = calloc(1, bytes); REQUIRE(huge != NULL);
    memcpy(huge, wav, 44); fixture_u32(huge + 4, (uint32_t)bytes - 8U); fixture_u32(huge + 40, frames * 2U);
    s = app_sampler_create(); REQUIRE(s != NULL);
    REQUIRE(app_sampler_import_wav(s, huge, bytes, "over frame cap", NULL) == APP_SAMPLER_RANGE);
    const uint32_t slow_frames = 8000U * 60U + 1U;
    fixture_u32(huge + 24, 8000U); fixture_u32(huge + 28, 16000U);
    fixture_u32(huge + 4, 36U + slow_frames * 2U); fixture_u32(huge + 40, slow_frames * 2U);
    REQUIRE(app_sampler_import_wav(s, huge, 44U + (size_t)slow_frames * 2U,
                                   "over source duration cap", NULL) == APP_SAMPLER_RANGE);
    REQUIRE(app_sampler_count(s) == 0 && app_sampler_revision(s) == 0);
    free(huge); app_sampler_destroy(s);
    puts("sampler: independently expected loop interpolation, one-shot tail, 64 slots and frame/duration limits PASS");
}

static void editing_and_render(void)
{
    uint8_t song[1024]; fixture_song(song);
    tracker_engine_t *engine = tracker_engine_create(song, sizeof(song)); REQUIRE(engine != NULL);
    app_sampler_t *s = sampler_from(song, sizeof(song)); REQUIRE(app_sampler_bind_engine(s, engine));
    app_sampler_sample_info_t before; REQUIRE(app_sampler_get_info(s, 0, &before));
    const uint64_t revision = app_sampler_revision(s);
    REQUIRE(app_sampler_edit_begin(s, 0) == APP_SAMPLER_OK);
    app_sampler_edit_t edit; REQUIRE(app_sampler_edit_get(s, &edit));
    edit.gain = 0.5f; edit.trim_start = 2; edit.trim_end = 6; edit.loop_start = 2; edit.loop_end = 6;
    REQUIRE(app_sampler_edit_set(s, &edit) == APP_SAMPLER_OK);
    REQUIRE(app_sampler_audition(s, 0) == APP_SAMPLER_OK);
    float output[64]; app_sampler_render(s, output, 32);
    for (unsigned i = 0; i < 64; ++i) { REQUIRE(near(output[i], 0.1875f)); }
    REQUIRE(app_sampler_revision(s) == revision);
    app_sampler_edit_cancel(s);
    app_sampler_sample_info_t info; REQUIRE(app_sampler_get_info(s, 0, &info));
    REQUIRE(info.trim_start == 0 && info.gain == before.gain);
    memset(output, 0x7f, sizeof(output)); app_sampler_render(s, output, 32);
    for (unsigned i = 0; i < 64; ++i) { REQUIRE(output[i] == 0); }
    REQUIRE(app_sampler_edit_begin(s, 0) == APP_SAMPLER_OK);
    REQUIRE(app_sampler_edit_set(s, &edit) == APP_SAMPLER_OK && app_sampler_edit_apply(s) == APP_SAMPLER_OK);
    REQUIRE(app_sampler_revision(s) == revision + 1U);
    REQUIRE(app_sampler_get_info(s, 0, &info)); REQUIRE(info.trim_start == 2 && info.gain == 0.5f && info.default_volume == 0.75f);
    /* Explicit S3M V32 overrides original instrument volume48, but edit gain
     * still multiplies it: sample.5 * volume.5 * gain.5 * leftpan.8 * .22. */
    tracker_engine_play(engine); tracker_engine_render_float(engine, output, 1);
    REQUIRE(near(output[0], 0.022f) && near(output[1], 0.0055f));
    REQUIRE(app_sampler_undo(s) == APP_SAMPLER_OK);
    REQUIRE(app_sampler_revision(s) == revision + 2U && app_sampler_undo(s) == APP_SAMPLER_BAD_STATE);
    tracker_engine_restart(engine); tracker_engine_render_float(engine, output, 1);
    REQUIRE(near(output[0], 0.044f));
    REQUIRE(app_sampler_edit_begin(s, 0) == APP_SAMPLER_OK);
    REQUIRE(app_sampler_edit_get(s, &edit)); edit.gain = NAN;
    REQUIRE(app_sampler_edit_set(s, &edit) == APP_SAMPLER_RANGE);
    REQUIRE(app_sampler_edit_get(s, &edit) && edit.gain == 1.0f);
    app_sampler_edit_cancel(s);
    app_sampler_destroy(s); tracker_engine_destroy(engine);
    puts("sampler: draft audition, Cancel / Apply / Undo, independent default volume and gain PASS");
}

static void assignment_and_transactions(void)
{
    uint8_t song[1024]; fixture_song(song);
    tracker_engine_t *engine = tracker_engine_create(song, sizeof(song)); REQUIRE(engine != NULL);
    tracker_engine_t *identity = engine;
    app_sampler_t *s = sampler_from(song, sizeof(song)); REQUIRE(app_sampler_bind_engine(s, engine));
    tracker_cell_t cell, after; tracker_engine_cell(engine, 0, 0, 0, &cell);
    uint8_t wav[64]; const int16_t pcm[] = {8192, 8192, 8192, 8192}; unsigned index;
    const size_t wav_size = fixture_wav(wav, 16, 1, 48000, pcm, 4);
    REQUIRE(app_sampler_import_wav(s, wav, wav_size, "quarter.wav", &index) == APP_SAMPLER_OK);
    REQUIRE(app_sampler_assign_slot(s, index, engine, 1) == APP_SAMPLER_OK);
    tracker_engine_cell(engine, 0, 0, 0, &after);
    REQUIRE(after.note == cell.note && after.instrument == cell.instrument && after.volume == cell.volume &&
            after.effect == cell.effect && after.param == cell.param);
    tracker_engine_play(engine); float output[4]; tracker_engine_render_float(engine, output, 1);
    REQUIRE(near(output[0], 0.022f));
    REQUIRE(app_sampler_assign_slot(s, index, engine, 2) == APP_SAMPLER_OK);
    tracker_engine_restart(engine); tracker_engine_render_float(engine, output, 1);
    REQUIRE(near(output[0], 0.044f)); /* Instrument1 restored to original +.5. */
    uint8_t *copy; size_t copy_size;
    REQUIRE(tracker_engine_copy_blob(engine, &copy, &copy_size));
    REQUIRE(copy_size == sizeof(song) && memcmp(copy, song, sizeof(song)) == 0); free(copy);
    uint8_t invalid[96] = {0};
    REQUIRE(tracker_engine_prepare(invalid, sizeof(invalid)) == NULL);
    REQUIRE(!tracker_engine_reload(engine, invalid, sizeof(invalid)) && engine == identity);
    tracker_engine_get_status(engine, &(tracker_status_t){0});
    tracker_engine_prepared_t *prepared = tracker_engine_prepare(song, sizeof(song)); REQUIRE(prepared != NULL);
    memset(song, 0, sizeof(song)); /* Prepared owns its input, not this array. */
    tracker_engine_commit(engine, prepared); REQUIRE(engine == identity && !tracker_engine_is_playing(engine));
    tracker_engine_cell(engine, 0, 0, 0, &after); REQUIRE(after.effect == 4 && after.volume == 32);
    app_sampler_destroy(s); tracker_engine_destroy(engine);
    puts("sampler: explicit assignment preserves pattern effects, moves restore old slot; reload rollback/stable handle PASS");
}

static void bank_roundtrip(const uint8_t *bytes, size_t size)
{
    app_sampler_t *s = sampler_from(bytes, size);
    app_sampler_bank_t *snapshot = app_sampler_bank_snapshot(s); REQUIRE(snapshot != NULL);
    uint8_t *encoded; size_t encoded_size;
    REQUIRE(app_sampler_bank_encode(snapshot, &encoded, &encoded_size) == APP_SAMPLER_OK);
    app_sampler_result_t result; app_sampler_bank_t *bank = app_sampler_bank_prepare(encoded, encoded_size, &result);
    REQUIRE(bank && result == APP_SAMPLER_OK && app_sampler_bank_count(bank) == 9);
    app_sampler_t *restored = app_sampler_create(); REQUIRE(restored != NULL); app_sampler_bank_commit(restored, bank);
    int16_t a[1024], b[1024];
    for (unsigned i = 0; i < 9; ++i) {
        app_sampler_sample_info_t ia, ib; REQUIRE(app_sampler_get_info(s, i, &ia) && app_sampler_get_info(restored, i, &ib));
        REQUIRE(!strcmp(ia.name, ib.name) && ia.frames == ib.frames && ia.c2_speed == ib.c2_speed && ia.default_volume == ib.default_volume);
        const unsigned count = app_sampler_waveform(s, i, a, 1024);
        REQUIRE(app_sampler_waveform(restored, i, b, 1024) == count && memcmp(a, b, count * sizeof(int16_t)) == 0);
    }
    uint8_t *bad = malloc(encoded_size); REQUIRE(bad != NULL);
    memcpy(bad, encoded, encoded_size); fixture_u32(bad + 8, 65);
    REQUIRE(!app_sampler_bank_prepare(bad, encoded_size, &result));
    memcpy(bad, encoded, encoded_size); fixture_u32(bad + 16 + 84, UINT32_MAX);
    REQUIRE(!app_sampler_bank_prepare(bad, encoded_size, &result));
    memcpy(bad, encoded, encoded_size); fixture_u32(bad + 16 + 96, 0x7fc00000U);
    REQUIRE(!app_sampler_bank_prepare(bad, encoded_size, &result));
    REQUIRE(!app_sampler_bank_prepare(encoded, encoded_size - 1U, &result));
    REQUIRE(app_sampler_count(restored) == 9);
    tracker_engine_prepared_t *song = tracker_engine_prepare(bytes, size); REQUIRE(song != NULL);
    REQUIRE(app_sampler_bank_apply_slots(snapshot, song) == APP_SAMPLER_OK);
    tracker_engine_prepared_destroy(song);
    app_sampler_destroy(s);
    uint8_t *again; size_t again_size;
    REQUIRE(app_sampler_bank_encode(snapshot, &again, &again_size) == APP_SAMPLER_OK);
    REQUIRE(again_size == encoded_size && memcmp(again, encoded, encoded_size) == 0);
    app_sampler_bank_destroy(snapshot); free(again); free(encoded); free(bad); app_sampler_destroy(restored);
    puts("sampler: independent PCM roundtrip, immutable snapshot lifetime and corrupt bounds rejection PASS");
}

static void bus_and_host_record(void)
{
    uint8_t song[1024]; fixture_song(song); tracker_engine_t *engine = tracker_engine_create(song, sizeof(song));
    REQUIRE(engine != NULL); app_sampler_t *s = sampler_from(song, sizeof(song));
    app_audio_bus_t *bus = app_audio_bus_create(engine, NULL); REQUIRE(bus != NULL && app_audio_bus_set_sampler(bus, s));
    REQUIRE(app_sampler_audition(s, 0) == APP_SAMPLER_OK);
    int16_t output[32]; app_audio_bus_render(bus, output, 16);
    REQUIRE(output[0] == 12287 && output[1] == 12287);
    REQUIRE(!app_audio_bus_set_sampler(bus, s));
    REQUIRE(app_audio_bus_set_source_gain(bus, APP_AUDIO_SOURCE_SAMPLER, 0.5f));
    app_audio_bus_render(bus, output, 16); REQUIRE(output[0] == 6143);
    REQUIRE(app_audio_bus_begin_edit(bus, 10));
    app_audio_bus_render(bus, output, 16); for (unsigned i = 0; i < 32; ++i) { REQUIRE(output[i] == 0); }
    app_audio_bus_end_edit(bus);
    const uint64_t revision = app_sampler_revision(s);
    REQUIRE(app_sampler_record_start(s) == APP_SAMPLER_UNSUPPORTED);
    app_sampler_record_status_t status; app_sampler_record_get_status(s, &status);
    REQUIRE(status.state == APP_SAMPLER_RECORD_UNSUPPORTED && status.frames == 0 && !status.monitor);
    REQUIRE(app_sampler_record_keep(s, "fake", NULL) == APP_SAMPLER_BAD_STATE);
    REQUIRE(app_sampler_record_audition(s) == APP_SAMPLER_BAD_STATE && app_sampler_revision(s) == revision);
    app_audio_bus_destroy(bus); REQUIRE(app_sampler_shutdown(&s, 1) && !s); tracker_engine_destroy(engine);
    puts("sampler: third bus gain/edit gate and truthful host recording unsupported PASS");
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 2); size_t size; uint8_t *module = read_file(argv[1], &size);
    aryx_metadata(module, size); wav_cases(); streamed_file(); loop_and_limits(); editing_and_render();
    assignment_and_transactions(); bank_roundtrip(module, size); bus_and_host_record();
    sampler_concurrency(module, size); free(module);
    printf("sampler native: PASS (%u assertions + concurrent checks)\n", checks); return 0;
}
