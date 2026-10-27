// SPDX-License-Identifier: Apache-2.0
/* OP-Ay1 composition root: boot Recovery support, build the portable tracker
 * engine, start audio and display, then hand the GSP scene to app_tracker. */

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "app_audio.h"
#include "app_display.h"
#include "app_synth.h"
#include "app_settings_device.h"
#include "app_note_router.h"
#include "app_usb_midi.h"
#include "app_project.h"
#include "app_sampler.h"
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
    app_synth_t *synth;
    app_audio_bus_t *bus;
    app_tracker_t *controller;
    app_settings_t *settings;
    app_note_router_t *router;
    app_usb_midi_t *usb;
    app_project_t *project;
    app_sampler_t *sampler;
    _Atomic(app_project_t *) inbox_project;
} application;

static esp_err_t inbox_status(void *context)
{
    (void)context;
    /* Only the application inbox is exposed; project generations and other
     * NAND users remain outside this externally writable volume. */
    app_project_t *project = atomic_load_explicit(&application.inbox_project, memory_order_acquire);
    return project && app_project_inbox_ready(project)
           ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* Only used before audio tasks have successfully borrowed these sources. */
static void release_audio_sources(void)
{
    app_audio_bus_destroy(application.bus);
    app_sampler_destroy(application.sampler);
    app_synth_destroy(application.synth);
    tracker_engine_destroy(application.engine);
    application.bus = NULL;
    application.synth = NULL;
    application.engine = NULL;
    application.sampler = NULL;
}

/* Once the mirror attaches, GSP and its display remain process-lifetime
 * objects. On startup failure stop only the new owners that never reached a
 * controller; audio has already borrowed its sources and stays idle. */
static void release_unstarted_controls(void)
{
    if (application.controller) {
        return;
    }
    if (application.project) {
        /* Iris may already hold an admitted inbox stream. There is no product
         * runtime restart API: retain its worker and storage until reboot. */
        ESP_LOGE(TAG, "Project/inbox service retained for recovery after UI startup failure");
        return;
    }
    if (!app_usb_midi_shutdown(&application.usb, 2000)) {
        ESP_LOGE(TAG, "USB cleanup pending; retaining its borrowed router");
        return;
    }
    app_note_router_destroy(application.router);
    application.router = NULL;
    if (application.settings) {
        const esp_err_t result = app_settings_stop(&application.settings, 2000);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Settings cleanup pending (0x%x)", result);
        }
    }
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
    const esp_iris_file_volume_config_t inbox = {
        .id = "opay_inbox", .base_path = "/nand/apps/op-ay1/inbox",
        .capabilities = ESP_IRIS_FILE_VOLUME_READ | ESP_IRIS_FILE_VOLUME_LIST |
        ESP_IRIS_FILE_VOLUME_WRITE | ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_file_volume_register_deferred(&inbox, inbox_status, NULL));
    /* Recovery must stay reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    const size_t module_size = (size_t)(aryx_s3m_end - aryx_s3m_start);
    application.engine = tracker_engine_create(aryx_s3m_start, module_size);
    if (application.engine == NULL) {
        ESP_LOGE(TAG, "S3M engine unavailable; Recovery RPC remains active");
        return;
    }
    application.synth = app_synth_create(APP_AUDIO_SAMPLE_RATE);
    application.sampler = app_sampler_create();
    application.bus = app_audio_bus_create(application.engine, application.synth);
    if (!application.synth || !application.sampler || !application.bus) {
        ESP_LOGE(TAG, "Audio sources unavailable; Vibe Mode RPC remains active");
        release_audio_sources();
        return;
    }
    if (!app_sampler_bind_engine(application.sampler, application.engine)) {
        ESP_LOGE(TAG, "Sampler could not bind the stable tracker source");
        release_audio_sources();
        return;
    }
    if (!app_audio_bus_set_sampler(application.bus, application.sampler)) {
        ESP_LOGE(TAG, "Sampler output could not join the audio bus");
        release_audio_sources();
        return;
    }
    const esp_err_t audio_error = app_audio_init_bus(application.bus);
    if (audio_error != ESP_OK) {
        ESP_LOGE(TAG, "Audio output unavailable (0x%x); Vibe Mode RPC remains active", audio_error);
        release_audio_sources();
        return;
    }

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
    const esp_err_t settings_error = app_settings_start(application.ui, display.presenter, &application.settings);
    if (settings_error != ESP_OK) {
        ESP_LOGE(TAG, "Settings unavailable (0x%x); Vibe Mode RPC remains active", settings_error);
        return;
    }
    application.router = app_note_router_create(application.synth);
    if (!application.router || !app_usb_midi_create(application.router, &application.usb)) {
        ESP_LOGE(TAG, "MIDI services unavailable; Vibe Mode RPC remains active");
        release_unstarted_controls();
        return;
    }

    const app_project_config_t project_config = {
        .tracker = application.engine, .synth = application.synth,
        .bus = application.bus, .sampler = application.sampler,
        .router = application.router,
        .example_data = aryx_s3m_start, .example_size = module_size,
        .root_path = NULL,
    };
    const app_project_error_t project_error = app_project_create(&project_config, &application.project);
    if (project_error != APP_PROJECT_OK) {
        ESP_LOGE(TAG, "Project service unavailable: %s", app_project_error_name(project_error));
        release_unstarted_controls();
        return;
    }
    atomic_store_explicit(&application.inbox_project, application.project, memory_order_release);

    const esp_err_t controller_error = app_tracker_start_workstation(application.ui, application.engine,
                                                                     application.synth, application.bus, application.router,
                                                                     application.usb, application.settings,
                                                                     application.project, application.sampler,
                                                                     &application.controller);
    if (controller_error != ESP_OK) {
        ESP_LOGE(TAG, "Workstation unavailable (0x%x); Vibe Mode RPC remains active", controller_error);
        release_unstarted_controls();
        return;
    }
    ESP_LOGI(TAG, "OP-Ay1 ready: %u bytes S3M + 4-voice synth", (unsigned)module_size);
}
