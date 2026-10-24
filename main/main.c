// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <string.h>

#include "app_audio.h"
#include "app_display.h"
#include "app_tracker.h"
#include "bsp/esp_mosaico.h"
#include "esp_gsp_debug.h"
#include "esp_iris.h"
#include "esp_iris_service_profiles.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "tracker_engine.h"
#include "ui_bundle.h"

static const char *TAG = "aryx";

extern const uint8_t aryx_s3m_start[] asm("_binary_aryx_s3m_start");
extern const uint8_t aryx_s3m_end[] asm("_binary_aryx_s3m_end");

static struct {
    esp_gsp_handle_t ui;
    tracker_engine_t *engine;
    app_tracker_t *controller;
} application;

/* iris rpc 0x1001/1: 12-byte pointer samples forwarded into GSP. Useful for
 * headless UI/audio checks and for comparing against the CST9220 reports. */
static esp_err_t page_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    if (request->payload_size != 1 || response_capacity < 1 ||
        response_size == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    app_tracker_set_page((app_tracker_t *)user_ctx, request->payload[0]);
    response[0] = request->payload[0];
    *response_size = 1;
    return ESP_OK;
}

static esp_err_t pointer_rpc(const esp_iris_rpc_request_t *request,
                             uint8_t *response, size_t response_capacity,
                             size_t *response_size, void *user_ctx)
{
    if (request->payload_size != ESP_IRIS_POINTER_MESSAGE_SIZE ||
        response == NULL || response_capacity < ESP_IRIS_POINTER_MESSAGE_SIZE ||
        response_size == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *payload = request->payload;
    const uint8_t phase = payload[0];
    const int16_t x = (int16_t)((uint16_t)payload[2] | ((uint16_t)payload[3] << 8));
    const int16_t y = (int16_t)((uint16_t)payload[4] | ((uint16_t)payload[5] << 8));
    (void)esp_gsp_inject_touch((esp_gsp_handle_t)user_ctx, x, y, phase != 2);
    memcpy(response, payload, ESP_IRIS_POINTER_MESSAGE_SIZE);
    *response_size = ESP_IRIS_POINTER_MESSAGE_SIZE;
    return ESP_OK;
}

/* iris rpc 0x1001/2: scripted drag <BBhhhh> (step_ms, steps, x1, y1, x2, y2).
 * Injects one press, "steps" interpolated moves and one release so gesture
 * routing (page swipe vs. scroll region vs. widget) can be checked headless. */
static esp_err_t drag_rpc(const esp_iris_rpc_request_t *request,
                          uint8_t *response, size_t response_capacity,
                          size_t *response_size, void *user_ctx)
{
    if (request->payload_size != 10 || response == NULL ||
        response_capacity < 10 || response_size == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *payload = request->payload;
    const uint8_t step_ms = payload[0];
    const uint8_t steps = payload[1];
    const int16_t x1 = (int16_t)((uint16_t)payload[2] | ((uint16_t)payload[3] << 8));
    const int16_t y1 = (int16_t)((uint16_t)payload[4] | ((uint16_t)payload[5] << 8));
    const int16_t x2 = (int16_t)((uint16_t)payload[6] | ((uint16_t)payload[7] << 8));
    const int16_t y2 = (int16_t)((uint16_t)payload[8] | ((uint16_t)payload[9] << 8));
    esp_gsp_handle_t ui = (esp_gsp_handle_t)user_ctx;
    (void)esp_gsp_inject_touch(ui, x1, y1, true);
    for (uint32_t step = 1; step <= steps; ++step) {
        if (step_ms != 0) {
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
        const int16_t x = (int16_t)(x1 + (int32_t)(x2 - x1) * (int32_t)step /
                                            (int32_t)steps);
        const int16_t y = (int16_t)(y1 + (int32_t)(y2 - y1) * (int32_t)step /
                                            (int32_t)steps);
        (void)esp_gsp_inject_touch(ui, x, y, true);
    }
    (void)esp_gsp_inject_touch(ui, x2, y2, false);
    memcpy(response, payload, 10);
    *response_size = 10;
    return ESP_OK;
}

void app_main(void)
{
    /* The CST9220 emits a continuous malformed-report warning stream and the
     * WiFi PHY prints its rate table; both would flush the retained log ring
     * that holds the evidence for a real failure. Keep only their errors. */
    esp_log_level_set("cst9220", ESP_LOG_ERROR);
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    /* Never inherit a latched haptic motor from another firmware. */
    (void)bsp_motor_init();
    (void)bsp_motor_set(false);
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Recovery must stay reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    const size_t module_size = (size_t)(aryx_s3m_end - aryx_s3m_start);
    application.engine = tracker_engine_create(aryx_s3m_start, module_size);
    if (application.engine == NULL) {
        ESP_LOGE(TAG, "S3M engine unavailable; Recovery RPC remains active");
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_audio_init(application.engine));

    esp_gsp_config_t config;
    const esp_err_t bundle_err = ui_bundle_open(&config);
    if (bundle_err != ESP_OK) {
        ESP_LOGE(TAG, "UI bundle unavailable (0x%x); audio stays idle", bundle_err);
        return;
    }

    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x)", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));

    ESP_ERROR_CHECK(app_tracker_start(application.ui, application.engine,
                                      &application.controller));
    ESP_ERROR_CHECK(esp_iris_rpc_register(ESP_IRIS_POINTER_SERVICE_ID,
                                          ESP_IRIS_POINTER_METHOD_ID,
                                          pointer_rpc, application.ui));
    ESP_ERROR_CHECK(esp_iris_rpc_register(ESP_IRIS_POINTER_SERVICE_ID, 2,
                                          drag_rpc, application.ui));
    ESP_ERROR_CHECK(esp_iris_rpc_register(0x1002, 1, page_rpc,
                                          application.controller));
    ESP_LOGI(TAG, "ARYX tracker ready: %u bytes S3M", (unsigned)module_size);
}
