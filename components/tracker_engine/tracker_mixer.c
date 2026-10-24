// SPDX-License-Identifier: Apache-2.0
/* Voice renderer, stereo mixer, and monitoring buffers. It has no knowledge of
 * S3M transport or the public lifecycle API. */

#include "tracker_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
static void *mixer_alloc(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : malloc(bytes);
}
#else
static void *mixer_alloc(size_t bytes) { return malloc(bytes); }
#endif

static int sample_value(const tracker_module_t *module,
                        const tracker_sample_t *sample, uint32_t index)
{
    if (index >= sample->length) {
        return 0;
    }
    const uint8_t raw = sample->data[index];
    return module->unsigned_samples ? (int)raw - 128 : (int)(int8_t)raw;
}

static void write_wave(tracker_mixer_t *mixer, int channel, float value)
{
    const float clamped = fminf(fmaxf(value, -1.0f), 1.0f);
    mixer->wave[channel * TRACKER_WAVE_SAMPLES + mixer->wave_head[channel]] =
        (int16_t)(clamped * 32767.0f);
    mixer->wave_head[channel] = (uint16_t)((mixer->wave_head[channel] + 1) %
                                           TRACKER_WAVE_SAMPLES);
}

static void mix_frame(tracker_mixer_t *mixer, const tracker_module_t *module,
                      float *left, float *right)
{
    *left = 0.0f;
    *right = 0.0f;
    for (int channel = 0; channel < module->channel_count; ++channel) {
        tracker_voice_t *voice = &mixer->voices[channel];
        const bool audible = !mixer->channel_mute[channel] &&
                             (mixer->solo_count == 0 ||
                              mixer->channel_solo[channel]);
        if (!audible || voice->sample == NULL || voice->step <= 0.0f) {
            write_wave(mixer, channel, 0.0f);
            mixer->channel_peak[channel] = 0.0f;
            continue;
        }

        const tracker_sample_t *sample = voice->sample;
        float position = voice->position;
        if (sample->loops && position >= (float)sample->loop_end) {
            const float loop_length =
                (float)(sample->loop_end - sample->loop_start);
            float wrapped = position - (float)sample->loop_start;
            wrapped -= floorf(wrapped / loop_length) * loop_length;
            position = (float)sample->loop_start + wrapped;
            voice->position = position;
        }

        float value = 0.0f;
        if (position < (float)sample->length) {
            const uint32_t index = (uint32_t)position;
            const float fraction = position - (float)index;
            const int current = sample_value(module, sample, index);
            int next;
            if (sample->loops && index + 1 >= sample->loop_end) {
                next = sample_value(module, sample, sample->loop_start);
            } else {
                next = sample_value(module, sample, index + 1);
            }
            value = ((float)current +
                     ((float)next - (float)current) * fraction) / 128.0f;
        }

        value *= voice->volume * mixer->channel_gain[channel];
        const float absolute = fabsf(value);
        if (absolute > mixer->channel_peak[channel]) {
            mixer->channel_peak[channel] = absolute;
        }
        write_wave(mixer, channel, value);
        *left += value * (1.0f - voice->pan) * 0.5f;
        *right += value * (1.0f + voice->pan) * 0.5f;

        voice->position = position + voice->step;
        if (!sample->loops && voice->position >= (float)sample->length) {
            voice->sample = NULL;
        }
    }
}

static int16_t float_to_i16(float value)
{
    if (value >= 32767.0f) {
        return INT16_MAX;
    }
    if (value <= -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

void tracker_mixer_clear_monitoring(tracker_mixer_t *mixer)
{
    if (mixer->wave != NULL) {
        memset(mixer->wave, 0,
               (size_t)TRACKER_MAX_CHANNELS * TRACKER_WAVE_SAMPLES *
                   sizeof(int16_t));
    }
    memset(mixer->wave_head, 0, sizeof(mixer->wave_head));
    memset(mixer->channel_peak, 0, sizeof(mixer->channel_peak));
    mixer->master_peak = 0.0f;
    mixer->master_clipped = false;
}

void tracker_mixer_destroy(tracker_mixer_t *mixer)
{
    if (mixer != NULL) {
        free(mixer->wave);
        mixer->wave = NULL;
    }
}

bool tracker_mixer_init(tracker_mixer_t *mixer, const tracker_module_t *module)
{
    memset(mixer, 0, sizeof(*mixer));
    mixer->wave = mixer_alloc((size_t)TRACKER_MAX_CHANNELS *
                              TRACKER_WAVE_SAMPLES * sizeof(int16_t));
    if (mixer->wave == NULL) {
        return false;
    }
    for (int channel = 0; channel < TRACKER_MAX_CHANNELS; ++channel) {
        mixer->channel_gain[channel] = 1.0f;
    }
    tracker_mixer_reset(mixer, module);
    return true;
}

void tracker_mixer_render(tracker_mixer_t *mixer,
                          const tracker_module_t *module, int16_t *stereo,
                          int frames)
{
    /* Peaks are accumulated across the sub-chunks of one engine render call;
     * the engine clears them before it starts, so a chunk split by a tick
     * boundary does not lose the level the meter should have seen. */
    for (int frame = 0; frame < frames; ++frame) {
        float left = 0.0f;
        float right = 0.0f;
        mix_frame(mixer, module, &left, &right);

        const float gain = TRACKER_MIX_HEADROOM * module->global_volume;
        const float scaled_left = left * gain * 32767.0f;
        const float scaled_right = right * gain * 32767.0f;
        stereo[frame * 2] = float_to_i16(scaled_left);
        stereo[frame * 2 + 1] = float_to_i16(scaled_right);
        const float peak = fmaxf(fabsf(scaled_left), fabsf(scaled_right));
        mixer->master_peak = fmaxf(mixer->master_peak, peak / 32768.0f);
        if (peak >= 32767.0f) {
            mixer->master_clipped = true;
        }
    }
}

void tracker_mixer_reset(tracker_mixer_t *mixer,
                         const tracker_module_t *module)
{
    for (int channel = 0; channel < TRACKER_MAX_CHANNELS; ++channel) {
        tracker_voice_t *voice = &mixer->voices[channel];
        memset(voice, 0, sizeof(*voice));
        voice->pan =
            channel < module->channel_count ? module->channel_pan[channel] : 0.0f;
        voice->volume = 1.0f;
    }
    tracker_mixer_clear_monitoring(mixer);
}
