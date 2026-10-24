// SPDX-License-Identifier: Apache-2.0
/* OP-Ay1 composition root: boot Recovery support, build the portable tracker
 * engine, start audio and display, then hand the GSP scene to app_tracker. */

#include <stddef.h>
#include <stdint.h>

#include "app_audio.h"
#include "app_display.h"
#include "app_tracker.h"
#include "bsp/esp_mosaico.h"
#include "esp_gsp.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "tracker_engine.h"
#include "ui_bundle.h"

static const char *TAG = "OP-Ay1";

extern const uint8_t aryx_s3m_start[] asm("_binary_aryx_s3m_start");
extern const uint8_t aryx_s3m_end[] asm("_binary_aryx_s3m_end");

static struct {
    esp_gsp_handle_t ui;
    tracker_engine_t *engine;
    app_tracker_t *controller;
} application;

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
    ESP_LOGI(TAG, "OP-Ay1 tracker ready: %u bytes S3M", (unsigned)module_size);
}
