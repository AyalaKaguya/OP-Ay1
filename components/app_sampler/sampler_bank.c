// SPDX-License-Identifier: Apache-2.0
#include "sampler_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#define BANK_HEADER_BYTES 16U
#define BANK_SAMPLE_BYTES 120U
#define BANK_VERSION 1U

void *sampler_alloc(size_t size)
{
#if defined(ESP_PLATFORM)
    /* Large PCM must not consume the internal memory needed by audio/DMA. */
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}

sampler_pcm_t *sampler_pcm_create(uint32_t frames)
{
    if (!frames || frames > APP_SAMPLER_MAX_FRAMES) {
        return NULL;
    }
    sampler_pcm_t *pcm = sampler_alloc(sizeof(*pcm) + (size_t)frames * 2U);
    if (pcm) {
        atomic_init(&pcm->references, 1U);
        pcm->capacity = frames;
    }
    return pcm;
}
void sampler_pcm_retain(sampler_pcm_t *pcm)
{
    if (pcm) {
        atomic_fetch_add_explicit(&pcm->references, 1U, memory_order_relaxed);
    }
}
void sampler_pcm_release(sampler_pcm_t *pcm)
{
    if (pcm && atomic_fetch_sub_explicit(&pcm->references, 1U, memory_order_acq_rel) == 1U) {
        free(pcm);
    }
}

app_sampler_bank_t *sampler_bank_clone(const app_sampler_bank_t *source)
{
    app_sampler_bank_t *bank = calloc(1, sizeof(*bank));
    if (bank && source) {
        memcpy(bank, source, sizeof(*bank));
        for (unsigned i = 0; i < bank->count; ++i) {
            sampler_pcm_retain(bank->samples[i].pcm);
        }
    }
    return bank;
}
void app_sampler_bank_destroy(app_sampler_bank_t *bank)
{
    if (bank) {
        for (unsigned i = 0; i < bank->count; ++i) {
            sampler_pcm_release(bank->samples[i].pcm);
        }
        free(bank);
    }
}
unsigned app_sampler_bank_count(const app_sampler_bank_t *bank)
{
    return bank ? bank->count : 0;
}
bool app_sampler_bank_get_info(const app_sampler_bank_t *bank, unsigned index,
                               app_sampler_sample_info_t *info)
{
    if (!bank || index >= bank->count || !info) {
        return false;
    }
    *info = bank->samples[index].info;
    return true;
}

bool sampler_info_valid(const app_sampler_sample_info_t *i)
{
    if (!i || (unsigned)i->source > APP_SAMPLER_SOURCE_RECORDING ||
            i->frames > APP_SAMPLER_MAX_FRAMES || i->sample_rate == 0 ||
            i->sample_rate > 384000U || i->c2_speed == 0 || i->c2_speed > 384000U ||
            !isfinite(i->gain) || i->gain < 0 || i->gain > 2 ||
            !isfinite(i->default_volume) || i->default_volume < 0 || i->default_volume > 1 ||
            i->original_slot > 256U || i->assigned_slot > 256U ||
            (!i->frames && i->assigned_slot) ||
            i->trim_start > i->trim_end || i->trim_end > i->frames ||
            (i->frames && i->trim_start == i->trim_end)) {
        return false;
    }
    return !i->loop_enabled || (i->loop_start >= i->trim_start &&
                                i->loop_start < i->loop_end && i->loop_end <= i->trim_end);
}
app_sampler_edit_t sampler_edit_from_info(const app_sampler_sample_info_t *i)
{
    app_sampler_edit_t edit = {i->trim_start, i->trim_end, i->loop_start,
                               i->loop_end, i->c2_speed, i->gain, i->loop_enabled
                              };
    return edit;
}
void sampler_info_set_edit(app_sampler_sample_info_t *i, const app_sampler_edit_t *e)
{
    i->trim_start = e->trim_start; i->trim_end = e->trim_end;
    i->loop_start = e->loop_start; i->loop_end = e->loop_end;
    i->c2_speed = e->c2_speed; i->gain = e->gain; i->loop_enabled = e->loop_enabled;
}

app_sampler_bank_t *app_sampler_bank_snapshot(app_sampler_t *sampler)
{
    if (!sampler) {
        return NULL;
    }
    sampler_lock(sampler);
    app_sampler_bank_t *copy = sampler_bank_clone(sampler->bank);
    sampler_unlock(sampler);
    return copy;
}
void app_sampler_bank_commit(app_sampler_t *sampler, app_sampler_bank_t *bank)
{
    if (!sampler || !bank) {
        return;
    }
    sampler_lock(sampler);
    app_sampler_bank_t *old = sampler->bank, *undo = sampler->undo;
    sampler->bank = bank; sampler->undo = NULL;
    sampler->editing = false;
    ++sampler->revision;
    sampler_audition_clear_locked(sampler);
    sampler_unlock(sampler);
    app_sampler_bank_destroy(old); app_sampler_bank_destroy(undo);
}

app_sampler_result_t app_sampler_bank_encode(const app_sampler_bank_t *bank,
                                             uint8_t **bytes, size_t *size)
{
    if (!bank || !bytes || !size || bank->count > APP_SAMPLER_MAX_SAMPLES) {
        return APP_SAMPLER_INVALID;
    }
    size_t total = BANK_HEADER_BYTES + bank->count * BANK_SAMPLE_BYTES;
    for (unsigned i = 0; i < bank->count; ++i) {
        if (!sampler_info_valid(&bank->samples[i].info)) {
            return APP_SAMPLER_INVALID;
        }
        total += (size_t)bank->samples[i].info.frames * 2U;
    }
    if (total > APP_SAMPLER_BANK_PCM_BYTES + BANK_HEADER_BYTES +
            APP_SAMPLER_MAX_SAMPLES * BANK_SAMPLE_BYTES) {
        return APP_SAMPLER_RANGE;
    }
    uint8_t *data = sampler_alloc(total);
    if (!data) {
        return APP_SAMPLER_NO_MEMORY;
    }
    memset(data, 0, BANK_HEADER_BYTES);
    memcpy(data, "AYSB", 4); sampler_put32(data + 4, BANK_VERSION);
    sampler_put32(data + 8, bank->count); sampler_put32(data + 12, (uint32_t)total);
    size_t cursor = BANK_HEADER_BYTES;
    for (unsigned index = 0; index < bank->count; ++index) {
        const sampler_sample_t *sample = &bank->samples[index];
        const app_sampler_sample_info_t *i = &sample->info;
        uint8_t *p = data + cursor;
        memset(p, 0, BANK_SAMPLE_BYTES);
        memcpy(p, i->name, APP_SAMPLER_NAME_BYTES);
        uint32_t gain; memcpy(&gain, &i->gain, 4);
        uint32_t volume; memcpy(&volume, &i->default_volume, 4);
        const uint32_t fields[] = {(uint32_t)i->source, i->frames, i->sample_rate,
                                   i->c2_speed, i->trim_start, i->trim_end, i->loop_start, i->loop_end,
                                   gain, i->loop_enabled ? 1U : 0U, i->original_slot, i->assigned_slot, i->frames, volume
                                  };
        for (unsigned f = 0; f < 14U; ++f) {
            sampler_put32(p + 64U + f * 4U, fields[f]);
        }
        cursor += BANK_SAMPLE_BYTES;
        for (uint32_t frame = 0; frame < i->frames; ++frame) {
            const uint16_t value = (uint16_t)sample->pcm->data[frame];
            data[cursor++] = (uint8_t)value; data[cursor++] = (uint8_t)(value >> 8);
        }
    }
    *bytes = data; *size = total;
    return APP_SAMPLER_OK;
}

app_sampler_bank_t *app_sampler_bank_prepare(const uint8_t *data, size_t size,
                                             app_sampler_result_t *result)
{
    app_sampler_result_t error = APP_SAMPLER_FORMAT;
    app_sampler_bank_t *bank = NULL;
    if (!data || size < BANK_HEADER_BYTES || memcmp(data, "AYSB", 4) ||
            sampler_u32(data + 4) != BANK_VERSION || sampler_u32(data + 12) != size) {
        goto done;
    }
    const unsigned count = sampler_u32(data + 8);
    if (count > APP_SAMPLER_MAX_SAMPLES) {
        error = APP_SAMPLER_FULL;
        goto done;
    }
    bank = sampler_bank_clone(NULL);
    if (!bank) {
        error = APP_SAMPLER_NO_MEMORY;
        goto done;
    }
    size_t cursor = BANK_HEADER_BYTES;
    for (unsigned index = 0; index < count; ++index) {
        if (cursor > size || BANK_SAMPLE_BYTES > size - cursor) {
            goto fail;
        }
        const uint8_t *p = data + cursor;
        sampler_sample_t *sample = &bank->samples[bank->count++];
        app_sampler_sample_info_t *i = &sample->info;
        memcpy(i->name, p, 64); i->name[63] = '\0';
        i->source = (app_sampler_source_t)sampler_u32(p + 64);
        i->frames = sampler_u32(p + 68); i->sample_rate = sampler_u32(p + 72);
        i->c2_speed = sampler_u32(p + 76); i->trim_start = sampler_u32(p + 80);
        i->trim_end = sampler_u32(p + 84); i->loop_start = sampler_u32(p + 88);
        i->loop_end = sampler_u32(p + 92);
        const uint32_t gain = sampler_u32(p + 96); memcpy(&i->gain, &gain, 4);
        const uint32_t volume = sampler_u32(p + 116); memcpy(&i->default_volume, &volume, 4);
        const uint32_t flags = sampler_u32(p + 100);
        const uint32_t original = sampler_u32(p + 104), assigned = sampler_u32(p + 108);
        if (original > 256U || assigned > 256U || flags > 1U) {
            goto fail;
        }
        for (unsigned previous = 0; assigned && previous < index; ++previous) {
            if (bank->samples[previous].info.assigned_slot == assigned) {
                goto fail;
            }
        }
        i->loop_enabled = flags != 0; i->original_slot = (uint16_t)original;
        i->assigned_slot = (uint16_t)assigned;
        if (!sampler_info_valid(i) || sampler_u32(p + 112) != i->frames) {
            goto fail;
        }
        cursor += BANK_SAMPLE_BYTES;
        const size_t pcm_bytes = (size_t)i->frames * 2U;
        if (pcm_bytes > size - cursor || pcm_bytes > APP_SAMPLER_BANK_PCM_BYTES - bank->pcm_bytes) {
            goto fail;
        }
        if (i->frames) {
            sample->pcm = sampler_pcm_create(i->frames);
            if (!sample->pcm) {
                error = APP_SAMPLER_NO_MEMORY;
                goto fail;
            }
            for (uint32_t frame = 0; frame < i->frames; ++frame) {
                sample->pcm->data[frame] = (int16_t)sampler_u16(data + cursor + frame * 2U);
            }
        }
        bank->pcm_bytes += pcm_bytes; cursor += pcm_bytes;
    }
    if (cursor != size) {
        goto fail;
    }
    error = APP_SAMPLER_OK;
    goto done;
fail:
    app_sampler_bank_destroy(bank); bank = NULL;
done:
    if (result) {
        *result = error;
    }
    return bank;
}

static tracker_pcm_sample_t tracker_sample(const sampler_sample_t *sample)
{
    const app_sampler_sample_info_t *i = &sample->info;
    tracker_pcm_sample_t output = {0};
    if (sample->pcm && i->frames) {
        output.pcm = sample->pcm->data + i->trim_start;
        output.frames = i->trim_end - i->trim_start;
        output.c2_speed = i->c2_speed; output.volume = i->default_volume; output.gain = i->gain;
        output.loop_enabled = i->loop_enabled;
        output.loop_start = i->loop_enabled ? i->loop_start - i->trim_start : 0;
        output.loop_end = i->loop_enabled ? i->loop_end - i->trim_start : 0;
    }
    return output;
}
app_sampler_result_t sampler_apply_live(const sampler_sample_t *sample,
                                        tracker_engine_t *engine)
{
    if (!sample->info.assigned_slot) {
        return APP_SAMPLER_OK;
    }
    if (!engine) {
        return APP_SAMPLER_BAD_STATE;
    }
    const tracker_pcm_sample_t pcm = tracker_sample(sample);
    return tracker_engine_replace_sample(engine, sample->info.assigned_slot, &pcm)
           ? APP_SAMPLER_OK : APP_SAMPLER_NO_MEMORY;
}
app_sampler_result_t app_sampler_bank_apply_slots(const app_sampler_bank_t *bank,
                                                  tracker_engine_prepared_t *song)
{
    if (!bank || !song) {
        return APP_SAMPLER_INVALID;
    }
    for (unsigned index = 0; index < bank->count; ++index) {
        const sampler_sample_t *s = &bank->samples[index];
        if (s->info.assigned_slot) {
            const tracker_pcm_sample_t pcm = tracker_sample(s);
            if (!tracker_engine_prepared_replace_sample(song, s->info.assigned_slot, &pcm)) {
                return APP_SAMPLER_NO_MEMORY;
            }
        }
    }
    return APP_SAMPLER_OK;
}

app_sampler_result_t app_sampler_assign_slot(app_sampler_t *sampler,
                                             unsigned index, tracker_engine_t *engine, unsigned slot)
{
    if (!sampler || !engine || slot == 0 || slot > 256U) {
        return APP_SAMPLER_INVALID;
    }
    /* Clone metadata and retain PCM before any fallible deep copy. The worker
     * serializes this operation against project switches. */
    sampler_lock(sampler);
    if (sampler->editing || sampler->closing || index >= sampler->bank->count) {
        sampler_unlock(sampler); return APP_SAMPLER_BUSY;
    }
    app_sampler_bank_t *bank = sampler_bank_clone(sampler->bank);
    const uint64_t revision = sampler->revision;
    sampler_unlock(sampler);
    if (!bank) {
        return APP_SAMPLER_NO_MEMORY;
    }
    if (!bank->samples[index].info.frames) {
        app_sampler_bank_destroy(bank);
        return APP_SAMPLER_BAD_STATE;
    }
    const tracker_pcm_sample_t pcm = tracker_sample(&bank->samples[index]);
    sampler_lock(sampler);
    if (sampler->revision != revision || sampler->editing || sampler->closing) {
        sampler_unlock(sampler); app_sampler_bank_destroy(bank); return APP_SAMPLER_BUSY;
    }
    /* Assignment is a worker operation. Holding this lock prevents a second
     * mutation between engine replacement and the corresponding saved overlay. */
    if (!tracker_engine_assign_sample(engine, slot, bank->samples[index].info.assigned_slot, &pcm)) {
        sampler_unlock(sampler); app_sampler_bank_destroy(bank); return APP_SAMPLER_NO_MEMORY;
    }
    for (unsigned j = 0; j < bank->count; ++j) {
        if (bank->samples[j].info.assigned_slot == slot) {
            bank->samples[j].info.assigned_slot = 0;
        }
    }
    bank->samples[index].info.assigned_slot = (uint16_t)slot;
    app_sampler_bank_t *old = sampler->bank, *undo = sampler->undo;
    sampler->bank = bank; sampler->undo = NULL; ++sampler->revision;
    sampler->engine = engine;
    sampler_unlock(sampler);
    app_sampler_bank_destroy(old); app_sampler_bank_destroy(undo);
    return APP_SAMPLER_OK;
}
