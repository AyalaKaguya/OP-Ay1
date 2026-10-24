// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#include "tracker_engine.h"

#define TRACKER_FRAME_W 480
#define TRACKER_FRAME_H 432
#define TRACKER_GRID_Y 46
#define TRACKER_ROW_H 19
#define TRACKER_VISIBLE_ROWS 19
#define TRACKER_WAVE_GRID_Y 40
#define TRACKER_WAVE_TILE_H 80
#define TRACKER_MIX_HEADER_H 96
#define TRACKER_MIX_STRIP_H 150
#define TRACKER_MIX_STRIPS 16
#define TRACKER_MIX_FADER_X 20
#define TRACKER_MIX_FADER_W 300
#define TRACKER_MIX_FADER_Y 58
#define TRACKER_MIX_FADER_H 10
#define TRACKER_MIX_MUTE_X 300
#define TRACKER_MIX_MUTE_Y 8
#define TRACKER_MIX_MUTE_W 66
#define TRACKER_MIX_MUTE_H 38
#define TRACKER_MIX_SOLO_X 376
#define TRACKER_MIX_MASTER_X 90
#define TRACKER_MIX_MASTER_Y 40
#define TRACKER_MIX_MASTER_W 360
#define TRACKER_MIX_MASTER_H 10

/* First song row shown in the scrolling pattern window. */
int tracker_pattern_top_line(tracker_engine_t *engine);

/* Renders the full 480x480 RGB565 frame for the pattern grid page. */
void tracker_render_pattern(uint16_t *frame, tracker_engine_t *engine);

/* Renders the full 480x480 RGB565 frame for the per-track scope page. */
void tracker_render_waves(uint16_t *frame, tracker_engine_t *engine);

/* Mixer page: fixed header (master volume + master meter) plus 16 scrollable
 * channel strips. scroll is the content offset in pixels (0..content-336). */
void tracker_render_mix(uint16_t *frame, tracker_engine_t *engine, int scroll,
                        int volume, const uint8_t *levels,
                        uint8_t master_level);

