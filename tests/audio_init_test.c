// SPDX-License-Identifier: Apache-2.0
/* Host fault injection for the real device adapter. Task stubs retain entry
 * points but never run them, so no codec or hardware connection occurs. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_audio.h"
#include "bsp/esp_mosaico.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct app_audio_bus {
    tracker_engine_t *tracker;
};
static struct app_audio_bus test_bus;
static int fail_at;
static int stage;
static int semaphores;
static int tasks;
static int buses;
static int notifications;
static int opened;
static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d stage %d: %s\n", __LINE__, fail_at, #condition); \
        ++failures; \
    } \
} while (0)

static bool inject_failure(void)
{
    return ++stage == fail_at;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    return inject_failure() ? NULL : (void *)&test_bus;
}

int esp_codec_dev_open(esp_codec_dev_handle_t handle,
                       const esp_codec_dev_sample_info_t *format)
{
    CHECK(handle != NULL && format->sample_rate == 48000 && format->channel == 2);
    if (inject_failure()) {
        return -1;
    }
    ++opened;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_close(esp_codec_dev_handle_t handle)
{
    CHECK(handle != NULL && opened == 1);
    --opened;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t handle, int volume)
{
    CHECK(handle != NULL && volume == 100);
    return inject_failure() ? -1 : ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t handle, bool muted)
{
    CHECK(handle != NULL);
    return !muted && inject_failure() ? -1 : ESP_CODEC_DEV_OK;
}

int esp_codec_dev_write(esp_codec_dev_handle_t handle, void *data, int size)
{
    (void)handle;
    (void)data;
    (void)size;
    CHECK(false); /* Gated task entry points must not run during init. */
    return ESP_CODEC_DEV_OK;
}

SemaphoreHandle_t xSemaphoreCreateCounting(unsigned maximum, unsigned initial)
{
    CHECK(maximum == 4 && initial <= maximum);
    if (inject_failure()) {
        return NULL;
    }
    void *handle = malloc(1);
    if (handle != NULL) {
        ++semaphores;
    }
    return handle;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    CHECK(semaphore != NULL && semaphores > 0);
    --semaphores;
    free(semaphore);
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout)
{
    (void)semaphore;
    (void)timeout;
    CHECK(false);
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    CHECK(false);
    return pdTRUE;
}

int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                            unsigned stack, void *argument, unsigned priority,
                            TaskHandle_t *handle, int core)
{
    (void)argument;
    CHECK(entry != NULL && name != NULL && stack >= 3072 && priority >= 6);
    CHECK(core == tskNO_AFFINITY && notifications == 0);
    if (inject_failure()) {
        return 0;
    }
    *handle = malloc(1);
    if (*handle == NULL) {
        return 0;
    }
    ++tasks;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    CHECK(task != NULL && tasks > 0 && notifications == 0);
    --tasks;
    free(task);
}

unsigned ulTaskNotifyTake(int clear, unsigned timeout)
{
    (void)clear;
    (void)timeout;
    CHECK(false);
    return 1;
}

int xTaskNotifyGive(TaskHandle_t task)
{
    CHECK(task != NULL && tasks == 2 && semaphores == 2 && opened == 1);
    ++notifications;
    return pdPASS;
}

void vTaskDelay(unsigned ticks)
{
    (void)ticks;
}
int64_t esp_timer_get_time(void)
{
    return 0;
}

app_audio_bus_t *app_audio_bus_create(tracker_engine_t *tracker, app_synth_t *synth)
{
    (void)synth;
    app_audio_bus_t *bus = malloc(sizeof(*bus));
    if (bus != NULL) {
        bus->tracker = tracker;
        ++buses;
    }
    return bus;
}

void app_audio_bus_destroy(app_audio_bus_t *bus)
{
    CHECK(bus != NULL && buses > 0);
    --buses;
    free(bus);
}

tracker_engine_t *app_audio_bus_tracker(app_audio_bus_t *bus)
{
    return bus != NULL ? bus->tracker : NULL;
}

void app_audio_bus_render(app_audio_bus_t *bus, int16_t *output, int frames)
{
    (void)bus;
    (void)output;
    (void)frames;
    CHECK(false);
}

void tracker_engine_play(tracker_engine_t *tracker)
{
    (void)tracker;
}
void tracker_engine_pause(tracker_engine_t *tracker)
{
    (void)tracker;
}

int main(void)
{
    CHECK(app_audio_init_bus(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(app_audio_init(NULL) == ESP_ERR_INVALID_ARG);
    for (fail_at = 1; fail_at <= 8; ++fail_at) {
        stage = 0;
        CHECK(app_audio_init_bus(&test_bus) != ESP_OK);
        CHECK(semaphores == 0 && tasks == 0 && opened == 0 && notifications == 0);
        stage = 0;
        CHECK(app_audio_init((tracker_engine_t *)&test_bus) != ESP_OK);
        CHECK(semaphores == 0 && tasks == 0 && opened == 0 && buses == 0);
    }
    fail_at = 0;
    stage = 0;
    CHECK(app_audio_init_bus(&test_bus) == ESP_OK);
    CHECK(notifications == 2 && semaphores == 2 && tasks == 2 && opened == 1);
    CHECK(app_audio_init_bus(&test_bus) == ESP_OK);
    struct app_audio_bus different_bus;
    CHECK(app_audio_init_bus(&different_bus) == ESP_ERR_INVALID_STATE);
    printf("audio init fault injection: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
