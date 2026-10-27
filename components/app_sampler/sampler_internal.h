// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_sampler.h"
#include <stdatomic.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
typedef SemaphoreHandle_t sampler_lock_t;
#else
typedef atomic_bool sampler_lock_t;
#endif

#define SAMPLER_MONITOR_FRAMES 4096U
#define SAMPLER_CAPTURE_BLOCK 512U

typedef struct {
    atomic_uint references;
    uint32_t capacity;
    int16_t data[];
} sampler_pcm_t;
typedef struct {
    app_sampler_sample_info_t info;
    sampler_pcm_t *pcm;
} sampler_sample_t;
struct app_sampler_bank {
    unsigned count;
    size_t pcm_bytes;
    sampler_sample_t samples[APP_SAMPLER_MAX_SAMPLES];
};
struct app_sampler {
    sampler_lock_t lock;
    app_sampler_bank_t *bank, *undo;
    tracker_engine_t *engine;
    unsigned undo_index;
    uint64_t revision;
    bool editing;
    unsigned edit_index;
    uint64_t edit_revision;
    app_sampler_edit_t edit;
    sampler_pcm_t *audition;
    app_sampler_sample_info_t audition_info;
    double audition_position;
    bool audition_playing;
    app_sampler_record_status_t record;
    sampler_pcm_t *take;
    bool capture_stop, closing;
    void *capture_task;
    int16_t capture_io[SAMPLER_CAPTURE_BLOCK * 2U];
    int16_t monitor[SAMPLER_MONITOR_FRAMES];
    unsigned monitor_head, monitor_tail, monitor_count;
};

void sampler_lock(app_sampler_t *sampler);
void sampler_unlock(app_sampler_t *sampler);
void *sampler_alloc(size_t size);
sampler_pcm_t *sampler_pcm_create(uint32_t frames);
void sampler_pcm_retain(sampler_pcm_t *pcm);
void sampler_pcm_release(sampler_pcm_t *pcm);
app_sampler_bank_t *sampler_bank_clone(const app_sampler_bank_t *source);
bool sampler_info_valid(const app_sampler_sample_info_t *info);
app_sampler_edit_t sampler_edit_from_info(const app_sampler_sample_info_t *info);
void sampler_info_set_edit(app_sampler_sample_info_t *info, const app_sampler_edit_t *edit);
app_sampler_result_t sampler_append(app_sampler_t *sampler, sampler_sample_t *sample,
                                    unsigned *index);
void sampler_audition_clear_locked(app_sampler_t *sampler);
app_sampler_result_t sampler_apply_live(const sampler_sample_t *sample,
                                        tracker_engine_t *engine);

static inline uint16_t sampler_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static inline uint32_t sampler_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void sampler_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
