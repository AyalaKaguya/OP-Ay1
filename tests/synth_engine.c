// SPDX-License-Identifier: Apache-2.0
#include "app_synth.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <threads.h>
#endif

#define TEST_RATE 48000U
#define TEST_BLOCK 256U

static void render_silence(app_synth_t *synth, size_t frames)
{
    float buffer[TEST_BLOCK * 2U];
    while (frames != 0U) {
        size_t block = frames > TEST_BLOCK ? TEST_BLOCK : frames;
        app_synth_render(synth, buffer, block);
        frames -= block;
    }
}

static double energy(const float *buffer, size_t frames)
{
    double sum = 0.0;
    for (size_t i = 0; i < frames * 2U; ++i) {
        assert(isfinite(buffer[i]));
        assert(fabsf(buffer[i]) < 32.0f);
        sum += (double)buffer[i] * buffer[i];
    }
    return sum / (double)(frames * 2U);
}

static app_synth_t *sine_synth(void)
{
    app_synth_t *synth = app_synth_create(TEST_RATE);
    assert(synth != NULL);
    app_synth_patch_t patch = app_synth_patch_for_preset(APP_SYNTH_PATCH_INIT);
    patch.waveform = APP_SYNTH_WAVE_SINE;
    patch.attack_s = 0.0f;
    patch.decay_s = 0.0f;
    patch.sustain = 1.0f;
    patch.release_s = 0.05f;
    patch.filter_cutoff_hz = 20000.0f;
    patch.filter_resonance = 0.0f;
    patch.lfo_depth = 0.0f;
    assert(app_synth_set_patch(synth, &patch));
    render_silence(synth, TEST_RATE / 2U);
    return synth;
}

static void test_pitch_and_release(void)
{
    app_synth_t *synth = sine_synth();
    float *buffer = calloc(TEST_RATE * 2U, sizeof(*buffer));
    assert(buffer != NULL);
    assert(app_synth_note_on(synth, 69, 1.0f));
    app_synth_render(synth, buffer, TEST_RATE);
    unsigned crossings = 0;
    for (size_t i = 1; i < TEST_RATE; ++i) {
        if (buffer[(i - 1U) * 2U] <= 0.0f && buffer[i * 2U] > 0.0f) {
            ++crossings;
        }
    }
    assert(crossings >= 439U && crossings <= 441U);
    assert(energy(buffer, TEST_RATE) > 0.001);
    assert(app_synth_note_off(synth, 69));
    app_synth_render(synth, buffer, TEST_RATE / 10U);
    app_synth_status_t status;
    app_synth_get_status(synth, &status);
    assert(status.active_voices == 0U);
    assert(energy(buffer + TEST_RATE / 10U, TEST_RATE / 20U) < 0.0000001);
    printf("pitch: A4 %u positive crossings/second; release reached silence\n", crossings);
    free(buffer);
    app_synth_destroy(synth);
}

static void test_blocks(void)
{
    app_synth_t *whole = app_synth_create(TEST_RATE);
    app_synth_t *chunks = app_synth_create(TEST_RATE);
    assert(whole != NULL && chunks != NULL);
    const size_t frames = 8193U;
    float *a = calloc(frames * 2U, sizeof(*a));
    float *b = calloc(frames * 2U, sizeof(*b));
    assert(a != NULL && b != NULL);
    assert(app_synth_set_patch_id(whole, APP_SYNTH_PATCH_PAD));
    assert(app_synth_set_patch_id(chunks, APP_SYNTH_PATCH_PAD));
    assert(app_synth_note_on(whole, 60, 0.8f));
    assert(app_synth_note_on(chunks, 60, 0.8f));
    assert(app_synth_note_on(whole, 67, 0.65f));
    assert(app_synth_note_on(chunks, 67, 0.65f));
    app_synth_render(whole, a, frames);
    for (size_t offset = 0; offset < frames;) {
        size_t block = (offset % 317U) + 1U;
        if (block > frames - offset) {
            block = frames - offset;
        }
        app_synth_render(chunks, b + offset * 2U, block);
        offset += block;
    }
    assert(memcmp(a, b, frames * 2U * sizeof(*a)) == 0);
    printf("blocks: 8193-frame render is bit-identical across irregular blocks\n");
    free(a);
    free(b);
    app_synth_destroy(whole);
    app_synth_destroy(chunks);
}

static void render_irregular(app_synth_t *synth, float *output, size_t frames)
{
    for (size_t offset = 0; offset < frames;) {
        size_t block = offset % 137U + 1U;
        if (block > frames - offset) {
            block = frames - offset;
        }
        app_synth_render(synth, output + offset * 2U, block);
        offset += block;
    }
}

static void test_idle_patch_and_dynamic_pan_blocks(void)
{
    app_synth_t *whole = app_synth_create(TEST_RATE);
    app_synth_t *chunks = app_synth_create(TEST_RATE);
    const size_t frames = 10007U;
    float *a = calloc(frames * 2U, sizeof(*a));
    float *b = calloc(frames * 2U, sizeof(*b));
    assert(whole != NULL && chunks != NULL && a != NULL && b != NULL);
    for (unsigned stage = 0; stage < 10U; ++stage) {
        app_synth_patch_t patch = app_synth_patch_for_preset((app_synth_patch_id_t)(stage % 4U));
        patch.pan = (float)((int)stage % 5 - 2) * 0.5f;
        assert(app_synth_set_patch(whole, &patch));
        assert(app_synth_set_patch(chunks, &patch));
        if (stage == 2U || stage == 7U) {
            assert(app_synth_note_on(whole, 60 + (int)stage, 0.8f));
            assert(app_synth_note_on(chunks, 60 + (int)stage, 0.8f));
        }
        if (stage == 5U) {
            app_synth_panic(whole);
            app_synth_panic(chunks);
        }
        app_synth_render(whole, a, frames);
        render_irregular(chunks, b, frames);
        assert(memcmp(a, b, frames * 2U * sizeof(*a)) == 0);
        app_synth_status_t status;
        assert(app_synth_get_status(whole, &status));
        assert(status.rendered_frames == (uint64_t)(stage + 1U) * frames);
        if (stage < 2U || stage == 5U || stage == 6U) {
            assert(energy(a, frames) == 0.0 && status.active_voices == 0U);
        } else {
            assert(energy(a, frames) > 0.00000001);
        }
    }
    app_synth_destroy(whole);
    app_synth_destroy(chunks);
    free(a);
    free(b);
    puts("idle/pan: dynamic pan and later notes remain block-equivalent after idle edits");
}

static void test_signed_zero_output(void)
{
    app_synth_t *synth = sine_synth();
    float *buffer = calloc(4096U * 2U, sizeof(*buffer));
    assert(buffer != NULL);
    assert(app_synth_note_on(synth, 69, 1.0e-44f));
    app_synth_render(synth, buffer, 4096U);
    unsigned negative_zeros = 0U;
    unsigned positive_zeros = 0U;
    for (unsigned i = 0; i < 4096U * 2U; ++i) {
        assert(isfinite(buffer[i]));
        if (buffer[i] == 0.0f) {
            negative_zeros += signbit(buffer[i]) ? 1U : 0U;
            positive_zeros += signbit(buffer[i]) ? 0U : 1U;
        }
    }
    assert(negative_zeros != 0U && positive_zeros != 0U);
    app_synth_panic(synth);
    app_synth_render(synth, buffer, 4096U);
    for (unsigned i = 0; i < 4096U * 2U; ++i) {
        assert(buffer[i] == 0.0f && !signbit(buffer[i]));
    }
    free(buffer);
    app_synth_destroy(synth);
    puts("signed zero: underflow keeps both polarities and panic yields positive zero");
}

static void test_waveforms_and_extremes(void)
{
    app_synth_t *synth = sine_synth();
    float buffer[4096U * 2U];
    double energies[5];
    assert(app_synth_note_on(synth, 72, 0.9f));
    for (unsigned wave = APP_SYNTH_WAVE_SAW; wave <= APP_SYNTH_WAVE_WAVETABLE; ++wave) {
        app_synth_patch_t patch;
        assert(app_synth_get_patch(synth, &patch));
        patch.waveform = (app_synth_waveform_t)wave;
        patch.wavetable_position = 0.7f;
        assert(app_synth_set_patch(synth, &patch));
        render_silence(synth, TEST_RATE / 4U);
        app_synth_render(synth, buffer, 4096U);
        energies[wave] = energy(buffer, 4096U);
        assert(energies[wave] > 0.0001);
    }
    assert(fabs(energies[APP_SYNTH_WAVE_SQUARE] - energies[APP_SYNTH_WAVE_SINE]) > 0.001);
    for (unsigned pass = 0; pass < 20U; ++pass) {
        app_synth_patch_t patch = app_synth_patch_for_preset(APP_SYNTH_PATCH_INIT);
        patch.waveform = (app_synth_waveform_t)(pass % 5U);
        patch.wavetable_position = pass & 1U ? -INFINITY : INFINITY;
        patch.detune_cents = pass & 1U ? -1000.0f : 1000.0f;
        patch.gain = 4.0f;
        patch.filter_cutoff_hz = pass & 1U ? -1000.0f : 1000000.0f;
        patch.filter_resonance = 3.0f;
        patch.attack_s = NAN;
        patch.decay_s = -1.0f;
        patch.release_s = -1.0f;
        patch.sustain = 2.0f;
        patch.lfo_hz = 100.0f;
        patch.lfo_target = (app_synth_lfo_target_t)(pass % 3U);
        patch.lfo_depth = 100.0f;
        assert(app_synth_set_patch(synth, &patch));
        assert(app_synth_note_on(synth, (int)(pass * 6U), 1.0f));
        app_synth_render(synth, buffer, 4096U);
        (void)energy(buffer, 4096U);
    }
    app_synth_patch_t sanitized;
    assert(app_synth_get_patch(synth, &sanitized));
    assert(isfinite(sanitized.wavetable_position));
    assert(sanitized.gain == 1.5f);
    assert(sanitized.filter_resonance == 0.95f);
    assert(sanitized.attack_s == 0.0f);
    app_synth_panic(synth);
    app_synth_render(synth, buffer, 4096U);
    assert(energy(buffer, 4096U) == 0.0);
    printf("waveforms/extremes: all 5 waves and all 3 LFO targets finite and bounded\n");
    app_synth_destroy(synth);
}

static void test_queue_emergency_and_steal(void)
{
    app_synth_t *synth = sine_synth();
    const int notes[] = {60, 62, 64, 65, 67};
    for (unsigned i = 0; i < 5U; ++i) {
        assert(app_synth_note_on(synth, notes[i], 0.8f));
    }
    render_silence(synth, 1024U);
    app_synth_status_t status;
    app_synth_get_status(synth, &status);
    assert(status.active_voices == 4U && status.stolen_voices == 1U);
    assert(app_synth_note_off(synth, 60));
    render_silence(synth, TEST_RATE / 10U);
    app_synth_get_status(synth, &status);
    assert(status.active_voices == 4U);
    app_synth_all_notes_off(synth);
    render_silence(synth, TEST_RATE / 10U);
    app_synth_get_status(synth, &status);
    assert(status.active_voices == 0U);
    for (unsigned i = 0; i < APP_SYNTH_COMMAND_CAPACITY; ++i) {
        assert(app_synth_note_on(synth, 69, 0.8f));
    }
    assert(!app_synth_note_on(synth, 70, 0.8f));
    assert(app_synth_note_off(synth, 69));
    render_silence(synth, TEST_RATE / 10U);
    app_synth_get_status(synth, &status);
    assert(status.active_voices == 0U && status.emergency_releases > 0U);
    for (unsigned i = 0; i < APP_SYNTH_COMMAND_CAPACITY; ++i) {
        assert(app_synth_note_on(synth, (int)(40U + i % 24U), 1.0f));
    }
    app_synth_panic(synth);
    float buffer[TEST_BLOCK * 2U];
    app_synth_render(synth, buffer, TEST_BLOCK);
    assert(energy(buffer, TEST_BLOCK) == 0.0);
    app_synth_get_status(synth, &status);
    assert(status.active_voices == 0U && status.panic_count == 1U);
    app_synth_render(synth, buffer, TEST_BLOCK);
    assert(energy(buffer, TEST_BLOCK) == 0.0);
    assert(app_synth_note_on(synth, 72, 0.8f));
    app_synth_render(synth, buffer, TEST_BLOCK);
    assert(energy(buffer, TEST_BLOCK) > 0.0001);
    assert(app_synth_get_status(synth, &status) && status.active_voices == 1U);
    app_synth_panic(synth);
    app_synth_render(synth, buffer, TEST_BLOCK);
    assert(energy(buffer, TEST_BLOCK) == 0.0);
    assert(!app_synth_note_on(synth, -1, 1.0f));
    assert(!app_synth_note_on(synth, 128, 1.0f));
    assert(!app_synth_note_on(synth, 60, NAN));
    printf("queue/steal: oldest held voice stolen; queue-full release and panic cannot stick\n");
    app_synth_destroy(synth);
}

static void test_source_headroom(void)
{
    app_synth_t *synth = sine_synth();
    app_synth_patch_t patch = app_synth_patch_for_preset(APP_SYNTH_PATCH_INIT);
    patch.waveform = APP_SYNTH_WAVE_SQUARE;
    patch.filter_cutoff_hz = 450.0f;
    patch.filter_resonance = 0.95f;
    patch.gain = 1.5f;
    patch.attack_s = 0.0f;
    patch.decay_s = 0.0f;
    patch.sustain = 1.0f;
    assert(app_synth_set_patch(synth, &patch));
    render_silence(synth, TEST_RATE / 2U);
    for (int note = 69; note < 73; ++note) {
        assert(app_synth_note_on(synth, note, 1.0f));
    }
    float buffer[TEST_BLOCK * 2U];
    float peak = 0.0f;
    for (unsigned block = 0; block < 1000U; ++block) {
        app_synth_render(synth, buffer, TEST_BLOCK);
        (void)energy(buffer, TEST_BLOCK);
        for (unsigned i = 0; i < TEST_BLOCK * 2U; ++i) {
            peak = fmaxf(peak, fabsf(buffer[i]));
        }
    }
    assert(peak > 1.0f);
    printf("source headroom: peak %.3f preserved for final bus saturation\n", peak);
    app_synth_destroy(synth);
}

static void test_preset_performance_levels(void)
{
    const int roots[] = {48, 60, 72};
    const int intervals[] = {0, 4, 7, 11};
    float *buffer = calloc(TEST_BLOCK * 2U, sizeof(*buffer));
    assert(buffer != NULL);
    for (int preset = 0; preset < APP_SYNTH_PATCH_COUNT; ++preset) {
        for (unsigned root = 0; root < sizeof(roots) / sizeof(roots[0]); ++root) {
            app_synth_t *synth = app_synth_create(TEST_RATE);
            assert(synth != NULL);
            assert(app_synth_set_patch_id(synth, (app_synth_patch_id_t)preset));
            render_silence(synth, TEST_RATE / 2U);
            assert(app_synth_note_on(synth, roots[root], 0.8f));
            render_silence(synth, TEST_RATE * 2U);
            double sum = 0.0;
            size_t measured = 0;
            while (measured < TEST_RATE * 2U) {
                size_t frames = TEST_RATE * 2U - measured;
                if (frames > TEST_BLOCK) {
                    frames = TEST_BLOCK;
                }
                app_synth_render(synth, buffer, frames);
                sum += energy(buffer, frames) * (double)frames;
                measured += frames;
            }
            const double rms = sqrt(sum / (double)measured);
            /* A normal one-finger voice must be usable at codec unity. The
             * old 0.18 normalization fails this level floor for every preset. */
            assert(rms > 0.060 && rms < 0.2);
            app_synth_destroy(synth);

            synth = app_synth_create(TEST_RATE);
            assert(synth != NULL);
            assert(app_synth_set_patch_id(synth, (app_synth_patch_id_t)preset));
            render_silence(synth, TEST_RATE / 2U);
            for (unsigned key = 0; key < 4; ++key) {
                assert(app_synth_note_on(synth, roots[root] + intervals[key], 1.0f));
            }
            float peak = 0.0f;
            size_t produced = 0;
            while (produced < TEST_RATE * 10U) {
                size_t frames = TEST_RATE * 10U - produced;
                if (frames > TEST_BLOCK) {
                    frames = TEST_BLOCK;
                }
                app_synth_render(synth, buffer, frames);
                for (size_t sample = 0; sample < frames * 2U; ++sample) {
                    assert(isfinite(buffer[sample]));
                    peak = fmaxf(peak, fabsf(buffer[sample]));
                }
                produced += frames;
            }
            assert(peak < 1.0f);
            printf("preset level: %s C%d single RMS %.4f / full-velocity chord peak %.4f\n",
                   app_synth_preset_name((app_synth_patch_id_t)preset),
                   roots[root] / 12 - 1, rms, (double)peak);
            app_synth_destroy(synth);
        }
    }
    free(buffer);
}

static void test_envelope(void)
{
    app_synth_t *synth = sine_synth();
    app_synth_patch_t patch;
    assert(app_synth_get_patch(synth, &patch));
    patch.attack_s = 0.1f;
    patch.decay_s = 0.1f;
    patch.sustain = 0.25f;
    patch.release_s = 0.1f;
    assert(app_synth_set_patch(synth, &patch));
    render_silence(synth, TEST_RATE / 2U);
    assert(app_synth_note_on(synth, 69, 1.0f));
    float buffer[4800U * 2U];
    app_synth_render(synth, buffer, 4800U);
    double early = energy(buffer, 480U);
    double late = energy(buffer + 4320U * 2U, 480U);
    assert(late > early * 20.0);
    render_silence(synth, TEST_RATE / 2U);
    app_synth_render(synth, buffer, 4800U);
    double sustain = energy(buffer, 4800U);
    assert(sustain < late * 0.12 && sustain > late * 0.02);
    app_synth_all_notes_off(synth);
    render_silence(synth, TEST_RATE / 5U);
    app_synth_status_t status;
    app_synth_get_status(synth, &status);
    assert(status.active_voices == 0U);
    printf("envelope: attack rises, decay reaches sustain, release clears voice\n");
    app_synth_destroy(synth);
}

typedef struct {
    app_synth_t *synth;
    unsigned id;
    atomic_uint *done;
} producer_context_t;

#ifdef _WIN32
static DWORD WINAPI stress_producer(LPVOID argument)
#else
static int stress_producer(void *argument)
#endif
{
    producer_context_t *context = argument;
    for (unsigned event = 0; event < 4000U; ++event) {
        int note = (int)(40U + (event + context->id * 7U) % 48U);
        (void)app_synth_note_on(context->synth, note, 0.7f);
        if (event % 73U == 0U) {
            (void)app_synth_set_patch_id(context->synth,
                                         (app_synth_patch_id_t)(event % APP_SYNTH_PATCH_COUNT));
        }
        (void)app_synth_note_off(context->synth, note);
        if (event % 97U == 0U) {
            app_synth_panic(context->synth);
        }
        app_synth_patch_t snapshot;
        memset(&snapshot, 0xA5, sizeof(snapshot));
        app_synth_patch_t old_patch = snapshot;
        if (app_synth_get_patch(context->synth, &snapshot)) {
            assert(isfinite(snapshot.gain) && snapshot.gain >= 0.0f && snapshot.gain <= 1.5f);
        } else {
            assert(memcmp(&snapshot, &old_patch, sizeof(snapshot)) == 0);
        }
        app_synth_status_t status;
        memset(&status, 0xA5, sizeof(status));
        app_synth_status_t old_status = status;
        if (app_synth_get_status(context->synth, &status)) {
            assert(status.active_voices <= APP_SYNTH_VOICE_COUNT);
            assert(status.queue_depth <= APP_SYNTH_COMMAND_CAPACITY);
            assert(isfinite(status.peak));
        } else {
            assert(memcmp(&status, &old_status, sizeof(status)) == 0);
        }
    }
    atomic_fetch_add_explicit(context->done, 1U, memory_order_release);
    return 0;
}

static void test_concurrent_commands(void)
{
    app_synth_t *synth = app_synth_create(TEST_RATE);
    assert(synth != NULL);
    atomic_uint done;
    atomic_init(&done, 0U);
    producer_context_t contexts[3];
#ifdef _WIN32
    HANDLE threads[3];
#else
    thrd_t threads[3];
#endif
    for (unsigned i = 0; i < 3U; ++i) {
        contexts[i].synth = synth;
        contexts[i].id = i;
        contexts[i].done = &done;
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, stress_producer, &contexts[i], 0, NULL);
        assert(threads[i] != NULL);
#else
        assert(thrd_create(&threads[i], stress_producer, &contexts[i]) == thrd_success);
#endif
    }
    float buffer[TEST_BLOCK * 2U];
    unsigned blocks = 0U;
    while (atomic_load_explicit(&done, memory_order_acquire) != 3U) {
        app_synth_render(synth, buffer, TEST_BLOCK);
        (void)energy(buffer, TEST_BLOCK);
        assert(++blocks < 100000U);
    }
    for (unsigned i = 0; i < 3U; ++i) {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i], 10000U) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[i]) != 0);
#else
        int result;
        assert(thrd_join(threads[i], &result) == thrd_success && result == 0);
#endif
    }
    app_synth_panic(synth);
    app_synth_render(synth, buffer, TEST_BLOCK);
    assert(energy(buffer, TEST_BLOCK) == 0.0);
    app_synth_status_t status;
    app_synth_get_status(synth, &status);
    assert(status.active_voices == 0U && status.queue_depth == 0U);
    printf("concurrency: 3 producers x 4000 events; %u render blocks, %u bounded ring drops\n",
           blocks, status.dropped_commands);
    app_synth_destroy(synth);
}

static void test_failed_snapshots_preserve_output(void)
{
    app_synth_t *synth = app_synth_create(TEST_RATE);
    assert(synth != NULL);
    app_synth_patch_t patch;
    app_synth_status_t status;
    memset(&patch, 0xA5, sizeof(patch));
    memset(&status, 0xA5, sizeof(status));
    app_synth_patch_t old_patch = patch;
    app_synth_status_t old_status = status;
    assert(!app_synth_get_patch(NULL, &patch));
    assert(!app_synth_get_status(NULL, &status));
    assert(!app_synth_get_patch(synth, NULL));
    assert(!app_synth_get_status(synth, NULL));
    assert(memcmp(&patch, &old_patch, sizeof(patch)) == 0);
    assert(memcmp(&status, &old_status, sizeof(status)) == 0);
    assert(app_synth_get_patch(synth, &patch));
    assert(app_synth_get_status(synth, &status));
    assert(patch.waveform == APP_SYNTH_WAVE_SAW && status.rendered_frames == 0U);
    app_synth_destroy(synth);
    puts("snapshots: failed reads preserve caller output; successful reads publish complete data");
}

static void write_u16(FILE *file, unsigned value)
{
    fputc((int)(value & 255U), file);
    fputc((int)((value >> 8U) & 255U), file);
}

static void write_u32(FILE *file, unsigned value)
{
    write_u16(file, value & 65535U);
    write_u16(file, value >> 16U);
}

static void write_demo(const char *path)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    const unsigned frames = TEST_RATE * 8U;
    fwrite("RIFF", 1, 4, file);
    write_u32(file, 36U + frames * 4U);
    fwrite("WAVEfmt ", 1, 8, file);
    write_u32(file, 16U);
    write_u16(file, 1U);
    write_u16(file, 2U);
    write_u32(file, TEST_RATE);
    write_u32(file, TEST_RATE * 4U);
    write_u16(file, 4U);
    write_u16(file, 16U);
    fwrite("data", 1, 4, file);
    write_u32(file, frames * 4U);
    app_synth_t *synth = app_synth_create(TEST_RATE);
    assert(synth != NULL);
    float buffer[TEST_BLOCK * 2U];
    for (unsigned preset = 0; preset < APP_SYNTH_PATCH_COUNT; ++preset) {
        app_synth_panic(synth);
        app_synth_render(synth, buffer, 0U);
        assert(app_synth_set_patch_id(synth, (app_synth_patch_id_t)preset));
        assert(app_synth_note_on(synth, preset == APP_SYNTH_PATCH_BASS ? 40 : 60, 0.9f));
        if (preset == APP_SYNTH_PATCH_PAD) {
            assert(app_synth_note_on(synth, 64, 0.65f));
            assert(app_synth_note_on(synth, 67, 0.65f));
        }
        for (unsigned offset = 0; offset < TEST_RATE * 2U;) {
            if (offset == TEST_RATE) {
                app_synth_all_notes_off(synth);
            }
            unsigned block = TEST_BLOCK;
            unsigned boundary = offset < TEST_RATE ? TEST_RATE : TEST_RATE * 2U;
            if (block > boundary - offset) {
                block = boundary - offset;
            }
            app_synth_render(synth, buffer, block);
            for (unsigned i = 0; i < block * 2U; ++i) {
                float limited = fmaxf(-1.0f, fminf(1.0f, buffer[i]));
                int sample = (int)lrintf(limited * 32767.0f);
                write_u16(file, (unsigned)(uint16_t)(int16_t)sample);
            }
            offset += block;
        }
    }
    assert(fclose(file) == 0);
    app_synth_destroy(synth);
    printf("wav: %s (8 seconds, INIT/BASS/LEAD/PAD)\n", path);
}

int main(int argc, char **argv)
{
    assert(app_synth_create(0.0f) == NULL);
    assert(app_synth_create(NAN) == NULL);
    test_pitch_and_release();
    test_blocks();
    test_idle_patch_and_dynamic_pan_blocks();
    test_signed_zero_output();
    test_waveforms_and_extremes();
    test_queue_emergency_and_steal();
    test_envelope();
    test_source_headroom();
    test_preset_performance_levels();
    test_concurrent_commands();
    test_failed_snapshots_preserve_output();
    if (argc > 1) {
        write_demo(argv[1]);
    }
    puts("synth host tests passed");
    return 0;
}
