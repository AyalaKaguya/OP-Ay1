// SPDX-License-Identifier: Apache-2.0
/*
 * Two-stage playback pipeline:
 *
 *   DSP task (core 1, priority 7)  ->  chunk ring  ->  I2S writer task (core 1)
 *
 * The DSP task only synthesises tracker samples into a small ring of chunks;
 * the writer task owns the blocking esp_codec_dev_write() call, whose buffer is
 * drained by the I2S DMA engine. Splitting the two keeps synthesis jitter away
 * from DMA refills and gives the renderer room to run ahead.
 */

#include "app_audio.h"

#include <stdatomic.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AUDIO_SAMPLE_RATE 48000
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

static struct {
    esp_codec_dev_handle_t codec;
    tracker_engine_t *engine;
    TaskHandle_t dsp_task;
    TaskHandle_t writer_task;
    SemaphoreHandle_t free_chunks;
    SemaphoreHandle_t ready_chunks;
    audio_chunk_t ring[AUDIO_RING_CHUNKS];
    atomic_int volume_req;
    atomic_bool running;
    int applied_volume;
    /* diagnostics */
    int64_t dsp_max_us;
    int64_t write_max_us;
    int64_t dsp_total_us;
    int64_t write_total_us;
    int64_t stats_start_us;
    unsigned dsp_chunks;
} s_audio = {.volume_req = AUDIO_DEFAULT_VOLUME, .applied_volume = -1};

static void apply_volume(void)
{
    const int volume = atomic_load(&s_audio.volume_req);
    if (volume == s_audio.applied_volume) {
        return;
    }
    if (esp_codec_dev_set_out_vol(s_audio.codec, volume) == ESP_CODEC_DEV_OK) {
        s_audio.applied_volume = volume;
    }
}

static void dsp_task(void *argument)
{
    (void)argument;
    int head = 0;
    while (atomic_load(&s_audio.running)) {
        if (xSemaphoreTake(s_audio.free_chunks, portMAX_DELAY) != pdTRUE) {
            break;
        }
        const int64_t start = esp_timer_get_time();
        tracker_engine_render(s_audio.engine, s_audio.ring[head],
                              AUDIO_CHUNK_FRAMES);
        const int64_t elapsed = esp_timer_get_time() - start;
        s_audio.dsp_total_us += elapsed;
        if (elapsed > s_audio.dsp_max_us) {
            s_audio.dsp_max_us = elapsed;
        }
        ++s_audio.dsp_chunks;
        head = (head + 1) % AUDIO_RING_CHUNKS;
        xSemaphoreGive(s_audio.ready_chunks);
    }
    vTaskDelete(NULL);
}

static void writer_task(void *argument)
{
    (void)argument;
    int tail = 0;
    while (atomic_load(&s_audio.running)) {
        if (xSemaphoreTake(s_audio.ready_chunks, portMAX_DELAY) != pdTRUE) {
            break;
        }
        apply_volume();
        const int64_t start = esp_timer_get_time();
        const int result = esp_codec_dev_write(s_audio.codec, s_audio.ring[tail],
                                               sizeof(audio_chunk_t));
        const int64_t elapsed = esp_timer_get_time() - start;
        s_audio.write_total_us += elapsed;
        if (elapsed > s_audio.write_max_us) {
            s_audio.write_max_us = elapsed;
        }
        tail = (tail + 1) % AUDIO_RING_CHUNKS;
        xSemaphoreGive(s_audio.free_chunks);
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec write failed: %d", result);
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        const int64_t now = esp_timer_get_time();
        if (now - s_audio.stats_start_us >= AUDIO_STATS_PERIOD_US &&
            s_audio.dsp_chunks > 0) {
            ESP_LOGI(TAG, "dsp avg=%lldus max=%lldus | write avg=%lldus max=%lldus",
                     (long long)(s_audio.dsp_total_us / s_audio.dsp_chunks),
                     (long long)s_audio.dsp_max_us,
                     (long long)(s_audio.write_total_us / s_audio.dsp_chunks),
                     (long long)s_audio.write_max_us);
            s_audio.stats_start_us = now;
            s_audio.dsp_max_us = 0;
            s_audio.write_max_us = 0;
            s_audio.dsp_total_us = 0;
            s_audio.write_total_us = 0;
            s_audio.dsp_chunks = 0;
        }
    }
    vTaskDelete(NULL);
}

esp_err_t app_audio_init(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio.codec != NULL) {
        return ESP_OK;
    }

    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(speaker != NULL, ESP_FAIL, TAG, "speaker init failed");

    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = AUDIO_CHANNELS,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(speaker, &format) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec open failed");

    s_audio.codec = speaker;
    s_audio.engine = engine;
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(speaker, AUDIO_DEFAULT_VOLUME) ==
                            ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "volume failed");
    s_audio.applied_volume = AUDIO_DEFAULT_VOLUME;
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_mute(speaker, false) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "unmute failed");

    s_audio.free_chunks = xSemaphoreCreateCounting(AUDIO_RING_CHUNKS,
                                                   AUDIO_RING_CHUNKS);
    s_audio.ready_chunks = xSemaphoreCreateCounting(AUDIO_RING_CHUNKS, 0);
    ESP_RETURN_ON_FALSE(s_audio.free_chunks != NULL && s_audio.ready_chunks != NULL,
                        ESP_ERR_NO_MEM, TAG, "chunk semaphores failed");

    atomic_store(&s_audio.running, true);
    s_audio.stats_start_us = esp_timer_get_time();
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(
                            dsp_task, "tracker_dsp", AUDIO_DSP_STACK, NULL,
                            AUDIO_DSP_PRIORITY, &s_audio.dsp_task,
                            AUDIO_TASK_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "dsp task create failed");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(
                            writer_task, "tracker_i2s", AUDIO_WRITER_STACK, NULL,
                            AUDIO_WRITER_PRIORITY, &s_audio.writer_task,
                            AUDIO_TASK_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "writer task create failed");

    ESP_LOGI(TAG, "ES8311 ready: %d Hz stereo, %d frame chunks, %d chunk ring",
             AUDIO_SAMPLE_RATE, AUDIO_CHUNK_FRAMES, AUDIO_RING_CHUNKS);
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
    if (s_audio.engine != NULL) {
        tracker_engine_play(s_audio.engine);
    }
}

void app_audio_pause(void)
{
    if (s_audio.engine != NULL) {
        tracker_engine_pause(s_audio.engine);
    }
}
