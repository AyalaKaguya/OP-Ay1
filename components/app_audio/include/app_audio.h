// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "app_audio_bus.h"
#include "tracker_engine.h"

/* Compatibility wrapper: creates a process-lifetime tracker-only bus. */
esp_err_t app_audio_init(tracker_engine_t *engine);

/* Borrows bus for the process lifetime, starts the only renderer and ES8311
 * writer. Called from the startup task. Failed initialization rolls back and
 * can be retried. Repeated init of the same active bus succeeds. */
esp_err_t app_audio_init_bus(app_audio_bus_t *bus);

/* Master volume 0..100 applied through the codec hardware volume. */
void app_audio_set_volume(int percent);

/* Tracker transport only; synth notes remain independently playable. */
void app_audio_play(void);
void app_audio_pause(void);
