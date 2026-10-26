// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_SETTINGS_CHARGE_UNKNOWN = 0,
    APP_SETTINGS_CHARGE_CHARGING,
    APP_SETTINGS_CHARGE_DISCHARGING,
    APP_SETTINGS_CHARGE_IDLE,
    APP_SETTINGS_CHARGE_FULL,
} app_settings_charge_t;

/* Snapshot values describe successfully applied settings. Battery percentage
 * comes from the fuel gauge, current is mA (+charging/-discharging), and
 * voltage is mV. Keep an old snapshot when a nonblocking read fails. */
typedef struct {
    int brightness;              /* 0..100 percent. */
    int rotation;                /* Clockwise degrees: 0, 90, 180, 270. */
    bool rotation_locked;        /* Freeze auto-rotation; manual turns work. */
    bool battery_valid;
    bool battery_stale;          /* A previously valid sample is now old. */
    int battery_percent;         /* 0..100, only valid with battery_valid. */
    int voltage_mv;
    int current_ma;
    app_settings_charge_t charge;
    uint64_t battery_updated_ms;
    bool imu_available;
    int last_error;              /* Device adapter's esp_err_t, 0 on success. */
    bool busy;                   /* Hardware change pending/in progress. */
} app_settings_snapshot_t;

/* Portable, single-owner orientation classifier. Device and host tests use
 * the same gravity/motion rejection, hysteresis and stable dwell timing. */
typedef struct {
    int applied_rotation;
    int candidate_rotation;
    uint64_t candidate_since_ms;
    uint64_t last_sample_ms;
    float previous_x;
    float previous_y;
    float previous_z;
    bool previous_valid;
} app_settings_orientation_t;

void app_settings_snapshot_defaults(app_settings_snapshot_t *snapshot);
bool app_settings_rotation_valid(int rotation);
void app_settings_orientation_init(app_settings_orientation_t *orientation,
                                   int rotation);
/* Returns true only when a new stable direction is accepted. Inputs are
 * BMI270 acceleration in g; interaction_busy and locked suppress changes. */
bool app_settings_orientation_update(app_settings_orientation_t *orientation,
                                     float x_g, float y_g, float z_g,
                                     uint64_t now_ms, bool locked,
                                     bool interaction_busy, int *out_rotation);

#ifdef __cplusplus
}
#endif
