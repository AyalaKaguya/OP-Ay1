// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app_audio_bus.h"

typedef struct host_audio host_audio_t;

/* UI-thread owned. Windows optionally uses waveOut; other hosts use a silent
 * clocked sink. A capture WAV can be selected through OP_AY1_SIM_WAV. */
host_audio_t *host_audio_create(app_audio_bus_t *bus);
void host_audio_step(host_audio_t *audio, uint64_t time_ms);
void host_audio_volume(host_audio_t *audio, int percent);
void host_audio_destroy(host_audio_t *audio);
