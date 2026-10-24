// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "tracker_engine.h"

/* Opens the ES8311 speaker path at 48 kHz/16-bit/stereo and starts the audio
 * task. The engine is rendered by this task only. Idempotent. */
esp_err_t app_audio_init(tracker_engine_t *engine);

/* Master volume 0..100 applied through the codec hardware volume. */
void app_audio_set_volume(int percent);

/* Starts / stops the audio task loop (playback state lives in the engine). */
void app_audio_play(void);
void app_audio_pause(void);
