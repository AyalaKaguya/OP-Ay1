// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tracker_engine.h"

enum {
    TRACKER_PAGE_PLAYBACK = 2,
    TRACKER_PAGE_MIXER = 3,
    TRACKER_PAGE_PADS = 4,
};

/* Immutable render snapshot. Levels are 0..255, gain steps 0..120 (100=0dB),
 * volume 0..100, speed_multiplier 0.5..2.0, and mix_scroll is a pixel offset.
 * Pad indices are 0..15 or -1; pad_blink is supplied by the transport clock.
 * Rendering has no side effects and owns neither the frame nor this snapshot. */
typedef struct {
    tracker_status_t status;
    int page;
    int volume;
    float speed_multiplier;
    int mix_scroll;
    int channel_count;
    uint8_t levels[TRACKER_MAX_CHANNELS];
    uint8_t master_level;
    bool clipped;
    uint8_t gain_steps[TRACKER_MAX_CHANNELS];
    bool muted[TRACKER_MAX_CHANNELS];
    bool soloed[TRACKER_MAX_CHANNELS];
    int pad_playing;
    int pad_next;
    bool pad_stop;
    bool pad_blink;
} tracker_controls_view_t;

/* Draws rows 0..431 of a caller-owned 480x480 RGB565 frame. Rows 432..479
 * belong to the navigation owner and are never changed by this renderer. */
void tracker_render_controls(uint16_t *frame, const tracker_controls_view_t *view);
