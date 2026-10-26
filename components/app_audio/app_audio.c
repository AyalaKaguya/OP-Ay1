// SPDX-License-Identifier: Apache-2.0
/* A single DSP owner feeds a bounded PCM ring. Only the writer owns codec I/O.
 * Task startup is gated until all resources exist, so failed init rolls back
 * without interrupting a renderer while it owns a source lock. */
#include "app_audio.h"

#include <stdatomic.h>

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AUDIO_CHANNELS 2
#define AUDIO_CHUNK_FRAMES 512
#define AUDIO_RING_CHUNKS 4
#define AUDIO_DSP_STACK 4096
#define AUDIO_WRITER_STACK 3072
#define AUDIO_DSP_PRIORITY 7
#define AUDIO_WRITER_PRIORITY 6
#if CONFIG_FREERTOS_UNICORE
#define AUDIO_TASK_CORE tskNO_AFFINITY
#else
#define AUDIO_TASK_CORE 1
#endif
#define AUDIO_DEFAULT_VOLUME 100
#define AUDIO_STATS_PERIOD_US (5 * 1000 * 1000)

static const char *TAG = "app_audio";
typedef int16_t audio_chunk_t[AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS];

/* Process-lifetime singleton; init is called by the composition task only.
 * The bus is borrowed except for the compatibility init() wrapper. */
static struct {
    esp_codec_dev_handle_t codec;
    app_audio_bus_t *bus;
    app_audio_bus_t *owned_bus;
    TaskHandle_t dsp_task;
    TaskHandle_t writer_task;
    SemaphoreHandle_t free_chunks;
    SemaphoreHandle_t ready_chunks;
    audio_chunk_t ring[AUDIO_RING_CHUNKS];
    /* The same semaphore ownership transfer as PCM protects each timing slot. */
    int64_t render_us[AUDIO_RING_CHUNKS];
    atomic_int volume_req;
    int applied_volume;
    bool ready;
} s_audio = {.volume_req = AUDIO_DEFAULT_VOLUME, .applied_volume = -1};

static void apply_volume(void)
{
    const int volume = atomic_load(&s_audio.volume_req);
    if (volume != s_audio.applied_volume &&
        esp_codec_dev_set_out_vol(s_audio.codec, volume) == ESP_CODEC_DEV_OK) {
        s_audio.applied_volume = volume;
    }
}

static void dsp_task(void *argument)
{
    (void)argument;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int head = 0;
    for (;;) {
        if (xSemaphoreTake(s_audio.free_chunks, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const int64_t start = esp_timer_get_time();
        app_audio_bus_render(s_audio.bus, s_audio.ring[head], AUDIO_CHUNK_FRAMES);
        s_audio.render_us[head] = esp_timer_get_time() - start;
        head = (head + 1) % AUDIO_RING_CHUNKS;
        (void)xSemaphoreGive(s_audio.ready_chunks);
    }
}

static void writer_task(void *argument)
{
    (void)argument;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int tail = 0;
    /* Statistics have one owner. Timings arrive alongside the matching PCM
     * chunk instead of racing with a reset in another task. */
    int64_t stats_start = esp_timer_get_time();
    int64_t dsp_total = 0;
    int64_t dsp_max = 0;
    int64_t write_total = 0;
    int64_t write_max = 0;
    unsigned chunks = 0;
    for (;;) {
        if (xSemaphoreTake(s_audio.ready_chunks, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const int64_t render_us = s_audio.render_us[tail];
        dsp_total += render_us;
        if (render_us > dsp_max) {
            dsp_max = render_us;
        }
        apply_volume();
        const int64_t start = esp_timer_get_time();
        const int result = esp_codec_dev_write(s_audio.codec, s_audio.ring[tail],
                                                sizeof(audio_chunk_t));
        const int64_t elapsed = esp_timer_get_time() - start;
        write_total += elapsed;
        if (elapsed > write_max) {
            write_max = elapsed;
        }
        ++chunks;
        tail = (tail + 1) % AUDIO_RING_CHUNKS;
        (void)xSemaphoreGive(s_audio.free_chunks);
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec write failed: %d", result);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        const int64_t now = esp_timer_get_time();
        if (now - stats_start >= AUDIO_STATS_PERIOD_US) {
            ESP_LOGI(TAG, "dsp avg=%lldus max=%lldus | write avg=%lldus max=%lldus",
                     (long long)(dsp_total / chunks), (long long)dsp_max,
                     (long long)(write_total / chunks), (long long)write_max);
            stats_start = now;
            dsp_total = 0;
            dsp_max = 0;
            write_total = 0;
            write_max = 0;
            chunks = 0;
        }
    }
}

/* Only used while both task entry points are still waiting for notification. */
static void rollback_init(bool codec_open)
{
    if (s_audio.dsp_task != NULL) {
        vTaskDelete(s_audio.dsp_task);
        s_audio.dsp_task = NULL;
    }
    if (s_audio.writer_task != NULL) {
        vTaskDelete(s_audio.writer_task);
        s_audio.writer_task = NULL;
    }
    if (s_audio.free_chunks != NULL) {
        vSemaphoreDelete(s_audio.free_chunks);
        s_audio.free_chunks = NULL;
    }
    if (s_audio.ready_chunks != NULL) {
        vSemaphoreDelete(s_audio.ready_chunks);
        s_audio.ready_chunks = NULL;
    }
    if (codec_open) {
        (void)esp_codec_dev_set_out_mute(s_audio.codec, true);
        (void)esp_codec_dev_close(s_audio.codec);
    }
    /* BSP owns the cached codec handle and its I2S interfaces. */
    s_audio.codec = NULL;
    s_audio.bus = NULL;
    s_audio.applied_volume = -1;
}

esp_err_t app_audio_init_bus(app_audio_bus_t *bus)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio.ready) {
        return s_audio.bus == bus ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = ESP_FAIL;
    bool codec_open = false;
    s_audio.codec = bsp_audio_codec_speaker_init();
    if (s_audio.codec == NULL) {
        ESP_LOGE(TAG, "speaker init failed");
        goto fail;
    }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = APP_AUDIO_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = AUDIO_CHANNELS,
    };
    if (esp_codec_dev_open(s_audio.codec, &format) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        goto fail;
    }
    codec_open = true;
    if (esp_codec_dev_set_out_vol(s_audio.codec, AUDIO_DEFAULT_VOLUME) !=
            ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(s_audio.codec, false) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec volume/unmute failed");
        goto fail;
    }
    s_audio.applied_volume = AUDIO_DEFAULT_VOLUME;
    s_audio.bus = bus;
    s_audio.free_chunks = xSemaphoreCreateCounting(AUDIO_RING_CHUNKS,
                                                    AUDIO_RING_CHUNKS);
    s_audio.ready_chunks = xSemaphoreCreateCounting(AUDIO_RING_CHUNKS, 0);
    error = ESP_ERR_NO_MEM;
    if (s_audio.free_chunks == NULL || s_audio.ready_chunks == NULL) {
        ESP_LOGE(TAG, "chunk semaphores failed");
        goto fail;
    }
    if (xTaskCreatePinnedToCore(dsp_task, "audio_dsp", AUDIO_DSP_STACK, NULL,
                                 AUDIO_DSP_PRIORITY, &s_audio.dsp_task,
                                 AUDIO_TASK_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(writer_task, "audio_i2s", AUDIO_WRITER_STACK, NULL,
                                 AUDIO_WRITER_PRIORITY, &s_audio.writer_task,
                                 AUDIO_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "audio task create failed");
        goto fail;
    }
    s_audio.ready = true;
    (void)xTaskNotifyGive(s_audio.dsp_task);
    (void)xTaskNotifyGive(s_audio.writer_task);
    ESP_LOGI(TAG, "ES8311 ready: %d Hz stereo, %d frame chunks, %d chunk ring",
             APP_AUDIO_SAMPLE_RATE, AUDIO_CHUNK_FRAMES, AUDIO_RING_CHUNKS);
    return ESP_OK;

fail:
    rollback_init(codec_open);
    return error;
}

esp_err_t app_audio_init(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio.ready) {
        return app_audio_bus_tracker(s_audio.bus) == engine
                   ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    app_audio_bus_t *bus = app_audio_bus_create(engine, NULL);
    if (bus == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t error = app_audio_init_bus(bus);
    if (error != ESP_OK) {
        app_audio_bus_destroy(bus);
        return error;
    }
    s_audio.owned_bus = bus;
    return ESP_OK;
}

void app_audio_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    atomic_store(&s_audio.volume_req, percent);
}

void app_audio_play(void)
{
    tracker_engine_t *engine = app_audio_bus_tracker(s_audio.bus);
    if (engine != NULL) {
        tracker_engine_play(engine);
    }
}

void app_audio_pause(void)
{
    tracker_engine_t *engine = app_audio_bus_tracker(s_audio.bus);
    if (engine != NULL) {
        tracker_engine_pause(engine);
    }
}
