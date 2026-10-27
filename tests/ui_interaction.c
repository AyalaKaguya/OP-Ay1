// SPDX-License-Identifier: Apache-2.0
/* Portable UI behavior checks with the real tracker, synth and output bus.
 * Product controls are exercised through pointer down/move/up. Tests only
 * initialize instances, advance their audio/time, select pages through the
 * public page API, and read state. They do not edit model state directly. */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_model.h"
#include "tracker_render.h"

#define SAMPLE_RATE 48000U
#define BLOCK_FRAMES 512
#define BOUNDARY_LIMIT_FRAMES (SAMPLE_RATE * 20U)

typedef struct {
    tracker_engine_t *tracker;
    app_synth_t *synth;
    app_audio_bus_t *bus;
    tracker_model_t model;
    int16_t *pcm;
    int16_t *wave_scratch;
    uint16_t *frame;
    uint64_t now_ms;
    unsigned audio_ms_remainder;
} fixture_t;

typedef void (*test_fn_t)(fixture_t *fixture);

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "ui_interaction: FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static bool near(float actual, float expected, float tolerance)
{
    return isfinite(actual) && fabsf(actual - expected) <= tolerance;
}

static uint8_t *read_module(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    require(file != NULL, "cannot open aryx.s3m fixture");
    require(fseek(file, 0, SEEK_END) == 0, "cannot seek fixture");
    const long length = ftell(file);
    require(length > 0 && length <= 16 * 1024 * 1024, "invalid fixture length");
    require(fseek(file, 0, SEEK_SET) == 0, "cannot rewind fixture");
    *size = (size_t)length;
    uint8_t *data = malloc(*size);
    require(data != NULL, "fixture allocation failed");
    require(fread(data, 1, *size, file) == *size, "incomplete fixture read");
    require(fclose(file) == 0, "fixture close failed");
    return data;
}

static fixture_t *fixture_create(const uint8_t *module, size_t size)
{
    fixture_t *f = calloc(1, sizeof(*f));
    require(f != NULL, "fixture context allocation failed");
    f->tracker = tracker_engine_create(module, size);
    f->synth = app_synth_create((float)SAMPLE_RATE);
    f->bus = app_audio_bus_create(f->tracker, f->synth);
    f->pcm = calloc(BLOCK_FRAMES * 2U, sizeof(*f->pcm));
    f->wave_scratch = malloc(TRACKER_WAVEFORM_SAMPLES * sizeof(*f->wave_scratch));
    f->frame = malloc(480U * 480U * sizeof(*f->frame));
    require(f->tracker && f->synth && f->bus && f->pcm && f->wave_scratch && f->frame,
            "audio/model fixture creation failed");
    tracker_model_init(&f->model, f->tracker, f->synth, f->bus);
    f->now_ms = 1;
    tracker_model_tick(&f->model, f->now_ms);
    return f;
}

static void fixture_destroy(fixture_t *f)
{
    app_audio_bus_destroy(f->bus);
    app_synth_destroy(f->synth);
    tracker_engine_destroy(f->tracker);
    free(f->pcm);
    free(f->wave_scratch);
    free(f->frame);
    free(f);
}

static void draw(fixture_t *f)
{
    if (f->model.view.page == TRACKER_PAGE_SYNTH) {
        tracker_render_synth(f->frame, &f->model.synth_view);
    } else if (f->model.view.page == TRACKER_PAGE_PATTERN) {
        tracker_render_pattern(f->frame, f->tracker);
    } else if (f->model.view.page == TRACKER_PAGE_WAVES) {
        tracker_render_waves(f->frame, f->tracker, f->wave_scratch, TRACKER_WAVEFORM_SAMPLES);
    } else {
        tracker_render_controls(f->frame, &f->model.view);
    }
}

static void pointer(fixture_t *f, int x, int y, bool pressed)
{
    f->now_ms += 16;
    tracker_model_pointer(&f->model, x, y, pressed, f->now_ms);
    tracker_model_tick(&f->model, f->now_ms);
}

static void tap(fixture_t *f, int x, int y)
{
    pointer(f, x, y, true);
    pointer(f, x, y, false);
}

static void drag(fixture_t *f, int x1, int y1, int x2, int y2)
{
    pointer(f, x1, y1, true);
    for (int step = 1; step <= 8; ++step) {
        pointer(f, x1 + (x2 - x1) * step / 8, y1 + (y2 - y1) * step / 8, true);
    }
    pointer(f, x2, y2, false);
}

static void dot(fixture_t *f, int page)
{
    tap(f, 200 + page * 16, 460);
    require(f->model.view.page == page, "navigation dot center selected the wrong page");
}

static uint64_t advance(fixture_t *f, unsigned frames)
{
    uint64_t magnitude = 0;
    for (unsigned produced = 0; produced < frames;) {
        const unsigned remaining = frames - produced;
        const int block = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        app_audio_bus_render(f->bus, f->pcm, block);
        for (int sample = 0; sample < block * 2; ++sample) {
            const int value = f->pcm[sample];
            magnitude += (unsigned)(value < 0 ? -value : value);
        }
        f->audio_ms_remainder += (unsigned)block * 1000U;
        f->now_ms += f->audio_ms_remainder / SAMPLE_RATE;
        f->audio_ms_remainder %= SAMPLE_RATE;
        tracker_model_tick(&f->model, f->now_ms);
        produced += (unsigned)block;
    }
    return magnitude;
}

static app_synth_status_t synth_status(fixture_t *f)
{
    app_synth_status_t status;
    memset(&status, 0, sizeof(status));
    require(app_synth_get_status(f->synth, &status), "synth status snapshot unavailable");
    return status;
}

static app_synth_patch_t synth_patch(fixture_t *f)
{
    app_synth_patch_t patch;
    memset(&patch, 0, sizeof(patch));
    require(app_synth_get_patch(f->synth, &patch), "synth patch snapshot unavailable");
    return patch;
}

static void silence_after_release(fixture_t *f)
{
    (void)advance(f, SAMPLE_RATE * 2U);
    require(synth_status(f).active_voices == 0, "released synth voice did not become idle");
    require(advance(f, BLOCK_FRAMES) == 0, "released synth continued producing PCM");
}

static void navigation(fixture_t *f)
{
    for (int page = 0; page < TRACKER_PAGE_COUNT; ++page) {
        dot(f, page);
        draw(f);
    }
    tap(f, 100, 460);
    require(f->model.view.page == TRACKER_PAGE_SYNTH, "outside-dot footer tap changed pages");
    dot(f, TRACKER_PAGE_WAVES);
    drag(f, 400, 400, 80, 400);
    require(f->model.view.page == TRACKER_PAGE_PATTERN, "horizontal navigation did not advance a page");
    drag(f, 80, 400, 400, 400);
    require(f->model.view.page == TRACKER_PAGE_WAVES, "horizontal navigation did not return a page");
}

static void playback(fixture_t *f)
{
    dot(f, TRACKER_PAGE_PLAYBACK);
    require(!tracker_engine_is_playing(f->tracker), "new song should start paused");
    dot(f, TRACKER_PAGE_PLAYBACK);
    tap(f, 350, 256);
    require(tracker_engine_is_playing(f->tracker), "PLAY did not start the song");
    require(advance(f, 8192) > 0, "playing the fixture produced no PCM");
    tap(f, 350, 256);
    require(!tracker_engine_is_playing(f->tracker), "PAUSE did not stop song progression");
    require(advance(f, BLOCK_FRAMES) == 0, "paused tracker produced PCM");
    for (int slot = 0; slot < 4; ++slot) {
        tap(f, 69 + slot * 114, 362);
        const float expected = (float)(slot + 1) * 0.5f;
        require(near(tracker_engine_speed_multiplier(f->tracker), expected, 0.0001f),
                "speed preset did not reach the tracker");
        require(near(f->model.view.speed_multiplier, expected, 0.0001f),
                "speed readout did not track its preset");
    }
    tap(f, 350, 256);
    (void)advance(f, 8192);
    tap(f, 120, 256);
    tracker_status_t status;
    tracker_engine_get_status(f->tracker, &status);
    require(!status.playing && status.order_index == 0 && status.row == 0,
            "RESTART must pause and rewind to the first song row");
    require(tracker_engine_launched_pattern(f->tracker) == -1, "RESTART retained pad mode");
    draw(f);
}

static void settle_scroll(fixture_t *f)
{
    for (int tick = 0; tick < 64; ++tick) {
        f->now_ms += 30;
        tracker_model_tick(&f->model, f->now_ms);
    }
}

static void mixer(fixture_t *f)
{
    dot(f, TRACKER_PAGE_MIXER);
    /* A manually muted solo must survive adding a second solo. Removing the
     * final solo later clears the temporary cascade and restores all channels. */
    tap(f, 414, 196);
    tap(f, 356, 196);
    require(f->model.view.soloed[0] && f->model.view.muted[0],
            "S0 then M0 did not leave channel zero muted");
    tap(f, 414, 262);
    require(f->model.view.soloed[0] && f->model.view.soloed[1] &&
            f->model.view.muted[0] && !f->model.view.muted[1],
            "adding S1 cleared the manual M0 mute");
    tap(f, 414, 196);
    require(!f->model.view.soloed[0] && f->model.view.soloed[1] &&
            f->model.view.muted[0], "removing S0 did not preserve S1 and M0");
    tap(f, 414, 262);
    for (int channel = 0; channel < f->model.view.channel_count; ++channel) {
        require(!f->model.view.soloed[channel] && !f->model.view.muted[channel],
                "removing the final solo did not restore all channels");
    }
    unsigned seen_volumes = 0;
    int previous = -1;
    for (int x = 100; x <= 389; ++x) {
        tap(f, x, 86);
        const int volume = f->model.view.volume;
        require(volume >= 0 && volume <= 100 && volume % 5 == 0,
                "master volume escaped 0..100 or five-percent steps");
        require(volume >= previous, "master volume mapping is not monotonic");
        seen_volumes |= 1U << (unsigned)(volume / 5);
        previous = volume;
    }
    require(seen_volumes == ((1U << 21) - 1U), "master control cannot reach every five-percent step");
    bool seen_gains[121] = {false};
    previous = -1;
    for (int x = 86; x <= 317; ++x) {
        tap(f, x, 205);
        const int step = f->model.view.gain_steps[0];
        require(step >= 0 && step <= 120 && step >= previous,
                "channel gain mapping escaped its range or is not monotonic");
        seen_gains[step] = true;
        previous = step;
    }
    for (int step = 0; step <= 120; ++step) {
        require(seen_gains[step], "gain control cannot reach every step from 0 through 120");
    }
    drag(f, 86, 205, 317, 205);
    require(f->model.view.page == TRACKER_PAGE_MIXER && f->model.view.gain_steps[0] == 120,
            "horizontal fader drag failed to reach +6dB or changed pages");
    require(near(tracker_engine_channel_gain(f->tracker, 0), powf(10.0f, 6.0f / 20.0f), 0.001f),
            "fader maximum did not reach the audio engine");
    drag(f, 317, 205, 86, 205);
    require(f->model.view.gain_steps[0] == 0 && tracker_engine_channel_gain(f->tracker, 0) == 0.0f,
            "fader minimum did not mute its channel gain");
    tap(f, 279, 205);
    require(f->model.view.gain_steps[0] == 100 &&
            near(tracker_engine_channel_gain(f->tracker, 0), 1.0f, 0.0001f),
            "unity fader point did not restore 0dB");

    tap(f, 356, 196);
    require(f->model.view.muted[0] && tracker_engine_channel_mute(f->tracker, 0),
            "M did not mute channel zero");
    tap(f, 414, 262);
    require(f->model.view.soloed[1] && !f->model.view.muted[1], "S did not solo channel one");
    for (int channel = 0; channel < f->model.view.channel_count; ++channel) {
        require(f->model.view.muted[channel] == (channel != 1), "solo did not cascade channel mutes");
    }
    const bool saved_mute = f->model.user_mute[0];
    tap(f, 356, 196);
    require(f->model.user_mute[0] == saved_mute && f->model.view.muted[0],
            "M changed a non-solo channel during solo");
    tap(f, 356, 262);
    require(f->model.view.soloed[1] && f->model.view.muted[1] &&
            tracker_engine_channel_solo(f->tracker, 1), "M on a solo channel cleared its solo");
    tap(f, 356, 262);
    tap(f, 414, 328);
    require(f->model.view.soloed[1] && f->model.view.soloed[2], "multiple solos were not retained");
    tap(f, 414, 262);
    require(!f->model.view.soloed[1] && f->model.view.muted[1] &&
            f->model.view.soloed[2], "removing one solo did not preserve the other");
    tap(f, 414, 328);
    for (int channel = 0; channel < f->model.view.channel_count; ++channel) {
        require(!f->model.view.soloed[channel] && !f->model.view.muted[channel] &&
                !tracker_engine_channel_mute(f->tracker, channel) &&
                !tracker_engine_channel_solo(f->tracker, channel),
                "removing the last solo did not restore all channels");
    }

    drag(f, 40, 400, 40, 180);
    require(f->model.view.mix_scroll > 0 && f->model.view.page == TRACKER_PAGE_MIXER,
            "vertical list drag did not scroll or changed pages");
    for (int swipe = 0; swipe < 5; ++swipe) {
        drag(f, 460, 400, 460, 180);
        settle_scroll(f);
        require(f->model.view.page == TRACKER_PAGE_MIXER, "vertical gutter drag changed pages");
    }
    require(f->model.view.mix_scroll == 792, "gutter drag did not reach the last four channels");
    require(f->model.view.channel_count == 12, "empty-channel check expects the twelve-channel fixture");
    for (int row = 0; row < 4; ++row) {
        const int channel = 12 + row;
        const uint8_t empty_gain = f->model.view.gain_steps[channel];
        drag(f, 100, 205 + row * 66, 317, 205 + row * 66);
        tap(f, 356, 196 + row * 66);
        tap(f, 414, 196 + row * 66);
        require(f->model.view.gain_steps[channel] == empty_gain && !f->model.view.muted[channel] &&
                !f->model.view.soloed[channel] && f->model.view.page == TRACKER_PAGE_MIXER,
                "empty channel responded to gain/mute/solo or changed pages");
    }
    draw(f);
}

static void pad_tap(fixture_t *f, int pad)
{
    tap(f, 69 + pad % 4 * 114, 148 + pad / 4 * 75);
}

static void pads(fixture_t *f)
{
    dot(f, TRACKER_PAGE_PADS);
    /* Pad blink follows the transport frame clock and effective BPM, rather
     * than wall-clock values passed to tracker_model_tick(). */
    dot(f, TRACKER_PAGE_PLAYBACK);
    tap(f, 409, 362);
    dot(f, TRACKER_PAGE_PADS);
    pad_tap(f, 0);
    tracker_status_t blink_status;
    tracker_engine_get_status(f->tracker, &blink_status);
    const bool blink_before = f->model.view.pad_blink;
    const uint64_t frames_before = blink_status.frames;
    f->now_ms += 900000;
    tracker_model_tick(&f->model, f->now_ms);
    tracker_engine_get_status(f->tracker, &blink_status);
    require(blink_status.frames == frames_before && f->model.view.pad_blink == blink_before,
            "pad blink changed when only wall-clock time changed");
    const float effective_bpm = blink_status.tempo * tracker_engine_speed_multiplier(f->tracker);
    require(effective_bpm >= 1.0f, "fixture has no effective BPM for pad clock test");
    const uint64_t half_beat = (uint64_t)(APP_AUDIO_SAMPLE_RATE * 30.0 / effective_bpm);
    require(half_beat > 0, "effective BPM produced an invalid half-beat");
    const unsigned to_boundary = (unsigned)(half_beat - blink_status.frames % half_beat);
    (void)advance(f, to_boundary);
    require(f->model.view.pad_blink != blink_before,
            "pad blink did not change at an effective-BPM frame boundary");

    /* Return to a clean paused pad scenario through public controls. */
    dot(f, TRACKER_PAGE_PLAYBACK);
    tap(f, 350, 256);
    tap(f, 350, 256);
    tap(f, 350, 256);
    tracker_model_set_page(&f->model, TRACKER_PAGE_PATTERN);
    tracker_model_set_page(&f->model, TRACKER_PAGE_PADS);
    pad_tap(f, 12);
    require(tracker_engine_launched_pattern(f->tracker) == -1 &&
            !tracker_engine_is_playing(f->tracker), "empty pad started a pattern");
    tap(f, 125, 148);
    require(tracker_engine_launched_pattern(f->tracker) == -1, "gap between pads triggered a launch");
    pad_tap(f, 0);
    require(tracker_engine_launched_pattern(f->tracker) == 0 &&
            tracker_engine_is_playing(f->tracker), "first pad tap did not immediately start its pattern");
    (void)advance(f, BLOCK_FRAMES);
    pad_tap(f, 0);
    require(tracker_engine_pattern_stop_pending(f->tracker), "playing pad did not queue a boundary stop");
    pad_tap(f, 0);
    require(!tracker_engine_pattern_stop_pending(f->tracker), "second pad tap did not cancel its queued stop");
    pad_tap(f, 1);
    require(tracker_engine_launched_pattern(f->tracker) == 0 &&
            tracker_engine_pending_pattern(f->tracker) == 1, "another pad did not queue a pattern switch");
    unsigned rendered = 0;
    while (tracker_engine_launched_pattern(f->tracker) != 1 && rendered < BOUNDARY_LIMIT_FRAMES) {
        (void)advance(f, BLOCK_FRAMES);
        rendered += BLOCK_FRAMES;
    }
    require(tracker_engine_launched_pattern(f->tracker) == 1 &&
            tracker_engine_pending_pattern(f->tracker) == -1 &&
            tracker_engine_is_playing(f->tracker), "queued pad did not switch at an audio pattern boundary");
    pad_tap(f, 1);
    require(tracker_engine_pattern_stop_pending(f->tracker), "running pad did not queue its stop");
    rendered = 0;
    while (tracker_engine_is_playing(f->tracker) && rendered < BOUNDARY_LIMIT_FRAMES) {
        (void)advance(f, BLOCK_FRAMES);
        rendered += BLOCK_FRAMES;
    }
    require(!tracker_engine_is_playing(f->tracker) &&
            tracker_engine_launched_pattern(f->tracker) == -1 &&
            !tracker_engine_pattern_stop_pending(f->tracker), "queued stop did not close at the audio boundary");
    pad_tap(f, 1);
    require(tracker_engine_is_playing(f->tracker), "pad did not restart after boundary stop");
    dot(f, TRACKER_PAGE_PLAYBACK);
    tap(f, 350, 256);
    require(!tracker_engine_is_playing(f->tracker) && tracker_engine_launched_pattern(f->tracker) == 1,
            "pause must preserve the running pad mode");
    dot(f, TRACKER_PAGE_PADS);
    pad_tap(f, 1);
    require(tracker_engine_is_playing(f->tracker) && tracker_engine_launched_pattern(f->tracker) == 1,
            "same pad did not resume while paused");
    dot(f, TRACKER_PAGE_PLAYBACK);
    tap(f, 350, 256);
    tap(f, 350, 256);
    require(tracker_engine_is_playing(f->tracker) && tracker_engine_launched_pattern(f->tracker) == -1,
            "Playback PLAY did not leave pad mode for song playback");
    draw(f);
}

static void synth_keys(fixture_t *f)
{
    dot(f, TRACKER_PAGE_SYNTH);
    require(!tracker_engine_is_playing(f->tracker), "synth test requires a paused tracker");
    pointer(f, 50, 420, true);
    require(f->model.synth_view.note == 60, "C4 press did not send MIDI note 60");
    require(advance(f, 4096) > 0 && synth_status(f).active_voices > 0,
            "held synth key did not produce PCM while the song was paused");
    require(f->model.view.master_level > 0, "final master meter did not include the synth");
    pointer(f, 110, 420, true);
    require(f->model.synth_view.note == 62 && f->model.view.page == TRACKER_PAGE_SYNTH,
            "white-key glissando did not change note or changed pages");
    require(advance(f, 4096) > 0, "glissando produced no PCM");
    pointer(f, 110, 420, false);
    require(f->model.synth_view.note == -1, "key release left a held-note indicator");
    silence_after_release(f);

    pointer(f, 82, 350, true);
    require(f->model.synth_view.note == 61, "black C-sharp key did not take input priority");
    (void)advance(f, 2048);
    pointer(f, 20, 350, true);
    require(f->model.synth_view.note == -1, "leaving the keyboard did not release the note");
    pointer(f, 20, 350, false);
    silence_after_release(f);

    pointer(f, 50, 420, true);
    (void)advance(f, 2048);
    tracker_model_set_page(&f->model, TRACKER_PAGE_PATTERN);
    require(f->model.synth_view.note == -1 && !f->model.pointer_down,
            "page change did not release keyboard ownership");
    silence_after_release(f);
    dot(f, TRACKER_PAGE_SYNTH);

    pointer(f, 50, 420, true);
    (void)advance(f, 4096);
    pointer(f, 20, 320, false);
    tap(f, 399, 295);
    require(advance(f, BLOCK_FRAMES) == 0 && synth_status(f).active_voices == 0 &&
            synth_status(f).panic_count > 0 && f->model.synth_view.note == -1,
            "PANIC did not immediately clear PCM, voices and held-note feedback");
    pointer(f, 110, 420, true);
    require(advance(f, 4096) > 0 && synth_status(f).active_voices > 0,
            "a new note could not sound after PANIC");
    pointer(f, 110, 420, false);
    silence_after_release(f);

    pointer(f, 50, 420, true);
    (void)advance(f, 2048);
    f->now_ms += 30001;
    tracker_model_tick(&f->model, f->now_ms);
    require(!f->model.pointer_down && f->model.synth_view.note == -1,
            "stale pointer stream did not release keyboard ownership");
    silence_after_release(f);
    draw(f);
}

static void synth_tab(fixture_t *f, int tab)
{
    tap(f, 79 + tab * 107, 126);
    require(f->model.synth_view.tab == tab, "synth tab did not select its parameter group");
    draw(f);
}

static void parameter_drag(fixture_t *f, int x1, int y, int x2)
{
    drag(f, x1, y, x2, y);
    (void)advance(f, BLOCK_FRAMES);
    require(!f->model.synth_view.command_failed, "parameter edit was rejected by the synth");
}

static void synth_patch_queue_retry(fixture_t *f)
{
    synth_tab(f, 0);
    for (unsigned index = 0; index < APP_SYNTH_COMMAND_CAPACITY; ++index) {
        require(app_synth_note_on(f->synth, 36 + (int)(index % 48), 0.5f),
                "could not fill the synth command queue");
    }
    app_synth_status_t full = synth_status(f);
    require(full.queue_depth >= APP_SYNTH_COMMAND_CAPACITY,
            "synth command queue did not report full before patch retry");

    /* No audio is rendered between this pointer sequence and the assertion:
     * the model must expose the rejected patch as pending/BUSY. */
    pointer(f, 429, 191, true);
    pointer(f, 429, 191, false);
    require(f->model.synth_view.command_failed,
            "a full synth command queue did not report a rejected patch");
    require(synth_status(f).queue_depth > 0,
            "patch rejection unexpectedly drained the command queue");

    (void)advance(f, BLOCK_FRAMES);
    require(!f->model.synth_view.command_failed,
            "model tick did not retry the pending patch after audio drained commands");
    const app_synth_patch_t accepted = synth_patch(f);
    require(near(accepted.detune_cents, f->model.synth_view.patch.detune_cents, 0.001f) &&
            near(accepted.gain, f->model.synth_view.patch.gain, 0.0001f),
            "DSP patch snapshot diverged from the UI patch after queue retry");
}

static void synth_controls(fixture_t *f)
{
    dot(f, TRACKER_PAGE_SYNTH);
    for (int tab = 0; tab < 4; ++tab) {
        synth_tab(f, tab);
    }
    synth_tab(f, 0);
    for (int wave = 1; wave <= 5; ++wave) {
        tap(f, 100, 180);
        (void)advance(f, BLOCK_FRAMES);
        const app_synth_waveform_t expected = (app_synth_waveform_t)(wave % 5);
        require(f->model.synth_view.patch.waveform == expected &&
                synth_patch(f).waveform == expected, "waveform selector did not reach every engine waveform");
    }
    for (int preset = 1; preset <= APP_SYNTH_PATCH_COUNT; ++preset) {
        tap(f, 425, 74);
        (void)advance(f, BLOCK_FRAMES);
        require(f->model.synth_view.preset == preset % APP_SYNTH_PATCH_COUNT &&
                synth_status(f).patch_id == (app_synth_patch_id_t)(preset % APP_SYNTH_PATCH_COUNT),
                "preset next selector did not reach the DSP");
    }
    tap(f, 52, 74);
    (void)advance(f, BLOCK_FRAMES);
    require(f->model.synth_view.preset == APP_SYNTH_PATCH_PAD &&
            synth_status(f).patch_id == APP_SYNTH_PATCH_PAD, "preset previous selector did not wrap");
    tap(f, 425, 74);
    (void)advance(f, BLOCK_FRAMES);

    parameter_drag(f, 234, 191, 429);
    require(near(synth_patch(f).detune_cents, 100.0f, 0.001f), "OSC detune upper endpoint did not update");
    parameter_drag(f, 429, 191, 234);
    require(near(synth_patch(f).detune_cents, -100.0f, 0.001f), "OSC detune lower endpoint did not update");
    parameter_drag(f, 234, 239, 429);
    require(synth_patch(f).gain > 1.45f && synth_patch(f).gain <= 1.5f, "OSC level upper endpoint did not update");
    parameter_drag(f, 429, 239, 234);
    require(synth_patch(f).gain == 0.0f, "OSC level lower endpoint did not mute");
    for (int wave = 0; wave < 4; ++wave) {
        tap(f, 100, 180);
        (void)advance(f, BLOCK_FRAMES);
    }
    require(synth_patch(f).waveform == APP_SYNTH_WAVE_WAVETABLE, "table position test did not select wavetable");
    parameter_drag(f, 234, 191, 429);
    require(near(synth_patch(f).wavetable_position, 1.0f, 0.0001f), "wavetable morph upper endpoint did not update");
    parameter_drag(f, 429, 191, 234);
    require(synth_patch(f).wavetable_position == 0.0f, "wavetable morph lower endpoint did not update");

    synth_tab(f, 1);
    parameter_drag(f, 130, 191, 429);
    require(near(synth_patch(f).filter_cutoff_hz, 20000.0f, 1.0f), "filter cutoff upper endpoint did not update");
    parameter_drag(f, 429, 191, 130);
    require(near(synth_patch(f).filter_cutoff_hz, 20.0f, 0.001f), "filter cutoff lower endpoint did not update");
    parameter_drag(f, 130, 239, 429);
    require(near(synth_patch(f).filter_resonance, 0.95f, 0.0001f), "resonance upper endpoint did not update");
    parameter_drag(f, 429, 239, 130);
    require(synth_patch(f).filter_resonance == 0.0f, "resonance lower endpoint did not update");

    synth_tab(f, 2);
    for (int slot = 0; slot < 4; ++slot) {
        const int left = 44 + slot * 101;
        parameter_drag(f, left, 238, left + 79);
        app_synth_patch_t patch = synth_patch(f);
        float value = slot == 0 ? patch.attack_s : slot == 1 ? patch.decay_s
                      : slot == 2 ? patch.sustain : patch.release_s;
        require(near(value, slot == 2 ? 1.0f : 5.0f, 0.001f), "ADSR upper endpoint did not update");
        parameter_drag(f, left + 79, 238, left);
        patch = synth_patch(f);
        value = slot == 0 ? patch.attack_s : slot == 1 ? patch.decay_s
                : slot == 2 ? patch.sustain : patch.release_s;
        require(value == 0.0f, "ADSR lower endpoint did not update");
    }

    synth_tab(f, 3);
    parameter_drag(f, 234, 191, 429);
    require(near(synth_patch(f).lfo_hz, 40.0f, 0.001f), "LFO rate upper endpoint did not update");
    parameter_drag(f, 429, 191, 234);
    require(synth_patch(f).lfo_hz == 0.0f, "LFO rate lower endpoint did not update");
    for (int target = 0; target < 3; ++target) {
        parameter_drag(f, 234, 239, 429);
        const float expected = target == APP_SYNTH_LFO_TARGET_PITCH ? 12.0f : 1.0f;
        require(synth_patch(f).lfo_target == (app_synth_lfo_target_t)target &&
                near(synth_patch(f).lfo_depth, expected, 0.001f), "LFO target/depth did not update");
        tap(f, 100, 246);
        (void)advance(f, BLOCK_FRAMES);
        require(synth_patch(f).lfo_depth == 0.0f, "changing LFO target did not clear depth");
    }
    for (int index = 0; index < 10; ++index) {
        tap(f, 151, 295);
    }
    require(f->model.synth_view.octave == 8, "octave up escaped its upper bound");
    for (int index = 0; index < 12; ++index) {
        tap(f, 51, 295);
    }
    require(f->model.synth_view.octave == 0, "octave down escaped its lower bound");
    require(f->model.view.page == TRACKER_PAGE_SYNTH, "parameter drag changed pages");
    synth_patch_queue_retry(f);
    draw(f);
}

int main(int argc, char **argv)
{
    require(argc == 2 || argc == 3, "usage: ui_interaction <aryx.s3m> [case-name]");
    const char *selected = argc == 3 ? argv[2] : "all";
    static const struct {
        const char *name;
        test_fn_t run;
    } cases[] = {
        {"navigation", navigation}, {"playback", playback}, {"mixer", mixer},
        {"pads", pads}, {"synth-keys", synth_keys}, {"synth-controls", synth_controls},
    };
    size_t size = 0;
    uint8_t *module = read_module(argv[1], &size);
    unsigned count = 0;
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        if (strcmp(selected, "all") != 0 && strcmp(selected, cases[index].name) != 0) {
            continue;
        }
        fixture_t *fixture = fixture_create(module, size);
        cases[index].run(fixture);
        fixture_destroy(fixture);
        printf("ui_interaction: %s PASS\n", cases[index].name);
        fflush(stdout);
        ++count;
    }
    require(count > 0, "unknown case-name");
    free(module);
    puts("ui_interaction: PASS");
    return EXIT_SUCCESS;
}
