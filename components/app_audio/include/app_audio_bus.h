// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_synth.h"
#include "tracker_engine.h"

#define APP_AUDIO_SAMPLE_RATE 48000
#define APP_AUDIO_BLOCK_FRAMES 512
#define APP_AUDIO_SCOPE_SAMPLES 2048

typedef struct app_audio_bus app_audio_bus_t;

typedef enum {
    APP_AUDIO_SOURCE_TRACKER,
    APP_AUDIO_SOURCE_SYNTH,
    APP_AUDIO_SOURCE_COUNT
} app_audio_source_t;

typedef struct {
    float master_peak;   /* Latest block, normalized before final saturation. */
    int master_level;    /* Latest block peak, 0..255 (255 = full scale). */
    bool clipped;        /* Latched until consume_clip(). */
    uint64_t frames;     /* Total output frames, including silence. */
} app_audio_bus_status_t;

/* Borrows 48 kHz sources until destroy; at least one source is required. NULL on
 * allocation/lock failure. The sources remain owned by the caller. */
app_audio_bus_t *app_audio_bus_create(tracker_engine_t *tracker,
                                      app_synth_t *synth);
/* All render/control users must have stopped before destroy(). */
void app_audio_bus_destroy(app_audio_bus_t *bus);

/* One audio owner; interleaved 48 kHz stereo. No allocation in render().
 * Source gains precede the sole output saturation. NULL bus produces silence. */
void app_audio_bus_render(app_audio_bus_t *bus, int16_t *stereo, int frames);

/* Thread safe; finite gain 0..2, defaults to 1. Invalid requests return false.
 * Gains are sampled once per render call. */
bool app_audio_bus_set_source_gain(app_audio_bus_t *bus,
                                    app_audio_source_t source, float gain);
float app_audio_bus_source_gain(app_audio_bus_t *bus, app_audio_source_t source);
void app_audio_bus_get_status(app_audio_bus_t *bus,
                              app_audio_bus_status_t *status);
bool app_audio_bus_consume_clip(app_audio_bus_t *bus);
/* Copies up to 2048 recent mono output samples, oldest first. Initial history
 * is zero. count is clamped to the history length. */
int app_audio_bus_waveform(app_audio_bus_t *bus, int16_t *out, int count);
tracker_engine_t *app_audio_bus_tracker(app_audio_bus_t *bus);
