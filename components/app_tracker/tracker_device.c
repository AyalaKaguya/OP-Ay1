// SPDX-License-Identifier: Apache-2.0
/* Device services are outside the portable GSP rendering and input owner. */
#include "app_tracker.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_audio.h"
#include "app_settings_device.h"
#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_iris.h"
#include "esp_iris_service_profiles.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_button.h"

#define WORKSTATION_STATUS_SERVICE 0x2101U
#define WORKSTATION_STATUS_METHOD 1U
#define WORKSTATION_AUDITION_METHOD 2U
#define WORKSTATION_SETTINGS_METHOD 3U
#define WORKSTATION_WORKSPACE_METHOD 4U
#define POINTER_QUEUE_CAPACITY 32

typedef struct {
    int16_t x;
    int16_t y;
    bool pressed;
} tracker_pointer_t;

typedef struct {
    uint8_t notes[4];
    uint16_t velocity;
    uint16_t duration_ms;
} tracker_audition_t;

typedef struct {
    esp_timer_handle_t watchdog;
    button_handle_t buttons[BSP_BUTTON_NUM];
    button_handle_t boot_button;
    int button_count;
    atomic_uint ticks;
    atomic_uint transport_requests;
    atomic_bool back_requested;
    atomic_bool cancel_input;
    atomic_bool active;
    atomic_bool snapshot_ready;
    app_tracker_t *controller;
    QueueHandle_t input_queue;
    QueueHandle_t audition_queue;
    SemaphoreHandle_t snapshot_lock;
    app_tracker_snapshot_t snapshot;
    app_synth_t *synth;
    app_settings_t *settings;
    app_tracker_settings_snapshot_t settings_snapshot;
    uint64_t audition_deadline_ms;
    unsigned previous_ticks;
    unsigned stalled_seconds;
} tracker_device_t;

static const char *TAG = "app_tracker";
/* Device services are initialized once by app_main and retained until reboot.
 * Button callbacks only post into this process-lifetime context. */
static bool device_started;

static void *allocate_frame(void *context, size_t bytes)
{
    (void)context;
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void free_frame(void *context, void *frame)
{
    (void)context;
    free(frame);
}

static uint64_t time_ms(void *context)
{
    (void)context;
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static void set_volume(void *context, int percent)
{
    (void)context;
    app_audio_set_volume(percent);
}

static void heartbeat(void *context)
{
    tracker_device_t *device = context;
    if (!atomic_load_explicit(&device->active, memory_order_acquire)) {
        return;
    }
    atomic_fetch_add_explicit(&device->ticks, 1U, memory_order_relaxed);
    const unsigned requests = atomic_exchange_explicit(&device->transport_requests,
                                                       0, memory_order_acquire);
    if (requests) {
        app_tracker_request_transport(device->controller, (requests & 2U) != 0);
    }
    if (atomic_exchange_explicit(&device->back_requested, false, memory_order_acquire)) {
        app_tracker_request_back(device->controller);
    }
    tracker_pointer_t pointer;
    for (unsigned i = 0; i < POINTER_QUEUE_CAPACITY &&
            xQueueReceive(device->input_queue, &pointer, 0) == pdTRUE; ++i) {
        app_tracker_feed_pointer(device->controller, pointer.x, pointer.y, pointer.pressed);
    }
    if (atomic_exchange_explicit(&device->cancel_input, false, memory_order_acquire)) {
        app_tracker_cancel_pointer(device->controller);
    }
    tracker_audition_t audition;
    if (xQueueReceive(device->audition_queue, &audition, 0) == pdTRUE) {
        app_synth_all_notes_off(device->synth);
        bool accepted = true;
        for (unsigned note = 0; note < 4; ++note) {
            accepted = app_synth_note_on(device->synth, audition.notes[note],
                                         audition.velocity / 127.0f) && accepted;
        }
        device->audition_deadline_ms = time_ms(NULL) + audition.duration_ms;
        if (!accepted) {
            app_synth_panic(device->synth);
            device->audition_deadline_ms = 0;
        }
    }
    if (device->audition_deadline_ms && time_ms(NULL) >= device->audition_deadline_ms) {
        app_synth_all_notes_off(device->synth);
        device->audition_deadline_ms = 0;
    }
    if (xSemaphoreTake(device->snapshot_lock, 0) == pdTRUE) {
        (void)app_tracker_read(device->controller, &device->snapshot);
        (void)app_tracker_read_settings(device->controller, &device->settings_snapshot);
        (void)xSemaphoreGive(device->snapshot_lock);
        atomic_store_explicit(&device->snapshot_ready, true, memory_order_release);
    }
}

static bool read_settings(void *context, app_settings_snapshot_t *snapshot)
{
    tracker_device_t *device = context;
    return device->settings && app_settings_read_snapshot(device->settings, snapshot) == ESP_OK;
}

static bool request_settings(void *context, int brightness, int rotation, bool locked)
{
    tracker_device_t *device = context;
    if (!device->settings) {
        return false;
    }
    return app_settings_request_brightness(device->settings, brightness) == ESP_OK &&
           app_settings_request_rotation(device->settings, rotation) == ESP_OK &&
           app_settings_request_lock(device->settings, locked) == ESP_OK;
}

static bool request_idle(void *context, uint32_t dim_after_s, uint32_t off_after_s)
{
    tracker_device_t *device = context;
    return device->settings && app_settings_request_idle(device->settings, dim_after_s, off_after_s) == ESP_OK;
}

static bool record_activity(void *context)
{
    tracker_device_t *device = context;
    return device->settings && app_settings_record_activity(device->settings);
}

static void interaction_busy(void *context, bool busy)
{
    tracker_device_t *device = context;
    if (device->settings) {
        app_settings_set_interaction_busy(device->settings, busy);
    }
}

static esp_err_t settings_rpc(const esp_iris_rpc_request_t *request,
                              uint8_t *response, size_t capacity,
                              size_t *response_size, void *context)
{
    tracker_device_t *device = context;
    if (!request || !response || !response_size || request->payload_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&device->snapshot_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(device->snapshot_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Read fields under the lock directly; do not put two large service
     * snapshots on the small RPC task stack. */
    const app_tracker_settings_snapshot_t *s = &device->settings_snapshot;
    const int count = snprintf((char *)response, capacity,
                               "{\"open\":%d,\"confirm\":%d,\"timeouts\":%d,\"brightness\":%d,\"effective\":%d,\"dim_s\":%lu,\"off_s\":%lu,\"dimmed\":%d,\"screen_off\":%d,\"rotation\":%d,\"locked\":%d,"
                               "\"battery_valid\":%d,\"battery_stale\":%d,\"battery_percent\":%d,\"voltage_mv\":%d,"
                               "\"current_ma\":%d,\"charge\":%d,\"imu\":%d,\"display_error\":%d,\"busy\":%d,"
                               "\"usb_mode\":%d,\"usb_state\":%d,\"connected\":%d,\"rx\":%lu,\"tx\":%lu,\"usb_error\":%ld}",
                               s->menu_open, s->confirmation_open, s->timeouts_open, s->display.brightness,
                               s->display.effective_brightness, (unsigned long)s->display.dim_after_s,
                               (unsigned long)s->display.off_after_s, s->display.dimmed, s->display.screen_off,
                               s->display.rotation,
                               s->display.rotation_locked, s->display.battery_valid, s->display.battery_stale,
                               s->display.battery_percent, s->display.voltage_mv, s->display.current_ma, s->display.charge,
                               s->display.imu_available, s->display.last_error, s->display.busy, s->usb.actual_mode,
                               s->usb.state, s->usb.connected, (unsigned long)s->usb.received_packets,
                               (unsigned long)s->usb.sent_packets, (long)s->usb.last_error);
    (void)xSemaphoreGive(device->snapshot_lock);
    if (count < 0 || (size_t)count >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_size = (size_t)count;
    return ESP_OK;
}

static esp_err_t pointer_rpc(const esp_iris_rpc_request_t *request,
                             uint8_t *response, size_t capacity,
                             size_t *response_size, void *context)
{
    tracker_device_t *device = context;
    if (!request || !response || !response_size ||
            request->payload_size != ESP_IRIS_POINTER_MESSAGE_SIZE ||
            capacity < ESP_IRIS_POINTER_MESSAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!atomic_load_explicit(&device->active, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *data = request->payload;
    const int16_t x = (int16_t)((uint16_t)data[2] | (uint16_t)data[3] << 8);
    const int16_t y = (int16_t)((uint16_t)data[4] | (uint16_t)data[5] << 8);
    if (data[0] > 2 || x < 0 || y < 0 || x >= 480 || y >= 480) {
        return ESP_ERR_INVALID_ARG;
    }
    const tracker_pointer_t pointer = {.x = x, .y = y, .pressed = data[0] != 2};
    if (xQueueSend(device->input_queue, &pointer, 0) != pdTRUE) {
        /* Callers may abort a gesture on any rejected move without an up. */
        atomic_store_explicit(&device->cancel_input, true, memory_order_release);
        if (pointer.pressed) {
            return ESP_ERR_TIMEOUT;
        }
        /* A release must survive a full input queue. Drain older moves first,
         * then cancel on the UI thread so they cannot resurrect the key. */
    }
    memcpy(response, data, ESP_IRIS_POINTER_MESSAGE_SIZE);
    *response_size = ESP_IRIS_POINTER_MESSAGE_SIZE;
    return ESP_OK;
}

static esp_err_t status_rpc(const esp_iris_rpc_request_t *request,
                            uint8_t *response, size_t capacity,
                            size_t *response_size, void *context)
{
    tracker_device_t *device = context;
    if (!request || !response || !response_size || request->payload_size != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&device->active, memory_order_acquire) ||
            !atomic_load_explicit(&device->snapshot_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(device->snapshot_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const app_tracker_snapshot_t *s = &device->snapshot;
    const int count = snprintf((char *)response, capacity,
                               "{\"page\":%d,\"volume\":%d,\"scroll\":%d,\"note\":%d,\"tab\":%d,"
                               "\"preset\":%d,\"playing\":%d,\"launched\":%d,\"pending\":%d,"
                               "\"stop\":%d,\"voices\":%u,\"peak\":%.6f,\"frames\":%llu,"
                               "\"gain\":[%d,%d,%d,%d],\"mute\":[%d,%d,%d,%d],"
                               "\"solo\":[%d,%d,%d,%d],\"error\":%d}",
                               s->page, s->volume, s->mix_scroll, s->held_note, s->synth_tab, s->preset,
                               s->playing, s->launched_pattern, s->pending_pattern, s->stop_pending,
                               (unsigned)s->active_voices, (double)s->synth_peak, (unsigned long long)s->rendered_frames,
                               s->gain_steps[0], s->gain_steps[1], s->gain_steps[2], s->gain_steps[3],
                               s->muted[0], s->muted[1], s->muted[2], s->muted[3],
                               s->soloed[0], s->soloed[1], s->soloed[2], s->soloed[3], s->last_error);
    (void)xSemaphoreGive(device->snapshot_lock);
    if (count < 0 || (size_t)count >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_size = (size_t)count;
    return ESP_OK;
}

static esp_err_t workspace_rpc(const esp_iris_rpc_request_t *request,
                               uint8_t *response, size_t capacity,
                               size_t *response_size, void *context)
{
    tracker_device_t *device = context;
    if (!request || !response || !response_size || request->payload_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&device->active, memory_order_acquire) ||
            !atomic_load_explicit(&device->snapshot_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(device->snapshot_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const app_tracker_snapshot_t *s = &device->snapshot;
    const int count = snprintf((char *)response, capacity,
                               "{\"workspace\":%d,\"home\":%d,\"opened\":%d,\"dirty\":%d,\"busy\":%d,\"generation\":%llu,\"name\":\"",
                               s->workspace_screen, s->home_page, s->project_opened, s->project_dirty,
                               s->project_busy, (unsigned long long)s->project_generation);
    size_t used = count > 0 ? (size_t)count : capacity;
    bool valid = used < capacity;
    /* Encode straight into the bounded response. No large public snapshot or
     * six-times-expanded name buffer is placed on the RPC task stack. */
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; valid && i < sizeof(s->project_name) && s->project_name[i]; ++i) {
        const uint8_t value = (uint8_t)s->project_name[i];
        const size_t bytes = value < 32U ? 6U : value == '"' || value == '\\' ? 2U : 1U;
        if (used + bytes + 3U >= capacity) {
            valid = false;
            break;
        }
        if (value < 32U) {
            response[used++] = '\\'; response[used++] = 'u';
            response[used++] = '0'; response[used++] = '0';
            response[used++] = (uint8_t)hex[value >> 4];
            response[used++] = (uint8_t)hex[value & 15U];
        } else {
            if (bytes == 2U) {
                response[used++] = '\\';
            }
            response[used++] = value;
        }
    }
    (void)xSemaphoreGive(device->snapshot_lock);
    if (!valid || used + 3U > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    response[used++] = '"'; response[used++] = '}'; response[used] = '\0';
    *response_size = used;
    return ESP_OK;
}

/* Bounded diagnostic audition for target DSP acceptance. It uses the normal
 * voice command queue on the UI thread and cannot hold notes indefinitely. */
static esp_err_t audition_rpc(const esp_iris_rpc_request_t *request,
                              uint8_t *response, size_t capacity,
                              size_t *response_size, void *context)
{
    tracker_device_t *device = context;
    if (!request || !response || !response_size || request->payload_size != 8 || capacity < 8) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!atomic_load_explicit(&device->active, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *data = request->payload;
    const tracker_audition_t command = {
        .notes = {data[0], data[1], data[2], data[3]},
        .velocity = (uint16_t)data[4] | (uint16_t)data[5] << 8,
                                                          .duration_ms = (uint16_t)data[6] | (uint16_t)data[7] << 8,
    };
    if (command.notes[0] > 127 || command.notes[1] > 127 ||
            command.notes[2] > 127 || command.notes[3] > 127 ||
            command.velocity < 1 || command.velocity > 127 ||
            command.duration_ms < 100 || command.duration_ms > 10000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(device->audition_queue, &command, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    (void)record_activity(device);
    memcpy(response, data, 8);
    *response_size = 8;
    return ESP_OK;
}

static void stall_check(void *context)
{
    tracker_device_t *device = context;
    if (!atomic_load_explicit(&device->active, memory_order_acquire)) {
        return;
    }
    const unsigned ticks = atomic_load_explicit(&device->ticks, memory_order_relaxed);
    if (ticks != device->previous_ticks) {
        device->previous_ticks = ticks;
        device->stalled_seconds = 0;
        return;
    }
    if (++device->stalled_seconds == 5) {
        ESP_LOGE(TAG, "UI stalled ticks=%u internal_free=%u spiram_free=%u", ticks,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (device->stalled_seconds >= 7) {
        esp_system_abort("UI task stalled");
    }
}

static void ai_short(void *button, void *context)
{
    (void)button;
    tracker_device_t *device = context;
    (void)record_activity(device);
    atomic_fetch_or_explicit(&device->transport_requests, 1U, memory_order_release);
}

static void ai_long(void *button, void *context)
{
    (void)button;
    tracker_device_t *device = context;
    (void)record_activity(device);
    atomic_fetch_or_explicit(&device->transport_requests, 2U, memory_order_release);
}

static void boot_back(void *button, void *context)
{
    (void)button;
    tracker_device_t *device = context;
    if (!atomic_load_explicit(&device->active, memory_order_acquire) || record_activity(device)) {
        return;
    }
    atomic_store_explicit(&device->back_requested, true, memory_order_release);
}

static void create_boot_button(tracker_device_t *device)
{
    const button_config_t common = {0};
    const button_gpio_config_t gpio = {
        .gpio_num = BSP_BUTTON_BOOT_GPIO, .active_level = BSP_BUTTON_ACTIVE_LEVEL,
        .enable_power_save = false, .disable_pull = false,
    };
    const esp_err_t created = iot_button_new_gpio_device(&common, &gpio, &device->boot_button);
    if (created != ESP_OK) {
        ESP_LOGW(TAG, "BOOT back button unavailable (GPIO %d, %d)", BSP_BUTTON_BOOT_GPIO, created);
        return;
    }
    const esp_err_t registered = iot_button_register_cb(device->boot_button, BUTTON_SINGLE_CLICK,
                                                        NULL, boot_back, device);
    if (registered != ESP_OK) {
        const esp_err_t deleted = iot_button_delete(device->boot_button);
        if (deleted == ESP_OK) {
            device->boot_button = NULL;
        }
        ESP_LOGW(TAG, "BOOT back callback unavailable (%d; cleanup %d)", registered, deleted);
    } else {
        ESP_LOGI(TAG, "BOOT back button ready on GPIO %d", BSP_BUTTON_BOOT_GPIO);
    }
}

static void shutdown(void *context)
{
    tracker_device_t *device = context;
    atomic_store_explicit(&device->active, false, memory_order_release);
    if (device->watchdog) {
        const esp_err_t stopped = esp_timer_stop_blocking(device->watchdog, pdMS_TO_TICKS(2000));
        if (stopped == ESP_OK || stopped == ESP_ERR_INVALID_STATE) {
            (void)esp_timer_delete(device->watchdog);
            device->watchdog = NULL;
        }
    }
    /* The button driver has no callback completion fence. Its handles and this
     * inert tombstone remain valid until reboot; callbacks never retain UI. */
}

esp_err_t app_tracker_start_workstation(esp_gsp_handle_t ui, tracker_engine_t *engine,
                                        app_synth_t *synth, app_audio_bus_t *bus,
                                        app_note_router_t *router, app_usb_midi_t *usb,
                                        app_settings_t *settings, app_project_t *project,
                                        app_sampler_t *sampler, app_tracker_t **out_handle)
{
    if (device_started) {
        return ESP_ERR_INVALID_STATE;
    }
    tracker_device_t *device = calloc(1, sizeof(*device));
    if (!device) {
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&device->ticks, 0);
    atomic_init(&device->transport_requests, 0);
    atomic_init(&device->back_requested, false);
    atomic_init(&device->cancel_input, false);
    atomic_init(&device->active, false);
    atomic_init(&device->snapshot_ready, false);
    device->settings = settings;
    device->input_queue = xQueueCreate(POINTER_QUEUE_CAPACITY, sizeof(tracker_pointer_t));
    device->audition_queue = xQueueCreate(1, sizeof(tracker_audition_t));
    device->snapshot_lock = xSemaphoreCreateMutex();
    if (!device->input_queue || !device->snapshot_lock || !device->audition_queue) {
        if (device->input_queue) {
            vQueueDelete(device->input_queue);
        }
        if (device->snapshot_lock) {
            vSemaphoreDelete(device->snapshot_lock);
        }
        if (device->audition_queue) {
            vQueueDelete(device->audition_queue);
        }
        free(device);
        return ESP_ERR_NO_MEM;
    }
    app_tracker_config_t config = {
        .tracker = engine, .synth = synth, .audio_bus = bus,
        .note_router = router, .usb_midi = usb, .project = project, .sampler = sampler,
        .hooks = {
            .context = device, .allocate_frame = allocate_frame,
            .free_frame = free_frame, .time_ms = time_ms, .set_volume = set_volume,
            .heartbeat = heartbeat, .shutdown = shutdown, .read_settings = read_settings,
            .request_settings = request_settings, .interaction_busy = interaction_busy,
            .request_idle = request_idle, .record_activity = record_activity,
        },
    };
    const esp_err_t result = app_tracker_create(ui, &config, out_handle);
    if (result != ESP_OK) {
        if (!out_handle || !*out_handle) {
            vQueueDelete(device->input_queue);
            vQueueDelete(device->audition_queue);
            vSemaphoreDelete(device->snapshot_lock);
            free(device);
        }
        return result;
    }
    device->controller = *out_handle;
    device->synth = synth;
    device_started = true;
    atomic_store_explicit(&device->active, true, memory_order_release);
    const esp_err_t pointer_result = esp_iris_rpc_register(
                                         ESP_IRIS_POINTER_SERVICE_ID, ESP_IRIS_POINTER_METHOD_ID, pointer_rpc, device);
    const esp_err_t status_result = esp_iris_rpc_register(
                                        WORKSTATION_STATUS_SERVICE, WORKSTATION_STATUS_METHOD, status_rpc, device);
    const esp_err_t audition_result = esp_iris_rpc_register(
                                          WORKSTATION_STATUS_SERVICE, WORKSTATION_AUDITION_METHOD, audition_rpc, device);
    const esp_err_t settings_result = esp_iris_rpc_register(
                                          WORKSTATION_STATUS_SERVICE, WORKSTATION_SETTINGS_METHOD, settings_rpc, device);
    const esp_err_t workspace_result = esp_iris_rpc_register(
                                           WORKSTATION_STATUS_SERVICE, WORKSTATION_WORKSPACE_METHOD, workspace_rpc, device);
    if (pointer_result != ESP_OK || status_result != ESP_OK || audition_result != ESP_OK ||
            settings_result != ESP_OK || workspace_result != ESP_OK) {
        ESP_LOGW(TAG, "Workstation RPC registration failed (%d/%d/%d/%d/%d)",
                 pointer_result, status_result, audition_result, settings_result, workspace_result);
    }
    const esp_timer_create_args_t args = {
        .callback = stall_check, .arg = device, .name = "ui_stall",
    };
    if (esp_timer_create(&args, &device->watchdog) == ESP_OK) {
        if (esp_timer_start_periodic(device->watchdog, 1000000) != ESP_OK) {
            ESP_LOGW(TAG, "UI stall timer could not start");
        }
    } else {
        ESP_LOGW(TAG, "UI stall timer unavailable");
    }
    if (bsp_iot_button_create(device->buttons, &device->button_count, BSP_BUTTON_NUM) == ESP_OK &&
            device->button_count > BSP_BUTTON_AI) {
        const button_handle_t button = device->buttons[BSP_BUTTON_AI];
        const esp_err_t short_result = iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, ai_short, device);
        const esp_err_t long_result = iot_button_register_cb(button, BUTTON_LONG_PRESS_START, NULL, ai_long, device);
        if (short_result != ESP_OK || long_result != ESP_OK) {
            ESP_LOGW(TAG, "AI button callback unavailable");
        }
    }
    create_boot_button(device);
    return ESP_OK;
}

esp_err_t app_tracker_start_bus(esp_gsp_handle_t ui, tracker_engine_t *engine,
                                app_synth_t *synth, app_audio_bus_t *bus,
                                app_tracker_t **out_handle)
{
    return app_tracker_start_workstation(ui, engine, synth, bus, NULL, NULL, NULL,
                                         NULL, NULL, out_handle);
}

esp_err_t app_tracker_start(esp_gsp_handle_t ui, tracker_engine_t *engine,
                            app_tracker_t **out_handle)
{
    return app_tracker_start_bus(ui, engine, NULL, NULL, out_handle);
}
