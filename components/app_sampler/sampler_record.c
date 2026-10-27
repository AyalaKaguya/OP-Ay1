// SPDX-License-Identifier: Apache-2.0
#include "sampler_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"

#define CAPTURE_STACK 4096U
#define CAPTURE_PRIORITY 5U

static void capture_worker(void *context)
{
    app_sampler_t *s = context;
    app_sampler_result_t result = APP_SAMPLER_OK;
    /* Task creation publishes the handle under the instance lock before this
     * notification; shutdown cannot free an unannounced worker. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    bool opened = false;
    if (!mic) { result = APP_SAMPLER_IO; goto finished; }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = APP_SAMPLER_CAPTURE_RATE, .channel = 2,
        .bits_per_sample = 16, .channel_mask = 0,
    };
    if (esp_codec_dev_open(mic, &format) != ESP_CODEC_DEV_OK) {
        result = APP_SAMPLER_IO; goto finished;
    }
    opened = true;
    float applied_gain = -1.0f;
    for (;;) {
        sampler_lock(s);
        const bool stop = s->capture_stop || s->closing || s->record.frames >= APP_SAMPLER_MAX_FRAMES;
        const float gain = s->record.gain_db;
        sampler_unlock(s);
        if (stop) { break; }
        if (gain != applied_gain) {
            if (esp_codec_dev_set_in_gain(mic, gain) != ESP_CODEC_DEV_OK) {
                result = APP_SAMPLER_IO; break;
            }
            applied_gain = gain;
        }
        if (esp_codec_dev_read(mic, s->capture_io, sizeof(s->capture_io)) != ESP_CODEC_DEV_OK) {
            result = APP_SAMPLER_IO; break;
        }
        sampler_lock(s);
        const uint32_t remaining = APP_SAMPLER_MAX_FRAMES - s->record.frames;
        const uint32_t count = remaining < SAMPLER_CAPTURE_BLOCK ? remaining : SAMPLER_CAPTURE_BLOCK;
        float peak = 0;
        for (uint32_t frame = 0; frame < count; ++frame) {
            const int32_t mono = ((int32_t)s->capture_io[frame * 2U] + s->capture_io[frame * 2U + 1U]) / 2;
            s->take->data[s->record.frames + frame] = (int16_t)mono;
            const float level = fabsf((float)mono / 32768.0f);
            if (level > peak) { peak = level; }
            if (s->record.monitor) {
                if (s->monitor_count == SAMPLER_MONITOR_FRAMES) {
                    s->monitor_tail = (s->monitor_tail + 1U) % SAMPLER_MONITOR_FRAMES;
                    --s->monitor_count;
                }
                s->monitor[s->monitor_head] = (int16_t)mono;
                s->monitor_head = (s->monitor_head + 1U) % SAMPLER_MONITOR_FRAMES;
                ++s->monitor_count;
            }
        }
        s->record.frames += count; s->record.peak = peak;
        sampler_unlock(s);
    }
finished:
    /* BSP retains codec/I2S handles. The owned codec API releases only its input
     * reference; the shared ES8311 DAC/I2S TX stay enabled for app_audio. */
    if (opened && esp_codec_dev_close(mic) != ESP_CODEC_DEV_OK) { result = APP_SAMPLER_IO; }
    sampler_lock(s);
    const uint32_t frames = s->record.frames;
    sampler_pcm_t *take = s->take;
    sampler_unlock(s);
    if (take && frames && frames < take->capacity) {
        sampler_pcm_t *smaller = heap_caps_realloc(take,
            sizeof(*take) + (size_t)frames * 2U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (smaller) { take = smaller; take->capacity = frames; }
    }
    sampler_lock(s);
    s->take = take;
    s->record.result = result;
    s->record.state = result == APP_SAMPLER_OK && frames ? APP_SAMPLER_RECORD_READY : APP_SAMPLER_RECORD_ERROR;
    if (!frames && result == APP_SAMPLER_OK) { s->record.result = APP_SAMPLER_BAD_STATE; }
    s->record.monitor = false; s->monitor_count = 0; s->record.peak = 0;
    s->capture_task = NULL;
    sampler_unlock(s);
    /* No instance access after publishing exit: shutdown may now release it. */
    vTaskDelete(NULL);
}
#endif

app_sampler_result_t app_sampler_record_start(app_sampler_t *s)
{
    if (!s) { return APP_SAMPLER_INVALID; }
#if !defined(ESP_PLATFORM)
    sampler_lock(s);
    s->record.state = APP_SAMPLER_RECORD_UNSUPPORTED;
    s->record.result = APP_SAMPLER_UNSUPPORTED;
    sampler_unlock(s); return APP_SAMPLER_UNSUPPORTED;
#else
    sampler_lock(s);
    if (s->capture_task || s->take || s->closing || s->editing) {
        sampler_unlock(s); return APP_SAMPLER_BUSY;
    }
    if (s->bank->count >= APP_SAMPLER_MAX_SAMPLES) { sampler_unlock(s); return APP_SAMPLER_FULL; }
    sampler_unlock(s);
    sampler_pcm_t *take = sampler_pcm_create(APP_SAMPLER_MAX_FRAMES);
    if (!take) { return APP_SAMPLER_NO_MEMORY; }
    sampler_lock(s);
    if (s->capture_task || s->take || s->closing || s->editing) {
        sampler_unlock(s); sampler_pcm_release(take); return APP_SAMPLER_BUSY;
    }
    s->take = take; s->capture_stop = false;
    s->record.frames = 0; s->record.peak = 0;
    s->monitor_count = 0; s->monitor_head = s->monitor_tail = 0;
    s->record.state = APP_SAMPLER_RECORD_RECORDING; s->record.result = APP_SAMPLER_OK;
    TaskHandle_t task = NULL;
    if (xTaskCreate(capture_worker, "sampler_mic", CAPTURE_STACK, s, CAPTURE_PRIORITY, &task) != pdPASS) {
        s->take = NULL; s->record.state = APP_SAMPLER_RECORD_ERROR;
        s->record.result = APP_SAMPLER_NO_MEMORY;
        sampler_unlock(s); sampler_pcm_release(take); return APP_SAMPLER_NO_MEMORY;
    }
    s->capture_task = task;
    sampler_unlock(s);
    xTaskNotifyGive(task);
    return APP_SAMPLER_OK;
#endif
}
void app_sampler_record_stop(app_sampler_t *s)
{
    if (!s) { return; }
    sampler_lock(s);
    s->capture_stop = true;
    if (s->record.state == APP_SAMPLER_RECORD_RECORDING) { s->record.state = APP_SAMPLER_RECORD_STOPPING; }
    sampler_unlock(s);
}
void app_sampler_record_get_status(app_sampler_t *s, app_sampler_record_status_t *status)
{
    if (!status) { return; }
    memset(status, 0, sizeof(*status));
    if (!s) { status->state = APP_SAMPLER_RECORD_UNSUPPORTED; status->result = APP_SAMPLER_UNSUPPORTED; return; }
    sampler_lock(s); *status = s->record; sampler_unlock(s);
}
app_sampler_result_t app_sampler_record_set_gain(app_sampler_t *s, float db)
{
    if (!s || !isfinite(db) || db < 0 || db > 42.0f) { return APP_SAMPLER_RANGE; }
    sampler_lock(s); s->record.gain_db = floorf(db / 6.0f + 0.5f) * 6.0f; sampler_unlock(s);
    return APP_SAMPLER_OK;
}
void app_sampler_record_set_monitor(app_sampler_t *s, bool enabled)
{
    if (!s) { return; }
    sampler_lock(s);
    s->record.monitor = enabled;
    s->monitor_count = 0; s->monitor_head = s->monitor_tail = 0;
    sampler_unlock(s);
}
static void take_info(app_sampler_sample_info_t *i, uint32_t frames, const char *name)
{
    memset(i, 0, sizeof(*i));
    snprintf(i->name, sizeof(i->name), "%s", name && *name ? name : "Recording");
    i->source = APP_SAMPLER_SOURCE_RECORDING;
    i->frames = frames; i->sample_rate = i->c2_speed = APP_SAMPLER_CAPTURE_RATE;
    i->trim_end = i->loop_end = frames; i->gain = 1.0f; i->default_volume = 1.0f;
}
app_sampler_result_t app_sampler_record_audition(app_sampler_t *s)
{
    if (!s) { return APP_SAMPLER_INVALID; }
    sampler_lock(s);
    if (s->record.state != APP_SAMPLER_RECORD_READY || !s->take || s->closing) {
        sampler_unlock(s); return APP_SAMPLER_BAD_STATE;
    }
    sampler_audition_clear_locked(s);
    s->audition = s->take; sampler_pcm_retain(s->audition);
    take_info(&s->audition_info, s->record.frames, "Recording");
    s->audition_position = 0; s->audition_playing = true;
    sampler_unlock(s); return APP_SAMPLER_OK;
}
app_sampler_result_t app_sampler_record_keep(app_sampler_t *s, const char *name, unsigned *index)
{
    if (!s) { return APP_SAMPLER_INVALID; }
    sampler_sample_t *sample = calloc(1, sizeof(*sample));
    if (!sample) { return APP_SAMPLER_NO_MEMORY; }
    sampler_lock(s);
    if (s->record.state != APP_SAMPLER_RECORD_READY || !s->take || s->closing) {
        sampler_unlock(s); free(sample); return APP_SAMPLER_BAD_STATE;
    }
    sample->pcm = s->take; sampler_pcm_retain(sample->pcm);
    take_info(&sample->info, s->record.frames, name);
    sampler_unlock(s);
    const app_sampler_result_t result = sampler_append(s, sample, index);
    if (result == APP_SAMPLER_OK) { app_sampler_record_discard(s); }
    sampler_pcm_release(sample->pcm); free(sample); return result;
}
void app_sampler_record_discard(app_sampler_t *s)
{
    if (!s) { return; }
    sampler_lock(s);
    if (s->capture_task) { sampler_unlock(s); return; }
    sampler_pcm_t *take = s->take;
    s->take = NULL; s->record.frames = 0; s->record.peak = 0;
    s->record.monitor = false; s->monitor_count = 0;
#if defined(ESP_PLATFORM)
    s->record.state = APP_SAMPLER_RECORD_IDLE; s->record.result = APP_SAMPLER_OK;
#else
    s->record.state = APP_SAMPLER_RECORD_UNSUPPORTED; s->record.result = APP_SAMPLER_UNSUPPORTED;
#endif
    if (s->audition == take) { sampler_audition_clear_locked(s); }
    sampler_unlock(s); sampler_pcm_release(take);
}
