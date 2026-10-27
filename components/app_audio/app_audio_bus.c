// SPDX-License-Identifier: Apache-2.0
#include "app_audio_bus.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#define BUS_RENDER_FRAMES 128
#define BUS_GAIN_SCALE 65536u

struct app_audio_bus {
    tracker_engine_t *tracker;
    app_synth_t *synth;
    atomic_uint gain[APP_AUDIO_SOURCE_COUNT];
    float tracker_samples[APP_AUDIO_BLOCK_FRAMES * 2];
    float synth_samples[BUS_RENDER_FRAMES * 2];
    int16_t scope[APP_AUDIO_SCOPE_SAMPLES];
    unsigned scope_head;
    app_audio_bus_status_t status;
#if defined(ESP_PLATFORM)
    SemaphoreHandle_t monitor_lock;
#else
    atomic_bool monitor_lock;
#endif
};

static void monitor_lock(app_audio_bus_t *bus)
{
#if defined(ESP_PLATFORM)
    (void)xSemaphoreTake(bus->monitor_lock, portMAX_DELAY);
#else
    while (atomic_exchange_explicit(&bus->monitor_lock, true,
                                    memory_order_acquire)) {
    }
#endif
}

static void monitor_unlock(app_audio_bus_t *bus)
{
#if defined(ESP_PLATFORM)
    (void)xSemaphoreGive(bus->monitor_lock);
#else
    atomic_store_explicit(&bus->monitor_lock, false, memory_order_release);
#endif
}

static int16_t to_sample(float value)
{
    if (value >= 1.0f) {
        return INT16_MAX;
    }
    if (value <= -1.0f) {
        return INT16_MIN;
    }
    return (int16_t)(value * 32767.0f);
}

app_audio_bus_t *app_audio_bus_create(tracker_engine_t *tracker,
                                      app_synth_t *synth)
{
    if (tracker == NULL && synth == NULL) {
        return NULL;
    }
    app_audio_bus_t *bus = calloc(1, sizeof(*bus));
    if (bus == NULL) {
        return NULL;
    }
    bus->tracker = tracker;
    bus->synth = synth;
    for (int source = 0; source < APP_AUDIO_SOURCE_COUNT; ++source) {
        atomic_init(&bus->gain[source], BUS_GAIN_SCALE);
    }
#if defined(ESP_PLATFORM)
    bus->monitor_lock = xSemaphoreCreateMutex();
    if (bus->monitor_lock == NULL) {
        free(bus);
        return NULL;
    }
#else
    atomic_init(&bus->monitor_lock, false);
#endif
    return bus;
}

void app_audio_bus_destroy(app_audio_bus_t *bus)
{
    if (bus == NULL) {
        return;
    }
#if defined(ESP_PLATFORM)
    vSemaphoreDelete(bus->monitor_lock);
#endif
    free(bus);
}

void app_audio_bus_render(app_audio_bus_t *bus, int16_t *stereo, int frames)
{
    if (stereo == NULL || frames <= 0) {
        return;
    }
    if (bus == NULL) {
        memset(stereo, 0, (size_t)frames * 2 * sizeof(*stereo));
        return;
    }
    const float tracker_gain = app_audio_bus_source_gain(bus,
                                                         APP_AUDIO_SOURCE_TRACKER);
    const float synth_gain = app_audio_bus_source_gain(bus, APP_AUDIO_SOURCE_SYNTH);
    float peak = 0.0f;
    bool clipped = false;
    for (int offset = 0; offset < frames;) {
        const int block = frames - offset < APP_AUDIO_BLOCK_FRAMES
                          ? frames - offset : APP_AUDIO_BLOCK_FRAMES;
        memset(bus->tracker_samples, 0, (size_t)block * 2 * sizeof(float));
        if (bus->tracker != NULL) {
            /* Tracker clears its source meters per render call. Render a full
             * device block so an early transient survives the later silence. */
            tracker_engine_render_float(bus->tracker, bus->tracker_samples, block);
        }
        for (int position = 0; position < block;) {
            const int count = block - position < BUS_RENDER_FRAMES
                              ? block - position : BUS_RENDER_FRAMES;
            memset(bus->synth_samples, 0, (size_t)count * 2 * sizeof(float));
            if (bus->synth != NULL) {
                app_synth_render(bus->synth, bus->synth_samples, count);
            }
            /* Only this short output/history copy shares a lock with the UI;
             * source renderers never run under the monitoring lock. */
            monitor_lock(bus);
            for (int frame = 0; frame < count; ++frame) {
                const int sample = frame * 2;
                const int tracker_sample = (position + frame) * 2;
                float left = bus->tracker_samples[tracker_sample] * tracker_gain +
                             bus->synth_samples[sample] * synth_gain;
                float right = bus->tracker_samples[tracker_sample + 1] * tracker_gain +
                              bus->synth_samples[sample + 1] * synth_gain;
                if (!isfinite(left) || !isfinite(right)) {
                    left = 0.0f;
                    right = 0.0f;
                    clipped = true;
                }
                /* Inputs are finite here; avoid libc signaling-NaN helpers in
                 * the per-sample meter without changing the summation order. */
                const float left_peak = fabsf(left);
                const float right_peak = fabsf(right);
                const float frame_peak = left_peak > right_peak ? left_peak : right_peak;
                if (frame_peak > peak) {
                    peak = frame_peak;
                }
                clipped = clipped || frame_peak >= 1.0f;
                const int16_t output_left = to_sample(left);
                const int16_t output_right = to_sample(right);
                stereo[(offset + position + frame) * 2] = output_left;
                stereo[(offset + position + frame) * 2 + 1] = output_right;
                bus->scope[bus->scope_head] =
                    (int16_t)(((int32_t)output_left + output_right) / 2);
                bus->scope_head = (bus->scope_head + 1) % APP_AUDIO_SCOPE_SAMPLES;
            }
            monitor_unlock(bus);
            position += count;
        }
        offset += block;
    }
    monitor_lock(bus);
    bus->status.master_peak = peak;
    bus->status.master_level = (int)((peak < 1.0f ? peak : 1.0f) * 255.0f + 0.5f);
    bus->status.clipped = bus->status.clipped || clipped;
    bus->status.frames += (uint64_t)frames;
    monitor_unlock(bus);
}

bool app_audio_bus_set_source_gain(app_audio_bus_t *bus,
                                   app_audio_source_t source, float gain)
{
    if (bus == NULL || source < 0 || source >= APP_AUDIO_SOURCE_COUNT ||
            !isfinite(gain) || gain < 0.0f || gain > 2.0f) {
        return false;
    }
    atomic_store_explicit(&bus->gain[source],
                          (unsigned)(gain * BUS_GAIN_SCALE + 0.5f),
                          memory_order_relaxed);
    return true;
}

float app_audio_bus_source_gain(app_audio_bus_t *bus, app_audio_source_t source)
{
    if (bus == NULL || source < 0 || source >= APP_AUDIO_SOURCE_COUNT) {
        return 0.0f;
    }
    return (float)atomic_load_explicit(&bus->gain[source], memory_order_relaxed) /
           BUS_GAIN_SCALE;
}

void app_audio_bus_get_status(app_audio_bus_t *bus, app_audio_bus_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (bus != NULL) {
        monitor_lock(bus);
        *status = bus->status;
        monitor_unlock(bus);
    }
}

bool app_audio_bus_consume_clip(app_audio_bus_t *bus)
{
    if (bus == NULL) {
        return false;
    }
    monitor_lock(bus);
    const bool clipped = bus->status.clipped;
    bus->status.clipped = false;
    monitor_unlock(bus);
    return clipped;
}

int app_audio_bus_waveform(app_audio_bus_t *bus, int16_t *out, int count)
{
    if (out == NULL || count <= 0) {
        return 0;
    }
    const int available = count < APP_AUDIO_SCOPE_SAMPLES
                          ? count : APP_AUDIO_SCOPE_SAMPLES;
    if (bus == NULL) {
        memset(out, 0, (size_t)available * sizeof(*out));
        return 0;
    }
    monitor_lock(bus);
    const unsigned start =
        (bus->scope_head + APP_AUDIO_SCOPE_SAMPLES - (unsigned)available) %
        APP_AUDIO_SCOPE_SAMPLES;
    for (int index = 0; index < available; ++index) {
        out[index] = bus->scope[(start + (unsigned)index) % APP_AUDIO_SCOPE_SAMPLES];
    }
    monitor_unlock(bus);
    return available;
}

tracker_engine_t *app_audio_bus_tracker(app_audio_bus_t *bus)
{
    return bus != NULL ? bus->tracker : NULL;
}
