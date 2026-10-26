// SPDX-License-Identifier: Apache-2.0
#include "tracker_graphics.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_font.h"
#include "tracker_font_data.h"

#define FRAME_W 480
#define FRAME_H 480
#define CONTENT_H 432

static int bounded(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static unsigned display_hex(int value)
{
    /* The decoder supports 256 orders/patterns; totals must show 100 in hex. */
    return (unsigned)bounded(value, 0, 256);
}

void tracker_gfx_init(tracker_graphics_t *gfx, uint16_t *frame)
{
    *gfx = (tracker_graphics_t) {
        frame, 0, 0, FRAME_W, CONTENT_H
    };
}

void tracker_gfx_clip(tracker_graphics_t *gfx, int x, int y, int w, int h)
{
    gfx->left = bounded(x, 0, FRAME_W);
    gfx->top = bounded(y, 0, FRAME_H);
    const int64_t right = (int64_t)x + (w > 0 ? w : 0);
    const int64_t bottom = (int64_t)y + (h > 0 ? h : 0);
    gfx->right = right < gfx->left ? gfx->left : right > FRAME_W ? FRAME_W : (int)right;
    gfx->bottom = bottom < gfx->top ? gfx->top : bottom > FRAME_H ? FRAME_H : (int)bottom;
}

void tracker_gfx_rect(tracker_graphics_t *gfx, int x, int y, int w, int h,
                      uint16_t color)
{
    if (gfx == NULL || gfx->frame == NULL || w <= 0 || h <= 0 ||
            x >= gfx->right || y >= gfx->bottom) {
        return;
    }
    const int64_t requested_right = (int64_t)x + w;
    const int64_t requested_bottom = (int64_t)y + h;
    if (requested_right <= gfx->left || requested_bottom <= gfx->top) {
        return;
    }
    const int x0 = x < gfx->left ? gfx->left : x;
    const int y0 = y < gfx->top ? gfx->top : y;
    const int x1 = requested_right > gfx->right ? gfx->right : (int)requested_right;
    const int y1 = requested_bottom > gfx->bottom ? gfx->bottom : (int)requested_bottom;
    for (int row = y0; row < y1; ++row) {
        uint16_t *line = gfx->frame + (size_t)row * FRAME_W;
        for (int column = x0; column < x1; ++column) {
            line[column] = color;
        }
    }
}

void tracker_gfx_outline(tracker_graphics_t *gfx, int x, int y, int w, int h,
                         uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    tracker_gfx_rect(gfx, x, y, w, 1, color);
    tracker_gfx_rect(gfx, x, y + h - 1, w, 1, color);
    tracker_gfx_rect(gfx, x, y, 1, h, color);
    tracker_gfx_rect(gfx, x + w - 1, y, 1, h, color);
}

void tracker_gfx_line(tracker_graphics_t *gfx, int x0, int y0, int x1, int y1,
                      uint16_t color)
{
    /* UI geometry stays within one frame; clip samples at the final write. */
    const int dx = abs(x1 - x0);
    const int dy = -abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        tracker_gfx_rect(gfx, x0, y0, 1, 1, color);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        const int twice_error = error * 2;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void tracker_gfx_text(tracker_graphics_t *gfx, int x, int y, int scale,
                      uint16_t color, const char *text)
{
    if (gfx == NULL || gfx->frame == NULL || text == NULL || scale < 1 ||
            scale > 16 || x >= gfx->right || y >= gfx->bottom ||
            (int64_t)y + TRACKER_FONT_HEIGHT * scale <= gfx->top) {
        return;
    }
    /* Use the existing font's exact glyphs and advances; rectangle clipping
     * also handles a row that is partially hidden by the mixer viewport. */
    int cursor = x;
    for (const unsigned char *letter = (const unsigned char *)text; *letter; ++letter) {
        unsigned code = *letter;
        if (code >= 'a' && code <= 'z') {
            code -= 'a' - 'A';
        }
        if (code >= TRACKER_FONT_FIRST && code <= TRACKER_FONT_LAST) {
            const uint8_t *glyph = tracker_font_5x7[code - TRACKER_FONT_FIRST];
            for (int row = 0; row < TRACKER_FONT_HEIGHT; ++row) {
                for (int column = 0; column < TRACKER_FONT_WIDTH; ++column) {
                    if (glyph[row] & (1U << (TRACKER_FONT_WIDTH - 1 - column))) {
                        tracker_gfx_rect(gfx, cursor + column * scale,
                                         y + row * scale, scale, scale, color);
                    }
                }
            }
        }
        cursor += (TRACKER_FONT_WIDTH + 1) * scale;
        if (cursor >= gfx->right) {
            break;
        }
    }
}

void tracker_gfx_text_right(tracker_graphics_t *gfx, int right, int y, int scale,
                            uint16_t color, const char *text)
{
    tracker_gfx_text(gfx, right - tracker_font_width(text, scale), y, scale,
                     color, text);
}

void tracker_gfx_text_center(tracker_graphics_t *gfx, int center, int y, int scale,
                             uint16_t color, const char *text)
{
    tracker_gfx_text(gfx, center - (tracker_font_width(text, scale) - scale) / 2,
                     y, scale, color, text);
}

void tracker_gfx_slider(tracker_graphics_t *gfx, int x, int y, int w,
                        float fraction, uint16_t color, bool show_unity)
{
    if (w < 12) {
        return;
    }
    if (!isfinite(fraction)) {
        fraction = 0.0f;
    }
    fraction = fminf(fmaxf(fraction, 0.0f), 1.0f);
    tracker_gfx_rect(gfx, x, y, w, 4, C_BORDER);
    for (int step = 0; step <= 8; ++step) {
        const int tick = x + (step * (w - 1) + 4) / 8;
        tracker_gfx_rect(gfx, tick, y + 7, 1, 3, C_BORDER);
    }
    if (show_unity) {
        tracker_gfx_rect(gfx, x + ((w - 1) * 100 + 60) / 120, y - 4,
                         1, 16, C_MUTED);
    }
    const int fill = (int)lroundf((float)w * fraction);
    tracker_gfx_rect(gfx, x, y, fill > 0 ? fill : 1, 4, color);
    const int knob = bounded(x + (int)lroundf((float)(w - 1) * fraction),
                             x + 5, x + w - 6);
    tracker_gfx_rect(gfx, knob - 5, y - 8, 11, 20, C_TEXT);
    tracker_gfx_rect(gfx, knob - 2, y - 3, 5, 10, C_PANEL);
}

void tracker_gfx_meter(tracker_graphics_t *gfx, int x, int y, int w, int h,
                       int level)
{
    const int count = w / 5;
    if (count <= 0) {
        return;
    }
    const float db = level > 0 ? 20.0f * log10f((float)bounded(level, 0, 255) / 255.0f)
                     : -48.0f;
    for (int index = 0; index < count; ++index) {
        const float threshold = -48.0f + 48.0f * (float)(index + 1) / (float)count;
        const uint16_t color = threshold <= -12.0f ? C_GREEN
                               : threshold <= -3.0f ? C_FX : C_ACCENT;
        tracker_gfx_rect(gfx, x + index * 5, y, 3, h,
                         threshold <= db ? color : C_LINE);
    }
}

void tracker_gfx_button(tracker_graphics_t *gfx, int x, int y, int w, int h,
                        const char *label, uint16_t color, bool active)
{
    tracker_gfx_rect(gfx, x, y, w, h,
                     active ? TRACKER_RGB565(25, 29, 35) : C_PANEL);
    tracker_gfx_outline(gfx, x, y, w, h, active ? color : C_BORDER);
    tracker_gfx_text_center(gfx, x + w / 2, y + (h - 14) / 2, 2, color, label);
    if (active) {
        tracker_gfx_rect(gfx, x + 1, y + h - 4, w - 2, 3, color);
    }
}

uint16_t tracker_gfx_channel_color(int channel)
{
    static const uint16_t colors[TRACKER_MAX_CHANNELS] = {
        TRACKER_RGB565(255, 76, 1), TRACKER_RGB565(55, 208, 255),
        TRACKER_RGB565(255, 194, 75), TRACKER_RGB565(124, 255, 107),
        TRACKER_RGB565(180, 140, 255), TRACKER_RGB565(255, 107, 181),
        TRACKER_RGB565(91, 140, 255), TRACKER_RGB565(107, 232, 216),
        TRACKER_RGB565(255, 159, 69), TRACKER_RGB565(198, 240, 75),
        TRACKER_RGB565(224, 107, 255), TRACKER_RGB565(155, 166, 181),
        TRACKER_RGB565(90, 96, 104), TRACKER_RGB565(90, 96, 104),
        TRACKER_RGB565(90, 96, 104), TRACKER_RGB565(90, 96, 104),
    };
    return channel >= 0 && channel < TRACKER_MAX_CHANNELS ? colors[channel] : C_DIM;
}

void tracker_gfx_header(tracker_graphics_t *gfx, const tracker_status_t *status,
                        const char *state_label)
{
    char buffer[32];
    tracker_gfx_rect(gfx, 0, 0, FRAME_W, 32, C_PANEL);
    tracker_gfx_text(gfx, 34, 10, 2, C_TEXT, "OP-Ay1");
    snprintf(buffer, sizeof(buffer), "PAT %02X/%02X", display_hex(status->pattern),
             display_hex(status->patterns));
    tracker_gfx_text(gfx, 130, 10, 1, C_MUTED, buffer);
    snprintf(buffer, sizeof(buffer), "ORD %02X/%02X", display_hex(status->order_index),
             display_hex(status->orders));
    tracker_gfx_text(gfx, 216, 10, 1, C_MUTED, buffer);
    snprintf(buffer, sizeof(buffer), "ROW %02X/%02X", display_hex(status->row),
             display_hex(status->row_count));
    tracker_gfx_text(gfx, 302, 10, 1, C_MUTED, buffer);
    snprintf(buffer, sizeof(buffer), "CH %02X/%02X", display_hex(status->channels),
             (unsigned)TRACKER_MAX_CHANNELS);
    tracker_gfx_text(gfx, 130, 20, 1, C_MUTED, buffer);
    const uint64_t total_seconds = status->frames / 48000U;
    /* Keep the fixed two-digit readout bounded even after very long sessions. */
    const unsigned minutes = (unsigned)(total_seconds / 60U % 100U);
    const unsigned seconds = (unsigned)(total_seconds % 60U);
    snprintf(buffer, sizeof(buffer), "TIME %02u:%02u", minutes, seconds);
    tracker_gfx_text(gfx, 216, 20, 1, C_MUTED, buffer);
    if (state_label != NULL) {
        tracker_gfx_text(gfx, 302, 20, 1,
                         status->playing ? C_ACCENT : C_MUTED, state_label);
    }
    snprintf(buffer, sizeof(buffer), "BPM %03u", (unsigned)bounded(status->tempo, 0, 999));
    tracker_gfx_text_right(gfx, 446, 10, 1, C_MUTED, buffer);
    snprintf(buffer, sizeof(buffer), "SPD %02X", display_hex(status->speed));
    tracker_gfx_text_right(gfx, 446, 20, 1, C_MUTED, buffer);
    tracker_gfx_rect(gfx, 0, 32, FRAME_W, 1, C_LINE);
}

void tracker_gfx_order_strip(tracker_graphics_t *gfx,
                             const tracker_status_t *status)
{
    tracker_gfx_rect(gfx, 0, 409, FRAME_W, 13, C_PANEL);
    tracker_gfx_rect(gfx, 0, 409, FRAME_W, 1, C_LINE);
    tracker_gfx_text(gfx, 4, 412, 1, C_MUTED, "ORD");
    if (status->orders <= 0) {
        return;
    }
    const int orders = bounded(status->orders, 1, 256);
    const int pitch = bounded((FRAME_W - 44) / orders, 3, 10);
    for (int step = 0; step < orders; ++step) {
        const uint16_t color = step < status->order_index ? C_ACCENT
                               : step == status->order_index ? C_TEXT : C_BORDER;
        tracker_gfx_rect(gfx, 30 + step * pitch, 412, 6, 7, color);
    }
}
