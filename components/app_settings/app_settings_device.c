// SPDX-License-Identifier: Apache-2.0
#include "app_settings_device.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "bq27220.h"
#include "bsp/esp_mosaico.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define SETTINGS_TASK_STACK_BYTES 6144
#define SETTINGS_TASK_PRIORITY 2
#define SENSOR_PERIOD_MS 40U
#define BATTERY_PERIOD_MS 1000U
#define BATTERY_STALE_MS 5000U
#define STORAGE_DEBOUNCE_MS 600U
#define STORAGE_RETRY_MS 5000U
#define DISPLAY_RETRY_MS 1000U
#define DISPLAY_PAUSE_TIMEOUT_MS 1000U
#define SETTINGS_NVS_VERSION 1U
#define BATTERY_MIN_MV 2000
#define BATTERY_MAX_MV 5500
#define CHARGE_CURRENT_THRESHOLD_MA 2

static const char *TAG = "app_settings";
/* BSP display/IMU/fuel gauge are process-wide services. One settings instance
 * claims their product policy; no device callbacks borrow this context. */
static atomic_flag service_claimed = ATOMIC_FLAG_INIT;

struct app_settings {
    esp_gsp_handle_t ui;
    esp_display_presenter_t *presenter;
    esp_gsp_esp_lcd_pause_t *pause;
    TaskHandle_t task;
    SemaphoreHandle_t snapshot_lock;
    SemaphoreHandle_t stopped;
    atomic_int requested_brightness;
    atomic_int requested_rotation;
    atomic_bool rotation_requested;
    atomic_bool requested_lock;
    atomic_bool interaction_busy;
    atomic_bool stop_requested;
    app_settings_snapshot_t current;
    app_settings_snapshot_t published;
    app_settings_orientation_t orientation;
    bsp_battery_status_t battery;
    nvs_handle_t storage;
    bool storage_open;
    bool storage_write_allowed;
    bool storage_dirty;
    bool brightness_applied;
    bool imu_owned;
    bool imu_running;
    bool battery_owned;
    bool battery_ready;
    bool battery_seen;
    bool manual_rotation_pending;
    int pending_rotation;
    int display_error;
    int storage_error;
    int imu_error;
    int battery_error;
    uint64_t save_after_ms;
    uint64_t display_retry_ms;
    uint64_t next_battery_ms;
    uint64_t next_sensor_ms;
};

static uint64_t clock_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static TickType_t ticks_for_ms(uint32_t ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks == 0 && ms != 0 ? 1 : ticks;
}

static void mark_storage_dirty(app_settings_t *s, uint64_t now)
{
    s->storage_dirty = true;
    s->save_after_ms = now + STORAGE_DEBOUNCE_MS;
}

static void load_preferences(app_settings_t *s)
{
    s->storage_error = nvs_open("opay_settings", NVS_READWRITE, &s->storage);
    if (s->storage_error != ESP_OK) {
        return;
    }
    s->storage_open = true;
    uint8_t version = 0;
    esp_err_t result = nvs_get_u8(s->storage, "version", &version);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        s->storage_write_allowed = true;
        s->storage_error = ESP_OK;
        return;
    }
    if (result != ESP_OK || version != SETTINGS_NVS_VERSION) {
        s->storage_error = result != ESP_OK ? result : ESP_ERR_NOT_SUPPORTED;
        return;
    }
    s->storage_write_allowed = true;
    uint8_t brightness = 0;
    uint8_t locked = 0;
    uint16_t rotation = 0;
    const esp_err_t brightness_error = nvs_get_u8(s->storage, "brightness", &brightness);
    const esp_err_t rotation_error = nvs_get_u16(s->storage, "rotation", &rotation);
    const esp_err_t lock_error = nvs_get_u8(s->storage, "locked", &locked);
    if (brightness_error != ESP_OK || rotation_error != ESP_OK || lock_error != ESP_OK ||
            brightness > 100 || locked > 1 || !app_settings_rotation_valid(rotation)) {
        s->storage_error = ESP_ERR_INVALID_STATE;
        return;
    }
    atomic_store_explicit(&s->requested_brightness, brightness, memory_order_relaxed);
    atomic_store_explicit(&s->requested_rotation, rotation, memory_order_relaxed);
    atomic_store_explicit(&s->requested_lock, locked != 0, memory_order_relaxed);
    s->pending_rotation = rotation;
    s->storage_error = ESP_OK;
}

static void save_preferences(app_settings_t *s, uint64_t now, bool force)
{
    if (!s->storage_open || !s->storage_write_allowed || !s->storage_dirty ||
            (!force && now < s->save_after_ms)) {
        return;
    }
    esp_err_t result = nvs_set_u8(s->storage, "version", SETTINGS_NVS_VERSION);
    if (result == ESP_OK) {
        result = nvs_set_u8(s->storage, "brightness", (uint8_t)s->current.brightness);
    }
    if (result == ESP_OK) {
        result = nvs_set_u16(s->storage, "rotation", (uint16_t)s->current.rotation);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(s->storage, "locked", s->current.rotation_locked ? 1U : 0U);
    }
    if (result == ESP_OK) {
        result = nvs_commit(s->storage);
    }
    s->storage_error = result;
    if (result == ESP_OK) {
        s->storage_dirty = false;
    } else {
        s->save_after_ms = now + STORAGE_RETRY_MS;
    }
}

static void publish(app_settings_t *s, uint64_t now)
{
    s->current.busy = s->pause != NULL || s->pending_rotation >= 0 ||
                      atomic_load_explicit(&s->rotation_requested, memory_order_acquire) ||
                      !s->brightness_applied || s->current.brightness !=
                      atomic_load_explicit(&s->requested_brightness, memory_order_relaxed);
    const bool stale = s->battery_seen && now - s->current.battery_updated_ms >= BATTERY_STALE_MS;
    s->current.battery_stale = stale;
    s->current.battery_valid = s->battery_seen && !stale && s->battery_error == ESP_OK;
    if (!s->current.battery_valid) {
        s->current.charge = APP_SETTINGS_CHARGE_UNKNOWN;
    }
    s->current.last_error = s->display_error != ESP_OK ? s->display_error
                            : s->storage_error != ESP_OK ? s->storage_error
                            : s->imu_error != ESP_OK ? s->imu_error : s->battery_error;
    if (xSemaphoreTake(s->snapshot_lock, portMAX_DELAY) == pdTRUE) {
        s->published = s->current;
        (void)xSemaphoreGive(s->snapshot_lock);
    }
}

static bool resume_display(app_settings_t *s)
{
    if (s->pause == NULL) {
        return true;
    }
    esp_gsp_handle_t resumed = NULL;
    const esp_err_t result = esp_gsp_esp_lcd_resume_paused(s->pause, &resumed);
    if (result != ESP_OK) {
        s->display_error = result;
        return false;
    }
    s->pause = NULL;
    if (resumed != s->ui) {
        s->display_error = ESP_ERR_INVALID_STATE;
        return false;
    }
    s->display_error = ESP_OK;
    return true;
}

static void apply_display(app_settings_t *s, uint64_t now)
{
    if (!resume_display(s) || now < s->display_retry_ms) {
        return;
    }
    const int brightness = atomic_load_explicit(&s->requested_brightness, memory_order_relaxed);
    const bool set_brightness = !s->brightness_applied || brightness != s->current.brightness;
    if (s->pending_rotation == s->current.rotation) {
        s->pending_rotation = -1;
        s->manual_rotation_pending = false;
    }
    const bool set_rotation = s->pending_rotation >= 0 &&
                              !atomic_load_explicit(&s->interaction_busy, memory_order_acquire);
    if (!set_brightness && !set_rotation) {
        return;
    }
    esp_err_t result = esp_gsp_esp_lcd_pause(s->ui, DISPLAY_PAUSE_TIMEOUT_MS, &s->pause);
    if (result != ESP_OK) {
        s->display_error = result;
        s->display_retry_ms = now + DISPLAY_RETRY_MS;
        return;
    }
    /* Brightness commands and rotation both share the panel's command stream
     * with GRAM transfers; the pause fence serializes them with the renderer. */
    if (set_brightness) {
        result = bsp_display_brightness_set(brightness);
        if (result == ESP_OK) {
            s->current.brightness = brightness;
            s->brightness_applied = true;
            mark_storage_dirty(s, now);
        }
    }
    if (result == ESP_OK && set_rotation &&
            !atomic_load_explicit(&s->interaction_busy, memory_order_acquire)) {
        result = esp_display_presenter_set_rotation(s->presenter,
                                                    (esp_display_present_rotation_t)s->pending_rotation);
        if (result == ESP_OK) {
            s->current.rotation = s->pending_rotation;
            s->pending_rotation = -1;
            s->manual_rotation_pending = false;
            app_settings_orientation_init(&s->orientation, s->current.rotation);
            mark_storage_dirty(s, now);
        }
    }
    const bool resumed = resume_display(s);
    if (result != ESP_OK) {
        s->display_error = result;
        s->display_retry_ms = now + DISPLAY_RETRY_MS;
        app_settings_orientation_init(&s->orientation, s->current.rotation);
    } else if (resumed) {
        s->display_error = ESP_OK;
        s->display_retry_ms = now + SENSOR_PERIOD_MS;
    }
}

static void initialize_sensors(app_settings_t *s)
{
    if (bsp_imu_get_handle() == NULL) {
        /* The service is the sole product IMU owner when no BSP handle exists;
         * retain cleanup responsibility even if BSP leaves a partial handle. */
        s->imu_owned = true;
        s->imu_error = bsp_imu_init();
        if (s->imu_error == ESP_OK) {
            s->imu_error = bsp_imu_start(NULL);
            s->imu_running = s->imu_error == ESP_OK;
        }
    } else {
        float x, y, z;
        s->imu_error = bsp_imu_get_accel(&x, &y, &z);
        s->imu_running = s->imu_error == ESP_OK;
    }
    s->current.imu_available = s->imu_running;

    /* A pre-existing fuel gauge remains borrowed. Only detach it on stop if
     * this service observed the uninitialized state and initialized it. */
    s->battery_error = bsp_battery_read(&s->battery);
    if (s->battery_error == ESP_ERR_INVALID_STATE) {
        s->battery_error = bsp_battery_init();
        s->battery_owned = s->battery_error == ESP_OK;
    }
    s->battery_ready = s->battery_error == ESP_OK;
}

static void poll_orientation(app_settings_t *s, uint64_t now)
{
    if (!s->imu_running || now < s->next_sensor_ms) {
        return;
    }
    s->next_sensor_ms = now + SENSOR_PERIOD_MS;
    float x, y, z;
    s->imu_error = bsp_imu_get_accel(&x, &y, &z);
    s->current.imu_available = s->imu_error == ESP_OK;
    if (s->imu_error != ESP_OK) {
        app_settings_orientation_init(&s->orientation, s->current.rotation);
        return;
    }
    int rotation = s->current.rotation;
    const bool pending_manual = atomic_load_explicit(&s->rotation_requested, memory_order_acquire);
    const bool busy = atomic_load_explicit(&s->interaction_busy, memory_order_acquire) ||
                      s->pending_rotation >= 0 || pending_manual;
    if (app_settings_orientation_update(&s->orientation, x, y, z, now,
                                        s->current.rotation_locked, busy, &rotation)) {
        s->pending_rotation = rotation;
        s->manual_rotation_pending = false;
    }
}

static void poll_battery(app_settings_t *s, uint64_t now)
{
    if (!s->battery_ready || now < s->next_battery_ms) {
        return;
    }
    s->next_battery_ms = now + BATTERY_PERIOD_MS;
    s->battery_error = bsp_battery_read(&s->battery);
    if (s->battery_error != ESP_OK) {
        return;
    }
    /* Several BSP gauge getters report zero on bus failure. Reject impossible
     * voltage/SOC instead of inventing a valid 0-percent battery reading. */
    if (s->battery.voltage_mv < BATTERY_MIN_MV || s->battery.voltage_mv > BATTERY_MAX_MV ||
            s->battery.state_of_charge > 100) {
        s->battery_error = ESP_ERR_INVALID_RESPONSE;
        return;
    }
    s->current.battery_percent = s->battery.state_of_charge;
    s->current.voltage_mv = s->battery.voltage_mv;
    s->current.current_ma = s->battery.current_ma;
    const battery_status_t flags = {.full = s->battery.status_flags};
    s->current.charge = flags.DSG || s->battery.current_ma < -CHARGE_CURRENT_THRESHOLD_MA
                        ? APP_SETTINGS_CHARGE_DISCHARGING
                        : s->battery.current_ma > CHARGE_CURRENT_THRESHOLD_MA
                        ? APP_SETTINGS_CHARGE_CHARGING
                        : flags.FC ? APP_SETTINGS_CHARGE_FULL : APP_SETTINGS_CHARGE_IDLE;
    s->current.battery_updated_ms = now;
    s->battery_seen = true;
}

static void consume_requests(app_settings_t *s, uint64_t now)
{
    const bool locked = atomic_load_explicit(&s->requested_lock, memory_order_relaxed);
    if (locked != s->current.rotation_locked) {
        s->current.rotation_locked = locked;
        app_settings_orientation_init(&s->orientation, s->current.rotation);
        if (locked && !s->manual_rotation_pending) {
            /* A manual request below supersedes this cancellation. */
            s->pending_rotation = -1;
        }
        mark_storage_dirty(s, now);
    }
    if (atomic_exchange_explicit(&s->rotation_requested, false, memory_order_acq_rel)) {
        s->pending_rotation = atomic_load_explicit(&s->requested_rotation, memory_order_relaxed);
        s->manual_rotation_pending = true;
        app_settings_orientation_init(&s->orientation, s->current.rotation);
    }
}

static esp_err_t release_owned_sensors(app_settings_t *s)
{
    if (s->imu_owned) {
        const esp_err_t stopped = s->imu_running ? bsp_imu_stop() : ESP_OK;
        const esp_err_t released = stopped == ESP_OK ? bsp_imu_deinit() : stopped;
        if (released != ESP_OK) {
            s->imu_error = released;
            return released;
        }
        s->imu_owned = false;
        s->imu_running = false;
    }
    if (s->battery_owned) {
        const esp_err_t result = bsp_battery_deinit();
        if (result != ESP_OK) {
            s->battery_error = result;
            return result;
        }
        s->battery_owned = false;
    }
    return ESP_OK;
}

static void settings_task(void *argument)
{
    app_settings_t *s = argument;
    initialize_sensors(s);
    ESP_LOGI(TAG, "settings worker ready: imu=%d battery=%d storage=%d",
             s->imu_running, s->battery_ready, s->storage_open);
    while (true) {
        const uint64_t now = clock_ms();
        if (atomic_load_explicit(&s->stop_requested, memory_order_acquire)) {
            if (resume_display(s) && release_owned_sensors(s) == ESP_OK) {
                break;
            }
        } else {
            consume_requests(s, now);
            poll_orientation(s, now);
            poll_battery(s, now);
            apply_display(s, now);
            save_preferences(s, now, false);
        }
        publish(s, now);
        (void)ulTaskNotifyTake(pdTRUE, ticks_for_ms(SENSOR_PERIOD_MS));
    }
    save_preferences(s, clock_ms(), true);
    if (s->storage_open) {
        nvs_close(s->storage);
        s->storage_open = false;
    }
    (void)xSemaphoreGive(s->stopped);
    /* Keep the task handle valid after a caller's stop timeout. The exclusive
     * stop owner deletes this inert task after receiving the completion signal.
     * No context access is permitted after signalling. */
    vTaskSuspend(NULL);
}

static void wake(app_settings_t *s)
{
    if (s->task != NULL) {
        (void)xTaskNotifyGive(s->task);
    }
}

esp_err_t app_settings_start(esp_gsp_handle_t ui,
                             esp_display_presenter_t *presenter,
                             app_settings_t **out_settings)
{
    if (ui == NULL || presenter == NULL || out_settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_settings = NULL;
    if (atomic_flag_test_and_set_explicit(&service_claimed, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    app_settings_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        atomic_flag_clear_explicit(&service_claimed, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    s->ui = ui;
    s->presenter = presenter;
    s->pending_rotation = -1;
    app_settings_snapshot_defaults(&s->current);
    s->current.busy = true;
    s->published = s->current;
    atomic_init(&s->requested_brightness, s->current.brightness);
    atomic_init(&s->requested_rotation, s->current.rotation);
    atomic_init(&s->rotation_requested, false);
    atomic_init(&s->requested_lock, s->current.rotation_locked);
    atomic_init(&s->interaction_busy, false);
    atomic_init(&s->stop_requested, false);
    s->snapshot_lock = xSemaphoreCreateMutex();
    s->stopped = xSemaphoreCreateBinary();
    if (s->snapshot_lock == NULL || s->stopped == NULL) {
        goto no_memory;
    }
    load_preferences(s);
    app_settings_orientation_init(&s->orientation, 0);
    if (xTaskCreate(settings_task, "app_settings", SETTINGS_TASK_STACK_BYTES, s,
                    SETTINGS_TASK_PRIORITY, &s->task) != pdPASS) {
        goto no_memory;
    }
    *out_settings = s;
    return ESP_OK;

no_memory:
    if (s->storage_open) {
        nvs_close(s->storage);
    }
    if (s->snapshot_lock != NULL) {
        vSemaphoreDelete(s->snapshot_lock);
    }
    if (s->stopped != NULL) {
        vSemaphoreDelete(s->stopped);
    }
    free(s);
    atomic_flag_clear_explicit(&service_claimed, memory_order_release);
    return ESP_ERR_NO_MEM;
}

static esp_err_t request_ready(const app_settings_t *s)
{
    return s == NULL ? ESP_ERR_INVALID_ARG
           : atomic_load_explicit(&s->stop_requested, memory_order_acquire)
           ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t app_settings_request_brightness(app_settings_t *s, int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = request_ready(s);
    if (result != ESP_OK) {
        return result;
    }
    atomic_store_explicit(&s->requested_brightness, percent, memory_order_relaxed);
    wake(s);
    return ESP_OK;
}

esp_err_t app_settings_request_rotation(app_settings_t *s, int degrees)
{
    if (!app_settings_rotation_valid(degrees)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = request_ready(s);
    if (result != ESP_OK) {
        return result;
    }
    atomic_store_explicit(&s->requested_rotation, degrees, memory_order_relaxed);
    atomic_store_explicit(&s->rotation_requested, true, memory_order_release);
    wake(s);
    return ESP_OK;
}

esp_err_t app_settings_request_lock(app_settings_t *s, bool locked)
{
    const esp_err_t result = request_ready(s);
    if (result != ESP_OK) {
        return result;
    }
    atomic_store_explicit(&s->requested_lock, locked, memory_order_relaxed);
    wake(s);
    return ESP_OK;
}

void app_settings_set_interaction_busy(app_settings_t *s, bool busy)
{
    if (request_ready(s) == ESP_OK) {
        atomic_store_explicit(&s->interaction_busy, busy, memory_order_release);
        if (!busy) {
            wake(s);
        }
    }
}

esp_err_t app_settings_read_snapshot(app_settings_t *s,
                                     app_settings_snapshot_t *out_snapshot)
{
    if (s == NULL || out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s->snapshot_lock, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_snapshot = s->published;
    (void)xSemaphoreGive(s->snapshot_lock);
    return ESP_OK;
}

esp_err_t app_settings_stop(app_settings_t **settings, uint32_t timeout_ms)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*settings == NULL) {
        return ESP_OK;
    }
    app_settings_t *s = *settings;
    atomic_store_explicit(&s->stop_requested, true, memory_order_release);
    wake(s);
    if (xSemaphoreTake(s->stopped, ticks_for_ms(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelete(s->task);
    vSemaphoreDelete(s->snapshot_lock);
    vSemaphoreDelete(s->stopped);
    free(s);
    *settings = NULL;
    atomic_flag_clear_explicit(&service_claimed, memory_order_release);
    return ESP_OK;
}
