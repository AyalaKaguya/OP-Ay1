// SPDX-License-Identifier: Apache-2.0
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_settings.h"

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "settings_orientation: FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void hold(app_settings_orientation_t *orientation, float x, float y, float z,
                   uint64_t *now, bool locked, bool busy, int *last_rotation,
                   int *changes, unsigned samples)
{
    for (unsigned sample = 0; sample < samples; ++sample) {
        *now += 40;
        if (app_settings_orientation_update(orientation, x, y, z, *now,
                                             locked, busy, last_rotation)) {
            ++*changes;
        }
    }
}

static void four_directions(void)
{
    static const struct {
        float x;
        float y;
        int rotation;
    } cases[] = {
        {0.0f, -1.0f, 0}, {1.0f, 0.0f, 90},
        {0.0f, 1.0f, 180}, {-1.0f, 0.0f, 270},
    };
    for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        app_settings_orientation_t orientation;
        app_settings_orientation_init(&orientation, (cases[index].rotation + 90) % 360);
        uint64_t now = 0;
        int rotation = -1;
        int changes = 0;
        hold(&orientation, cases[index].x, cases[index].y, 0.0f,
               &now, false, false, &rotation, &changes, 10);
        require(changes == 0 && rotation == -1, "direction accepted before 400ms stable dwell");
        hold(&orientation, cases[index].x, cases[index].y, 0.0f,
               &now, false, false, &rotation, &changes, 1);
        require(changes == 1 && rotation == cases[index].rotation,
                "stable cardinal gravity did not select the expected direction");
        hold(&orientation, cases[index].x, cases[index].y, 0.0f,
               &now, false, false, &rotation, &changes, 100);
        require(changes == 1, "unchanged orientation repeatedly requested rotation");
    }
}

static void rejected_motion(void)
{
    app_settings_orientation_t orientation;
    app_settings_orientation_init(&orientation, 0);
    uint64_t now = 0;
    int rotation = -1;
    int changes = 0;
    hold(&orientation, 0.0f, 0.0f, 1.0f, &now, false, false, &rotation, &changes, 20);
    hold(&orientation, 0.0f, 0.0f, -1.0f, &now, false, false, &rotation, &changes, 20);
    hold(&orientation, 0.71f, 0.70f, 0.0f, &now, false, false, &rotation, &changes, 20);
    hold(&orientation, 2.0f, 0.0f, 0.0f, &now, false, false, &rotation, &changes, 20);
    hold(&orientation, 0.0f, 0.0f, 0.0f, &now, false, false, &rotation, &changes, 20);
    hold(&orientation, NAN, 1.0f, 0.0f, &now, false, false, &rotation, &changes, 20);
    hold(&orientation, INFINITY, 0.0f, 0.0f, &now, false, false, &rotation, &changes, 20);
    require(changes == 0 && rotation == -1,
            "flat, diagonal, nonfinite or non-gravity sample caused rotation");
    for (int sample = 0; sample < 50; ++sample) {
        hold(&orientation, sample % 2 ? 1.0f : -1.0f, 0.0f, 0.0f,
               &now, false, false, &rotation, &changes, 1);
    }
    require(changes == 0, "shaking between gravity axes caused rotation");
    hold(&orientation, 1.0f, 0.0f, 0.0f, &now, false, false, &rotation, &changes, 13);
    require(changes == 1 && rotation == 90, "orientation did not recover after motion rejection");
}

static void lock_and_interaction(void)
{
    for (unsigned guard = 0; guard < 2; ++guard) {
        app_settings_orientation_t orientation;
        app_settings_orientation_init(&orientation, 0);
        uint64_t now = 0;
        int rotation = -1;
        int changes = 0;
        hold(&orientation, 1.0f, 0.0f, 0.0f, &now, guard == 0, guard == 1,
               &rotation, &changes, 50);
        require(changes == 0, "locked or active interaction allowed auto rotation");
        hold(&orientation, 1.0f, 0.0f, 0.0f, &now, false, false,
               &rotation, &changes, 10);
        require(changes == 0, "unlock/release reused an old stable dwell");
        hold(&orientation, 1.0f, 0.0f, 0.0f, &now, false, false,
               &rotation, &changes, 1);
        require(changes == 1 && rotation == 90, "unlock/release did not resume auto rotation");
    }
}

static void interrupted_dwell(void)
{
    app_settings_orientation_t orientation;
    app_settings_orientation_init(&orientation, 0);
    uint64_t now = 0;
    int rotation = -1;
    int changes = 0;
    hold(&orientation, 1.0f, 0.0f, 0.0f, &now, false, false, &rotation, &changes, 8);
    now += 300;
    hold(&orientation, 1.0f, 0.0f, 0.0f, &now, false, false, &rotation, &changes, 10);
    require(changes == 0, "a stale sample gap completed an interrupted dwell");
    now = 1;
    hold(&orientation, 1.0f, 0.0f, 0.0f, &now, false, false, &rotation, &changes, 10);
    require(changes == 0, "backward clock completed a stale dwell");
    hold(&orientation, 1.0f, 0.0f, 0.0f, &now, false, false, &rotation, &changes, 3);
    require(changes == 1 && rotation == 90, "interrupted dwell did not recover");
}

int main(void)
{
    app_settings_snapshot_t snapshot;
    app_settings_snapshot_defaults(&snapshot);
    require(snapshot.brightness == 70 && snapshot.rotation == 0 && snapshot.rotation_locked,
            "settings defaults changed unexpectedly");
    require(!snapshot.battery_valid && !snapshot.battery_stale &&
            snapshot.charge == APP_SETTINGS_CHARGE_UNKNOWN && !snapshot.imu_available,
            "defaults invented sensor data");
    require(app_settings_rotation_valid(0) && app_settings_rotation_valid(90) &&
            app_settings_rotation_valid(180) && app_settings_rotation_valid(270) &&
            !app_settings_rotation_valid(-90) && !app_settings_rotation_valid(45) &&
            !app_settings_rotation_valid(360), "rotation validator accepted an unsupported direction");
    four_directions();
    rejected_motion();
    lock_and_interaction();
    interrupted_dwell();
    puts("settings_orientation: four directions, dwell, motion/flat rejection, lock/input and clock gaps PASS");
    return EXIT_SUCCESS;
}
