// SPDX-License-Identifier: Apache-2.0

#include "tracker_font.h"

#include <string.h>

#include "tracker_font_data.h"

#define GLYPH_ADVANCE (TRACKER_FONT_WIDTH + 1)

static void fill_rect(uint16_t *frame, int frame_w, int frame_h, int x, int y,
                      int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width;
    int y1 = y + height;
    if (x1 > frame_w) {
        x1 = frame_w;
    }
    if (y1 > frame_h) {
        y1 = frame_h;
    }
    for (int row = y0; row < y1; ++row) {
        uint16_t *line = frame + (size_t)row * (size_t)frame_w;
        for (int column = x0; column < x1; ++column) {
            line[column] = color;
        }
    }
}

void tracker_font_draw(uint16_t *frame, int frame_w, int frame_h, int x, int y,
                       int scale, uint16_t color, const char *text)
{
    if (frame == NULL || text == NULL || scale < 1) {
        return;
    }
    int cursor = x;
    for (const char *letter = text; *letter != '\0'; ++letter) {
        unsigned code = (unsigned char)*letter;
        /* The generated 5x7 set has no lowercase glyphs; fold them onto the
         * uppercase ones so names like "OP-Ay1" stay readable. */
        if (code >= 'a' && code <= 'z') {
            code = code - 'a' + 'A';
        }
        if (code < TRACKER_FONT_FIRST || code > TRACKER_FONT_LAST) {
            cursor += GLYPH_ADVANCE * scale;
            continue;
        }
        const uint8_t *glyph = tracker_font_5x7[code - TRACKER_FONT_FIRST];
        for (int row = 0; row < TRACKER_FONT_HEIGHT; ++row) {
            const uint8_t bits = glyph[row];
            if (bits == 0) {
                continue;
            }
            for (int column = 0; column < TRACKER_FONT_WIDTH; ++column) {
                if ((bits & (1U << (TRACKER_FONT_WIDTH - 1 - column))) == 0) {
                    continue;
                }
                fill_rect(frame, frame_w, frame_h, cursor + column * scale,
                          y + row * scale, scale, scale, color);
            }
        }
        cursor += GLYPH_ADVANCE * scale;
    }
}

int tracker_font_width(const char *text, int scale)
{
    if (text == NULL || scale < 1) {
        return 0;
    }
    return (int)strlen(text) * GLYPH_ADVANCE * scale;
}
