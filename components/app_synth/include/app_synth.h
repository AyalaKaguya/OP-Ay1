// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SYNTH_SAMPLE_RATE 48000U
#define APP_SYNTH_VOICE_COUNT 4U
#define APP_SYNTH_COMMAND_CAPACITY 64U
#define APP_SYNTH_WAVETABLE_SIZE 256U
#define APP_SYNTH_MIN_NOTE 0
#define APP_SYNTH_MAX_NOTE 127

typedef struct app_synth app_synth_t;

typedef enum {
    APP_SYNTH_PATCH_INIT = 0,
    APP_SYNTH_PATCH_BASS,
    APP_SYNTH_PATCH_LEAD,
    APP_SYNTH_PATCH_PAD,
    APP_SYNTH_PATCH_COUNT,
} app_synth_patch_id_t;

typedef enum {
    APP_SYNTH_WAVE_SAW = 0,
    APP_SYNTH_WAVE_SQUARE,
    APP_SYNTH_WAVE_TRIANGLE,
    APP_SYNTH_WAVE_SINE,
    APP_SYNTH_WAVE_WAVETABLE,
} app_synth_waveform_t;

typedef enum {
    APP_SYNTH_LFO_TARGET_PITCH = 0,
    APP_SYNTH_LFO_TARGET_CUTOFF,
    APP_SYNTH_LFO_TARGET_AMP,
} app_synth_lfo_target_t;

/**
 * A patch is a snapshot of the controls used by every new and active voice.
 * Values outside the documented ranges are clamped by app_synth_set_patch().
 * Time values are seconds, frequencies are Hz, and gain/depth values are
 * linear. For the LFO, depth is semitones for PITCH, a 0..1 cutoff ratio for
 * CUTOFF, and a 0..1 amplitude ratio for AMP. The patch is copied into a
 * bounded command channel, so callers may reuse it after the call returns.
 */
typedef struct {
    app_synth_waveform_t waveform; /* saw, square, triangle, sine or wavetable */
    float wavetable_position;       /* 0..1, morphs the generated table */
    float detune_cents;             /* -100..100 */
    float gain;                     /* 0..1.5 */
    float pan;                      /* -1 left .. 1 right */

    float filter_cutoff_hz;         /* 20..20000 */
    float filter_resonance;         /* 0..0.95 */
    float attack_s;                 /* 0..5 */
    float decay_s;                  /* 0..5 */
    float sustain;                  /* 0..1 */
    float release_s;                /* 0..5 */

    float lfo_hz;                   /* 0..40 */
    float lfo_depth;                /* target-specific, see above */
    app_synth_lfo_target_t lfo_target;
} app_synth_patch_t;

typedef struct {
    uint64_t rendered_frames;
    uint32_t active_voices;
    uint32_t queue_depth;
    uint32_t dropped_commands;
    uint32_t stolen_voices;
    uint32_t emergency_releases;
    uint32_t panic_count;
    app_synth_patch_id_t patch_id;
    float peak;
    bool clipped;
} app_synth_status_t;

/** Creates an instance. The normal device rate is 48000 Hz. */
app_synth_t *app_synth_create(float sample_rate);
void app_synth_destroy(app_synth_t *synth);

/** Queues a smoothed patch change. */
bool app_synth_set_patch(app_synth_t *synth, const app_synth_patch_t *patch);
/** Returns a coherent last-accepted patch. On false, out_patch is unchanged;
 * retain the previous successful snapshot until a later call succeeds. */
bool app_synth_get_patch(const app_synth_t *synth, app_synth_patch_t *out_patch);
bool app_synth_set_patch_id(app_synth_t *synth, app_synth_patch_id_t id);
app_synth_patch_t app_synth_patch_for_preset(app_synth_patch_id_t id);
const char *app_synth_preset_name(app_synth_patch_id_t id);

/** Queues a note event. note is MIDI 0..127, velocity is linear 0..1. */
bool app_synth_note_on(app_synth_t *synth, int note, float velocity);
bool app_synth_note_off(app_synth_t *synth, int note);
void app_synth_all_notes_off(app_synth_t *synth);
/** Schedules a hard voice reset after pending commands drain. */
void app_synth_panic(app_synth_t *synth);

/** Fills interleaved stereo floats, where 1.0 is PCM full scale. Source
 * headroom above +/-1 is preserved for the final output bus to mix/limit.
 * The output buffer is caller
 * owned, must hold frames*2 floats, and is overwritten (not accumulated).
 * This function performs no allocation. The render task is the sole owner of
 * voice state; control calls only enqueue commands or set emergency flags. */
void app_synth_render(app_synth_t *synth, float *stereo, size_t frames);
/** Returns a coherent DSP snapshot. On false, out_status is unchanged;
 * retain the previous successful snapshot until a later call succeeds. */
bool app_synth_get_status(const app_synth_t *synth, app_synth_status_t *out_status);

#ifdef __cplusplus
}
#endif
