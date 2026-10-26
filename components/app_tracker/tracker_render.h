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
#define TRACKER_MIX_HEADER_H 164
#define TRACKER_MIX_VIEW_Y 164
#define TRACKER_MIX_VIEW_H 264
#define TRACKER_MIX_STRIP_H 66
#define TRACKER_MIX_STRIPS 16
#define TRACKER_MIX_FADER_X 86
#define TRACKER_MIX_FADER_W 232
#define TRACKER_MIX_FADER_Y 41
#define TRACKER_MIX_FADER_H 4
#define TRACKER_MIX_FADER_HIT_Y 21
#define TRACKER_MIX_FADER_HIT_H 44
#define TRACKER_MIX_MUTE_X 334
#define TRACKER_MIX_MUTE_Y 9
#define TRACKER_MIX_MUTE_W 44
#define TRACKER_MIX_MUTE_H 46
#define TRACKER_MIX_SOLO_X 392
#define TRACKER_MIX_MASTER_X 100
#define TRACKER_MIX_MASTER_Y 86
#define TRACKER_MIX_MASTER_W 290
#define TRACKER_MIX_MASTER_H 4

/* First song row shown in the scrolling pattern window. */
int tracker_pattern_top_line(tracker_engine_t *engine);

/* Renders rows 0..431; the navigation owner draws rows 432..479 separately. */
void tracker_render_pattern(uint16_t *frame, tracker_engine_t *engine);

/* Scope uses caller-owned reusable scratch; sample_count is its int16 capacity.
 * A NULL scratch or fewer than two samples draws the grid without wave traces. */
void tracker_render_waves(uint16_t *frame, tracker_engine_t *engine,
                           int16_t *scratch, int sample_count);

