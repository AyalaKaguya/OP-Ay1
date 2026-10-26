// SPDX-License-Identifier: Apache-2.0
#include "app_settings.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define DEFAULT_BRIGHTNESS_PERCENT 70
#define ORIENTATION_DWELL_MS 400U
#define ORIENTATION_MAX_SAMPLE_GAP_MS 200U
#define GRAVITY_MIN_G 0.75f
#define GRAVITY_MAX_G 1.25f
#define FLAT_Z_G 0.85f
#define AXIS_ENTER_G 0.68f
#define AXIS_DOMINANCE_G 0.18f
#define MOTION_DELTA_G 0.30f

void app_settings_snapshot_defaults(app_settings_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->brightness = DEFAULT_BRIGHTNESS_PERCENT;
    snapshot->rotation_locked = true;
    snapshot->charge = APP_SETTINGS_CHARGE_UNKNOWN;
}

bool app_settings_rotation_valid(int rotation)
{
    return rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
}

void app_settings_orientation_init(app_settings_orientation_t *o, int rotation)
{
    if (o == NULL) {
        return;
    }
    memset(o, 0, sizeof(*o));
    o->applied_rotation = app_settings_rotation_valid(rotation) ? rotation : 0;
    o->candidate_rotation = -1;
}

static int gravity_direction(float x, float y, float z)
{
    const float magnitude = sqrtf(x * x + y * y + z * z);
    if (!isfinite(magnitude) || magnitude < GRAVITY_MIN_G ||
            magnitude > GRAVITY_MAX_G || fabsf(z) > FLAT_Z_G) {
        return -1;
    }
    const float ax = fabsf(x);
    const float ay = fabsf(y);
    /* Keep the current angle around a diagonal; require a clear dominant
     * gravity axis before beginning the stable dwell for a new orientation. */
    if (ax >= AXIS_ENTER_G && ax >= ay + AXIS_DOMINANCE_G) {
        return x > 0.0f ? 90 : 270;
    }
    if (ay >= AXIS_ENTER_G && ay >= ax + AXIS_DOMINANCE_G) {
        return y > 0.0f ? 180 : 0;
    }
    return -1;
}

bool app_settings_orientation_update(app_settings_orientation_t *o,
                                     float x, float y, float z,
                                     uint64_t now_ms, bool locked,
                                     bool interaction_busy, int *out_rotation)
{
    if (o == NULL || out_rotation == NULL) {
        return false;
    }
    const bool finite = isfinite(x) && isfinite(y) && isfinite(z);
    const bool discontinuity = o->previous_valid &&
                               (now_ms < o->last_sample_ms ||
                                now_ms - o->last_sample_ms > ORIENTATION_MAX_SAMPLE_GAP_MS);
    const bool moving = o->previous_valid && finite &&
                        (fabsf(x - o->previous_x) > MOTION_DELTA_G ||
                         fabsf(y - o->previous_y) > MOTION_DELTA_G ||
                         fabsf(z - o->previous_z) > MOTION_DELTA_G);
    o->previous_x = x;
    o->previous_y = y;
    o->previous_z = z;
    o->previous_valid = finite;
    o->last_sample_ms = now_ms;
    if (!finite || locked || interaction_busy || discontinuity || moving) {
        o->candidate_rotation = -1;
        return false;
    }
    const int direction = gravity_direction(x, y, z);
    if (direction < 0 || direction == o->applied_rotation) {
        o->candidate_rotation = -1;
        return false;
    }
    if (direction != o->candidate_rotation) {
        o->candidate_rotation = direction;
        o->candidate_since_ms = now_ms;
        return false;
    }
    if (now_ms - o->candidate_since_ms < ORIENTATION_DWELL_MS) {
        return false;
    }
    o->applied_rotation = direction;
    o->candidate_rotation = -1;
    *out_rotation = direction;
    return true;
}
