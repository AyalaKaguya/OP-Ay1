// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tracker_engine.h"

#define TRACKER_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BG TRACKER_RGB565(11, 12, 14)
#define C_PANEL TRACKER_RGB565(16, 18, 22)
#define C_LINE TRACKER_RGB565(23, 26, 31)
#define C_BORDER TRACKER_RGB565(42, 46, 53)
#define C_TEXT TRACKER_RGB565(242, 242, 240)
#define C_MUTED TRACKER_RGB565(123, 130, 141)
#define C_ROW TRACKER_RGB565(90, 96, 104)
#define C_DIM TRACKER_RGB565(42, 46, 53)
#define C_DISABLED TRACKER_RGB565(58, 60, 64)
#define C_ACCENT TRACKER_RGB565(255, 76, 1)
#define C_CYAN TRACKER_RGB565(55, 208, 255)
#define C_FX TRACKER_RGB565(255, 194, 75)
#define C_GREEN TRACKER_RGB565(58, 219, 107)
#define C_CURRENT_ROW TRACKER_RGB565(30, 34, 40)
#define C_MASK_NOTE TRACKER_RGB565(122, 126, 132)
#define C_MASK_DIM TRACKER_RGB565(58, 60, 64)
#define C_MASK_ROW TRACKER_RGB565(78, 80, 84)

/* All coordinates are pixels. The caller owns a 480x480 RGB565 frame;
 * initialization clips drawing to its 432-pixel content area. Clip state is
 * local to one render call, so independent renderers never share mutable state. */
typedef struct {
    uint16_t *frame;
    int left;
    int top;
    int right;
    int bottom;
} tracker_graphics_t;

void tracker_gfx_init(tracker_graphics_t *gfx, uint16_t *frame);
void tracker_gfx_clip(tracker_graphics_t *gfx, int x, int y, int w, int h);
void tracker_gfx_rect(tracker_graphics_t *gfx, int x, int y, int w, int h,
                      uint16_t color);
void tracker_gfx_outline(tracker_graphics_t *gfx, int x, int y, int w, int h,
                         uint16_t color);
void tracker_gfx_line(tracker_graphics_t *gfx, int x0, int y0, int x1, int y1,
                      uint16_t color);
void tracker_gfx_text(tracker_graphics_t *gfx, int x, int y, int scale,
                      uint16_t color, const char *text);
void tracker_gfx_text_right(tracker_graphics_t *gfx, int right, int y, int scale,
                            uint16_t color, const char *text);
void tracker_gfx_text_center(tracker_graphics_t *gfx, int center, int y, int scale,
                             uint16_t color, const char *text);
void tracker_gfx_slider(tracker_graphics_t *gfx, int x, int y, int w,
                        float fraction, uint16_t color, bool show_unity);
void tracker_gfx_meter(tracker_graphics_t *gfx, int x, int y, int w, int h,
                       int level);
void tracker_gfx_button(tracker_graphics_t *gfx, int x, int y, int w, int h,
                        const char *label, uint16_t color, bool active);
uint16_t tracker_gfx_channel_color(int channel);

/* The compact status and order bars are shared by all tracker pages. */
void tracker_gfx_header(tracker_graphics_t *gfx, const tracker_status_t *status,
                        const char *state_label);
void tracker_gfx_order_strip(tracker_graphics_t *gfx,
                             const tracker_status_t *status);
