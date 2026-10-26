// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tracker_engine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRACKER_MAX_ORDERS 256
#define TRACKER_MAX_PATTERNS 256
#define TRACKER_MAX_INSTRUMENTS 256
#define TRACKER_SOURCE_CHANNELS 32
#define TRACKER_SAMPLE_RATE 48000
#define TRACKER_TICK_SECONDS 2.5
#define TRACKER_WAVE_SAMPLES TRACKER_WAVEFORM_SAMPLES
#define TRACKER_EMPTY 0xFF
#define TRACKER_DEFAULT_SPEED 6
#define TRACKER_DEFAULT_TEMPO 125
#define TRACKER_MIX_HEADROOM 0.22f

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t loop_start;
    uint32_t loop_end;
    float volume;
    uint32_t c2_speed;
    bool loops;
} tracker_sample_t;

typedef struct {
    uint8_t note;
    uint8_t instrument;
    uint8_t volume;
    uint8_t effect;
    uint8_t param;
} tracker_cell_data_t;

typedef struct {
    /* Immutable after creation except order entries, which are edited through
     * the engine order lock. The source blob remains owned by the caller. */
    const uint8_t *blob;
    size_t blob_size;
    bool unsigned_samples;
    uint8_t orders[TRACKER_MAX_ORDERS];
    int order_count;
    tracker_cell_data_t *cells;
    int pattern_count;
    tracker_sample_t samples[TRACKER_MAX_INSTRUMENTS];
    int sample_count;
    int channel_count;
    /* Slot -> S3M source channel, and the inverse map used while decoding so
     * that pattern cells are stored in slot order (0..channel_count-1). */
    uint8_t channel_source[TRACKER_MAX_CHANNELS];
    uint8_t source_slot[TRACKER_SOURCE_CHANNELS];
    float channel_pan[TRACKER_MAX_CHANNELS];
    float global_volume;
    int initial_speed;
    int initial_tempo;
    char title[TRACKER_TITLE_BYTES];
} tracker_module_t;

typedef struct {
    const tracker_sample_t *sample;
    const tracker_sample_t *instrument;
    float position;
    float step;
    double period;
    double target_period;
    float volume;
    float pan;
    float volume_slide;
    float period_slide;
    int portamento_speed;
    uint8_t effect_memory[8];
} tracker_voice_t;

typedef struct {
    tracker_voice_t voices[TRACKER_MAX_CHANNELS];
    float channel_gain[TRACKER_MAX_CHANNELS];
    bool channel_mute[TRACKER_MAX_CHANNELS];
    bool channel_solo[TRACKER_MAX_CHANNELS];
    int solo_count;
    float master_peak;

    int16_t *wave;
    uint16_t wave_head[TRACKER_MAX_CHANNELS];
    float channel_peak[TRACKER_MAX_CHANNELS];
    bool master_clipped;
} tracker_mixer_t;

struct tracker_engine {
    tracker_module_t module;
    tracker_mixer_t mixer;

    int speed;
    int tempo;
    float speed_multiplier;
    int order_index;
    /* Pattern-pad mode: >= 0 loops a single pattern; < 0 follows the order
     * list. pending_pattern is the queued next pad (-1 = none). */
    int launch_pattern;
    int pending_pattern;
    bool stop_pending;
    int row;
    int tick;
    int tick_frames_left;
    bool playing;
    bool loop;
    uint64_t frames;

    unsigned status_seq;
    tracker_status_t snapshot;
    /* Guards mutable song/transport/mixer state across tasks. The device build
     * uses a priority-inheriting FreeRTOS mutex (state_mutex); host builds use
     * the atomic spin flag. */
    void *state_mutex;
    int state_lock;
};

/* tracker_module.c */
bool tracker_module_parse(tracker_module_t *module, const uint8_t *s3m,
                          size_t size);
void tracker_module_destroy(tracker_module_t *module);

/* tracker_mixer.c */
bool tracker_mixer_init(tracker_mixer_t *mixer,
                        const tracker_module_t *module);
void tracker_mixer_destroy(tracker_mixer_t *module);
void tracker_mixer_reset(tracker_mixer_t *mixer,
                         const tracker_module_t *module);
void tracker_mixer_clear_monitoring(tracker_mixer_t *mixer);
void tracker_mixer_render(tracker_mixer_t *mixer,
                          const tracker_module_t *module, int16_t *stereo,
                          int frames);
void tracker_mixer_render_float(tracker_mixer_t *mixer,
                                const tracker_module_t *module, float *stereo,
                                int frames);

/* Shared S3M little-endian readers with checked-range call sites. */
static inline uint16_t tracker_read_u16(const uint8_t *data, size_t offset)
{
    return (uint16_t)(data[offset] | ((uint16_t)data[offset + 1] << 8));
}

static inline uint32_t tracker_read_u32(const uint8_t *data, size_t offset)
{
    return (uint32_t)data[offset] |
           ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) |
           ((uint32_t)data[offset + 3] << 24);
}
