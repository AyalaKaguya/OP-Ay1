// SPDX-License-Identifier: Apache-2.0
#include "sampler_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void sampler_lock(app_sampler_t *s)
{
#if defined(ESP_PLATFORM)
    (void)xSemaphoreTake(s->lock, portMAX_DELAY);
#else
    while (atomic_exchange_explicit(&s->lock, true, memory_order_acquire)) { }
#endif
}
void sampler_unlock(app_sampler_t *s)
{
#if defined(ESP_PLATFORM)
    (void)xSemaphoreGive(s->lock);
#else
    atomic_store_explicit(&s->lock, false, memory_order_release);
#endif
}

app_sampler_t *app_sampler_create(void)
{
    app_sampler_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->bank = sampler_bank_clone(NULL);
#if defined(ESP_PLATFORM)
    s->lock = xSemaphoreCreateMutex();
    if (!s->lock || !s->bank) {
        if (s->lock) {
            vSemaphoreDelete(s->lock);
        }
        app_sampler_bank_destroy(s->bank); free(s); return NULL;
    }
    s->record.state = APP_SAMPLER_RECORD_IDLE;
#else
    atomic_init(&s->lock, false);
    if (!s->bank) {
        free(s);
        return NULL;
    }
    s->record.state = APP_SAMPLER_RECORD_UNSUPPORTED;
    s->record.result = APP_SAMPLER_UNSUPPORTED;
#endif
    s->record.sample_rate = APP_SAMPLER_CAPTURE_RATE;
    s->record.max_frames = APP_SAMPLER_MAX_FRAMES;
    s->record.gain_db = 18.0f;
    s->record.monitor = false;
    return s;
}
bool app_sampler_bind_engine(app_sampler_t *s, tracker_engine_t *engine)
{
    if (!s || !engine) {
        return false;
    }
    sampler_lock(s);
    const bool allowed = !s->engine || s->engine == engine;
    if (allowed) {
        s->engine = engine;
    }
    sampler_unlock(s);
    return allowed;
}
const char *app_sampler_result_name(app_sampler_result_t result)
{
    static const char *const names[] = {"OK", "Invalid request", "Unsupported format",
                                        "Out of range", "Not enough memory", "Sample bank full", "Busy", "I/O error",
                                        "Recording unsupported on host", "Invalid state"
                                       };
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "Unknown error";
}
unsigned app_sampler_count(app_sampler_t *s)
{
    if (!s) {
        return 0;
    }
    sampler_lock(s); const unsigned count = s->bank->count; sampler_unlock(s);
    return count;
}
uint64_t app_sampler_revision(app_sampler_t *s)
{
    if (!s) {
        return 0;
    }
    sampler_lock(s); const uint64_t revision = s->revision; sampler_unlock(s);
    return revision;
}
bool app_sampler_get_info(app_sampler_t *s, unsigned index, app_sampler_sample_info_t *info)
{
    if (!s) {
        return false;
    }
    sampler_lock(s);
    const bool ok = app_sampler_bank_get_info(s->bank, index, info);
    sampler_unlock(s); return ok;
}
unsigned app_sampler_waveform(app_sampler_t *s, unsigned index, int16_t *out, unsigned capacity)
{
    if (!s || !out || !capacity) {
        return 0;
    }
    if (capacity > 2048U) {
        capacity = 2048U;
    }
    sampler_lock(s);
    if (index >= s->bank->count || !s->bank->samples[index].pcm) {
        sampler_unlock(s); return 0;
    }
    const sampler_sample_t *sample = &s->bank->samples[index];
    const unsigned count = sample->info.frames < capacity ? sample->info.frames : capacity;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t frame = (uint32_t)((uint64_t)i * sample->info.frames / count);
        out[i] = sample->pcm->data[frame];
    }
    sampler_unlock(s); return count;
}

app_sampler_result_t sampler_append(app_sampler_t *s, sampler_sample_t *sample, unsigned *index)
{
    sampler_lock(s);
    if (s->closing || s->editing) {
        sampler_unlock(s);
        return APP_SAMPLER_BUSY;
    }
    if (s->bank->count >= APP_SAMPLER_MAX_SAMPLES ||
            (size_t)sample->info.frames * 2U > APP_SAMPLER_BANK_PCM_BYTES - s->bank->pcm_bytes) {
        sampler_unlock(s); return APP_SAMPLER_FULL;
    }
    app_sampler_bank_t *bank = sampler_bank_clone(s->bank);
    if (!bank) {
        sampler_unlock(s);
        return APP_SAMPLER_NO_MEMORY;
    }
    const unsigned added = bank->count++;
    bank->samples[added] = *sample;
    sampler_pcm_retain(sample->pcm);
    bank->pcm_bytes += (size_t)sample->info.frames * 2U;
    app_sampler_bank_t *old = s->bank, *undo = s->undo;
    s->bank = bank; s->undo = NULL; ++s->revision;
    sampler_unlock(s);
    app_sampler_bank_destroy(old); app_sampler_bank_destroy(undo);
    if (index) {
        *index = added;
    }
    return APP_SAMPLER_OK;
}
app_sampler_result_t app_sampler_edit_begin(app_sampler_t *s, unsigned index)
{
    if (!s) {
        return APP_SAMPLER_INVALID;
    }
    sampler_lock(s);
    if (s->closing || index >= s->bank->count || !s->bank->samples[index].info.frames) {
        sampler_unlock(s); return APP_SAMPLER_BAD_STATE;
    }
    s->editing = true; s->edit_index = index; s->edit_revision = s->revision;
    s->edit = sampler_edit_from_info(&s->bank->samples[index].info);
    sampler_unlock(s); return APP_SAMPLER_OK;
}
bool app_sampler_edit_get(app_sampler_t *s, app_sampler_edit_t *edit)
{
    if (!s || !edit) {
        return false;
    }
    sampler_lock(s); const bool editing = s->editing;
    if (editing) {
        *edit = s->edit;
    }
    sampler_unlock(s); return editing;
}
app_sampler_result_t app_sampler_edit_set(app_sampler_t *s, const app_sampler_edit_t *edit)
{
    if (!s || !edit) {
        return APP_SAMPLER_INVALID;
    }
    sampler_lock(s);
    if (!s->editing || s->revision != s->edit_revision) {
        sampler_unlock(s); return APP_SAMPLER_BAD_STATE;
    }
    app_sampler_sample_info_t info = s->bank->samples[s->edit_index].info;
    sampler_info_set_edit(&info, edit);
    if (!sampler_info_valid(&info)) {
        sampler_unlock(s);
        return APP_SAMPLER_RANGE;
    }
    s->edit = *edit; sampler_unlock(s); return APP_SAMPLER_OK;
}
app_sampler_result_t app_sampler_edit_apply(app_sampler_t *s)
{
    if (!s) {
        return APP_SAMPLER_INVALID;
    }
    sampler_lock(s);
    if (!s->editing || s->closing || s->revision != s->edit_revision) {
        sampler_unlock(s); return APP_SAMPLER_BAD_STATE;
    }
    app_sampler_bank_t *bank = sampler_bank_clone(s->bank);
    if (!bank) {
        sampler_unlock(s);
        return APP_SAMPLER_NO_MEMORY;
    }
    sampler_info_set_edit(&bank->samples[s->edit_index].info, &s->edit);
    const app_sampler_result_t result = sampler_apply_live(&bank->samples[s->edit_index], s->engine);
    if (result != APP_SAMPLER_OK) {
        sampler_unlock(s); app_sampler_bank_destroy(bank); return result;
    }
    app_sampler_bank_t *discard = s->undo;
    s->undo = s->bank; s->undo_index = s->edit_index;
    s->bank = bank; s->editing = false; ++s->revision;
    sampler_audition_clear_locked(s);
    sampler_unlock(s); app_sampler_bank_destroy(discard); return APP_SAMPLER_OK;
}
void app_sampler_edit_cancel(app_sampler_t *s)
{
    if (!s) {
        return;
    }
    sampler_lock(s); s->editing = false; sampler_audition_clear_locked(s); sampler_unlock(s);
}
app_sampler_result_t app_sampler_undo(app_sampler_t *s)
{
    if (!s) {
        return APP_SAMPLER_INVALID;
    }
    sampler_lock(s);
    if (!s->undo || s->editing || s->closing) {
        sampler_unlock(s);
        return APP_SAMPLER_BAD_STATE;
    }
    const app_sampler_result_t result = sampler_apply_live(&s->undo->samples[s->undo_index], s->engine);
    if (result != APP_SAMPLER_OK) {
        sampler_unlock(s);
        return result;
    }
    app_sampler_bank_t *old = s->bank; s->bank = s->undo; s->undo = NULL;
    ++s->revision; sampler_audition_clear_locked(s);
    sampler_unlock(s); app_sampler_bank_destroy(old); return APP_SAMPLER_OK;
}

void sampler_audition_clear_locked(app_sampler_t *s)
{
    sampler_pcm_release(s->audition); s->audition = NULL;
    s->audition_playing = false; s->audition_position = 0;
}
app_sampler_result_t app_sampler_audition(app_sampler_t *s, unsigned index)
{
    if (!s) {
        return APP_SAMPLER_INVALID;
    }
    sampler_lock(s);
    if (index >= s->bank->count || !s->bank->samples[index].pcm || s->closing) {
        sampler_unlock(s); return APP_SAMPLER_BAD_STATE;
    }
    sampler_audition_clear_locked(s);
    s->audition = s->bank->samples[index].pcm; sampler_pcm_retain(s->audition);
    s->audition_info = s->bank->samples[index].info;
    if (s->editing && s->edit_index == index) {
        sampler_info_set_edit(&s->audition_info, &s->edit);
    }
    s->audition_position = s->audition_info.trim_start; s->audition_playing = true;
    sampler_unlock(s); return APP_SAMPLER_OK;
}
void app_sampler_audition_stop(app_sampler_t *s)
{
    if (!s) {
        return;
    }
    sampler_lock(s); sampler_audition_clear_locked(s); sampler_unlock(s);
}
void app_sampler_render(app_sampler_t *s, float *stereo, unsigned frames)
{
    if (!stereo) {
        return;
    }
    memset(stereo, 0, (size_t)frames * 2U * sizeof(float));
    if (!s) {
        return;
    }
    sampler_lock(s);
    for (unsigned f = 0; f < frames; ++f) {
        float value = 0;
        if (s->audition_playing && s->audition) {
            const app_sampler_sample_info_t *i = &s->audition_info;
            double position = s->audition_position;
            if (i->loop_enabled && position >= i->loop_end) {
                position = i->loop_start + fmod(position - i->loop_start, i->loop_end - i->loop_start);
            }
            if (position < i->trim_end) {
                const uint32_t frame = (uint32_t)position;
                uint32_t next = frame + 1U;
                if (i->loop_enabled && next >= i->loop_end) {
                    next = i->loop_start;
                }
                if (next >= i->trim_end) {
                    next = frame;
                }
                const float fraction = (float)(position - frame);
                value = ((float)s->audition->data[frame] +
                         ((float)s->audition->data[next] - s->audition->data[frame]) * fraction) /
                        32768.0f * i->gain * i->default_volume;
                s->audition_position = position + (double)i->c2_speed / APP_SAMPLER_CAPTURE_RATE;
            } else {
                s->audition_playing = false;
            }
        }
        if (s->record.monitor && s->monitor_count) {
            value += s->monitor[s->monitor_tail] / 32768.0f;
            s->monitor_tail = (s->monitor_tail + 1U) % SAMPLER_MONITOR_FRAMES;
            --s->monitor_count;
        }
        stereo[f * 2U] = value; stereo[f * 2U + 1U] = value;
    }
    sampler_unlock(s);
}

bool app_sampler_shutdown(app_sampler_t **pointer, uint32_t timeout_ms)
{
    if (!pointer || !*pointer) {
        return true;
    }
    app_sampler_t *s = *pointer;
    sampler_lock(s); s->closing = true; s->capture_stop = true; sampler_unlock(s);
#if defined(ESP_PLATFORM)
    const TickType_t started = xTaskGetTickCount(), timeout = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        sampler_lock(s); const bool running = s->capture_task != NULL; sampler_unlock(s);
        if (!running) {
            break;
        }
        if ((TickType_t)(xTaskGetTickCount() - started) >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5) ? pdMS_TO_TICKS(5) : 1);
    }
#else
    (void)timeout_ms;
#endif
    app_sampler_bank_destroy(s->bank); app_sampler_bank_destroy(s->undo);
    sampler_pcm_release(s->audition); sampler_pcm_release(s->take);
#if defined(ESP_PLATFORM)
    vSemaphoreDelete(s->lock);
#endif
    free(s); *pointer = NULL; return true;
}
void app_sampler_destroy(app_sampler_t *s)
{
    /* Lifetime contract requires stopped users; a blocked capture keeps its
     * borrowed resources alive instead of freeing them underneath the worker. */
    (void)app_sampler_shutdown(&s, 2000);
}
