// SPDX-License-Identifier: Apache-2.0
/*
 * Canvas renderer for the OP-Ay1 tracker. Both pages are rasterised only when
 * they become visible, so playback never competes with a full-frame redraw.
 */

#include "tracker_render.h"

#include <stdio.h>
#include <string.h>

#include "tracker_graphics.h"

#define CH_Y 33
#define CH_H 13
#define GRID_Y 46
#define ROW_H 19
#define GUTTER 14
#define COL_W 29

#define WAVE_TILE_W 112
#define WAVE_TILE_GRID_X 16
#define WAVE_TILE_LEFT 6
#define WAVE_TILE_CENTER 42
#define WAVE_TILE_AMPLITUDE 32

/* Keep the legacy pattern/wave geometry; all writes now share the clipped
 * graphics implementation used by the control pages. */
static void fill_rect(uint16_t *frame, int x, int y, int width, int height,
                      uint16_t color)
{
    tracker_graphics_t gfx;
    tracker_gfx_init(&gfx, frame);
    tracker_gfx_rect(&gfx, x, y, width, height, color);
}

static void text(uint16_t *frame, int x, int y, int scale, uint16_t color,
                 const char *value)
{
    tracker_graphics_t gfx;
    tracker_gfx_init(&gfx, frame);
    tracker_gfx_text(&gfx, x, y, scale, color, value);
}

static void text_right(uint16_t *frame, int right, int y, int scale,
                       uint16_t color, const char *value)
{
    tracker_graphics_t gfx;
    tracker_gfx_init(&gfx, frame);
    tracker_gfx_text_right(&gfx, right, y, scale, color, value);
}

static const char *note_text(int note, char out[4])
{
    static const char *const names[12] = {
        "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-",
    };
    if (note < 0) {
        snprintf(out, 4, "---");
        return out;
    }
    if (note == 0xFE) {
        snprintf(out, 4, "===");
        return out;
    }
    const int octave = (note >> 4) & 0x0F;
    const int semitone = note & 0x0F;
    if (semitone > 11 || octave > 9) {
        snprintf(out, 4, "???");
        return out;
    }
    out[0] = names[semitone][0];
    out[1] = names[semitone][1];
    out[2] = (char)('0' + octave);
    out[3] = '\0';
    return out;
}

static int pattern_top_line(tracker_engine_t *engine)
{
    const int total = tracker_engine_song_rows(engine);
    const int current = tracker_engine_current_song_row(engine);
    if (total <= 0 || current < 0) {
        return 0;
    }
    int top = current - TRACKER_VISIBLE_ROWS / 2;
    if (top > total - TRACKER_VISIBLE_ROWS) {
        top = total - TRACKER_VISIBLE_ROWS;
    }
    if (top < 0) {
        top = 0;
    }
    return top;
}

int tracker_pattern_top_line(tracker_engine_t *engine)
{
    return pattern_top_line(engine);
}

static void draw_top_bar(uint16_t *frame, const tracker_status_t *status,
                         const char *state_label)
{
    tracker_graphics_t gfx;
    tracker_gfx_init(&gfx, frame);
    tracker_gfx_header(&gfx, status, state_label);
}

static void draw_order_strip(uint16_t *frame, const tracker_status_t *status)
{
    tracker_graphics_t gfx;
    tracker_gfx_init(&gfx, frame);
    tracker_gfx_order_strip(&gfx, status);
}

void tracker_render_pattern(uint16_t *frame, tracker_engine_t *engine)
{
    if (frame == NULL || engine == NULL) {
        return;
    }
    tracker_status_t status;
    tracker_engine_get_status(engine, &status);

    fill_rect(frame, 0, 0, TRACKER_FRAME_W, TRACKER_FRAME_H, C_BG);
    draw_top_bar(frame, &status, status.playing ? "PLAY" : "PAUSE");

    for (int channel = 0; channel < TRACKER_MAX_CHANNELS; ++channel) {
        const int column_x = GUTTER + channel * COL_W;
        char label[8];
        snprintf(label, sizeof(label), "%02X", (unsigned)(channel & 0xFF));
        const int label_x = column_x + (COL_W - 12) / 2;
        text(frame, label_x, CH_Y + 3, 1,
             channel < status.channels ? tracker_gfx_channel_color(channel) : C_DIM, label);
        if ((channel + 1) % 4 == 0 && channel + 1 < TRACKER_MAX_CHANNELS) {
            fill_rect(frame, column_x + COL_W - 1, CH_Y, 1,
                      GRID_Y + ROW_H * TRACKER_VISIBLE_ROWS - CH_Y, C_LINE);
        }
    }
    fill_rect(frame, 0, CH_Y + CH_H, TRACKER_FRAME_W, 1, C_LINE);

    const int total = tracker_engine_song_rows(engine);
    const int current = tracker_engine_current_song_row(engine);
    if (total > 0 && current >= 0) {
        const int top_line = pattern_top_line(engine);
        /* Rows after the current pattern are the look-ahead region: rendered
         * grey until playback reaches them. */
        const int mask_start = current - status.row + status.row_count;
        for (int line = 0; line < TRACKER_VISIBLE_ROWS; ++line) {
            const int song_row = top_line + line;
            int pattern = 0;
            int row = 0;
            if (!tracker_engine_song_row_at(engine, song_row, &pattern, &row)) {
                continue;
            }
            const int y = GRID_Y + line * ROW_H;
            const bool current_row = song_row == current;
            const bool masked = song_row >= mask_start;
            if (current_row) {
                fill_rect(frame, 0, y, TRACKER_FRAME_W, ROW_H, C_CURRENT_ROW);
            }
            if (row == 0 && line > 0) {
                fill_rect(frame, 0, y, TRACKER_FRAME_W, 1, C_LINE);
            }

            char row_label[8];
            snprintf(row_label, sizeof(row_label), "%02X",
                     (unsigned)(row & 0xFF));
            text(frame, 2, y + 5, 1,
                 current_row ? C_ACCENT : (masked ? C_MASK_ROW : C_ROW),
                 row_label);

            for (int channel = 0; channel < TRACKER_MAX_CHANNELS; ++channel) {
                tracker_cell_t cell;
                tracker_engine_cell(engine, pattern, row, channel, &cell);
                const int column_x = GUTTER + channel * COL_W;

                char note[4];
                note_text(cell.note, note);
                uint16_t note_color = cell.note >= 0 ? tracker_gfx_channel_color(channel) : C_DIM;
                if (masked) {
                    note_color = cell.note >= 0 ? C_MASK_NOTE : C_MASK_DIM;
                }
                text(frame, column_x, y + 2, 1, note_color, note);

                char sub[5];
                if (cell.instrument > 0) {
                    snprintf(sub, sizeof(sub), "%02X",
                             (unsigned)(cell.instrument & 0xFF));
                } else {
                    snprintf(sub, sizeof(sub), "..");
                }
                text(frame, column_x + 1, y + 11, 1,
                     cell.instrument > 0 ? (masked ? C_MASK_DIM : C_MUTED) : C_DIM,
                     sub);

                if (cell.volume >= 0) {
                    snprintf(sub, sizeof(sub), "%02X",
                             (unsigned)(cell.volume & 0xFF));
                } else {
                    snprintf(sub, sizeof(sub), "..");
                }
                text(frame, column_x + 13, y + 11, 1,
                     cell.volume >= 0 ? (masked ? C_MASK_DIM : C_CYAN) : C_DIM,
                     sub);

                if (cell.effect >= 0) {
                    fill_rect(frame, column_x + COL_W - 3, y + 10, 2, 8,
                              masked ? C_MASK_DIM : C_FX);
                }
            }
        }
    }

    draw_order_strip(frame, &status);
}

void tracker_render_waves(uint16_t *frame, tracker_engine_t *engine,
                          int16_t *samples, int sample_count)
{
    if (frame == NULL || engine == NULL) {
        return;
    }
    tracker_status_t status;
    tracker_engine_get_status(engine, &status);

    fill_rect(frame, 0, 0, TRACKER_FRAME_W, TRACKER_FRAME_H, C_BG);
    draw_top_bar(frame, &status, status.playing ? "PLAY" : "PAUSE");

    for (int channel = 0; channel < TRACKER_MAX_CHANNELS; ++channel) {
        const int tile_x = WAVE_TILE_GRID_X + (channel % 4) * WAVE_TILE_W;
        const int tile_y = TRACKER_WAVE_GRID_Y + (channel / 4) * TRACKER_WAVE_TILE_H;
        const uint16_t color =
            channel < status.channels ? tracker_gfx_channel_color(channel) : C_DIM;

        fill_rect(frame, tile_x, tile_y, WAVE_TILE_W, 1, C_LINE);
        fill_rect(frame, tile_x, tile_y, 1, TRACKER_WAVE_TILE_H, C_LINE);

        char label[8];
        snprintf(label, sizeof(label), "%02X", (unsigned)(channel & 0xFF));
        text(frame, tile_x + WAVE_TILE_LEFT, tile_y + 5, 1, color, label);

        tracker_cell_t cell;
        tracker_engine_cell(engine, status.pattern, status.row, channel, &cell);
        if (cell.note >= 0) {
            char note[4];
            note_text(cell.note, note);
            text_right(frame, tile_x + WAVE_TILE_W - 6, tile_y + 5, 1,
                       cell.note >= 0 ? color : C_DIM, note);
        }

        const int center_y = tile_y + WAVE_TILE_CENTER;
        fill_rect(frame, tile_x + WAVE_TILE_LEFT, center_y,
                  WAVE_TILE_W - 2 * WAVE_TILE_LEFT, 1, C_LINE);

        if (channel < status.channels && samples != NULL && sample_count > 1) {
            const int available = tracker_engine_waveform(
                                      engine, channel, samples, sample_count < TRACKER_WAVEFORM_SAMPLES
                                      ? sample_count : TRACKER_WAVEFORM_SAMPLES);
            if (available > 1) {
                /* Fit about three cycles of the playing note: a fixed window
                 * either misses bass cycles or crams treble ones. */
                int window = available;
                const float cycle =
                    tracker_engine_channel_cycle_frames(engine, channel);
                if (cycle > 0.0f && cycle < (float)available / 3.0f) {
                    window = (int)(cycle * 3.0f);
                    if (window < 64) {
                        window = 64;
                    }
                    if (window > available) {
                        window = available;
                    }
                }
                int start = available - window;
                const int search = start + window / 2;
                for (int index = start; index + 1 < search; ++index) {
                    if (samples[index] <= 0 && samples[index + 1] > 0) {
                        start = index; /* scope-style rising-edge trigger */
                        break;
                    }
                }
                if (start > available - 2) {
                    start = available - 2;
                }
                const int width = WAVE_TILE_W - 2 * WAVE_TILE_LEFT;
                const int trace_left = tile_x + WAVE_TILE_LEFT;
                int previous_y = center_y;
                for (int px = 0; px < width; ++px) {
                    int pos = start + (px * (window - 1)) / (width - 1);
                    if (pos > available - 2) {
                        pos = available - 2;
                    }
                    const int offset = (int)((int32_t)samples[pos] *
                                             WAVE_TILE_AMPLITUDE / 32767);
                    const int y = center_y - offset;
                    const int top = previous_y < y ? previous_y : y;
                    const int bottom = previous_y > y ? previous_y : y;
                    fill_rect(frame, trace_left + px, top, 1, bottom - top + 1,
                              color);
                    previous_y = y;
                }
            }
        }
        fill_rect(frame, tile_x, tile_y + TRACKER_WAVE_TILE_H - 1, WAVE_TILE_W, 1,
                  C_LINE);
    }
}
