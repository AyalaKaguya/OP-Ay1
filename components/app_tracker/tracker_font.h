// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

/* 5x7 bitmap text for the canvas tracker pages. Coordinates clip to the frame. */
void tracker_font_draw(uint16_t *frame, int frame_w, int frame_h, int x, int y,
                       int scale, uint16_t color, const char *text);

/* Advance in pixels for a string at the given scale. */
int tracker_font_width(const char *text, int scale);
