// SPDX-License-Identifier: Apache-2.0
/*
 * Canvas renderer for the OP-Ay1 tracker. Both pages are rasterised only when
 * they become visible, so playback never competes with a full-frame redraw.
 */

#include "tracker_render.h"

#include <stdio.h>
#include <string.h>

#include "tracker_font.h"

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BG RGB565(11, 12, 14)
#define C_PANEL RGB565(16, 18, 22)
#define C_LINE RGB565(23, 26, 31)
#define C_TEXT RGB565(242, 242, 240)
#define C_MUTED RGB565(123, 130, 141)
#define C_ROW RGB565(90, 96, 104)
#define C_DIM RGB565(42, 46, 53)
#define C_ACCENT RGB565(255, 76, 1)
#define C_CYAN RGB565(55, 208, 255)
#define C_FX RGB565(255, 194, 75)
#define C_CURRENT_ROW RGB565(30, 34, 40)
#define C_MASK_NOTE RGB565(122, 126, 132)
#define C_MASK_DIM RGB565(58, 60, 64)
#define C_MASK_ROW RGB565(78, 80, 84)

#define TOP_H 32
#define CH_Y 33
#define CH_H 13
#define GRID_Y 46
#define ROW_H 19
#define GUTTER 14
#define COL_W 29
#define STRIP_Y 409
#define STRIP_H 13

#define WAVE_TILE_W 112
#define WAVE_TILE_GRID_X 16
#define WAVE_TILE_LEFT 6
#define WAVE_TILE_CENTER 42
#define WAVE_TILE_AMPLITUDE 32

static const uint16_t k_channel[TRACKER_MAX_CHANNELS] = {
    RGB565(255, 76, 1),   RGB565(55, 208, 255),  RGB565(255, 194, 75),
    RGB565(124, 255, 107), RGB565(180, 140, 255), RGB565(255, 107, 181),
    RGB565(91, 140, 255),  RGB565(107, 232, 216), RGB565(255, 159, 69),
    RGB565(198, 240, 75),  RGB565(224, 107, 255), RGB565(155, 166, 181),
    RGB565(90, 96, 104),   RGB565(90, 96, 104),   RGB565(90, 96, 104),
    RGB565(90, 96, 104),
};

static void fill_rect(uint16_t *frame, int x, int y, int width, int height,
                      uint16_t color)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > TRACKER_FRAME_W) {
        width = TRACKER_FRAME_W - x;
    }
    if (y + height > TRACKER_FRAME_H) {
        height = TRACKER_FRAME_H - y;
    }
    for (int row = 0; row < height; ++row) {
        uint16_t *line = frame + (size_t)(y + row) * TRACKER_FRAME_W + x;
        for (int column = 0; column < width; ++column) {
            line[column] = color;
        }
    }
}

static void text(uint16_t *frame, int x, int y, int scale, uint16_t color,
                 const char *value)
{
    tracker_font_draw(frame, TRACKER_FRAME_W, TRACKER_FRAME_H, x, y, scale, color,
                      value);
}

static void text_right(uint16_t *frame, int right, int y, int scale,
                       uint16_t color, const char *value)
{
    text(frame, right - tracker_font_width(value, scale), y, scale, color, value);
}

static void format_time(uint64_t frames, char out[9])
{
    const uint64_t total_seconds = frames / 48000U;
    const unsigned minutes = (unsigned)(total_seconds / 60U);
    const unsigned seconds = (unsigned)(total_seconds % 60U);
    snprintf(out, 9, "%02u:%02u", minutes, seconds);
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
    snprintf(out, 4, "%s%d", names[semitone], octave);
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
    char buffer[24];
    char time_buffer[9];
    fill_rect(frame, 0, 0, TRACKER_FRAME_W, TOP_H, C_PANEL);
    /* Keep the header inside the rounded-corner safe area. */
    text(frame, 30, 10, 2, C_TEXT, "OP-Ay1");
    text(frame, 82, 15, 1, C_ACCENT, "S3M");

    snprintf(buffer, sizeof(buffer), "PAT %02X/%02X", status->pattern,
             status->patterns);
    text(frame, 130, 10, 1, C_MUTED, buffer);
    snprintf(buffer, sizeof(buffer), "ORD %02X/%02X", status->order_index,
             status->orders);
    text(frame, 216, 10, 1, C_MUTED, buffer);
    snprintf(buffer, sizeof(buffer), "ROW %02X/%02X", status->row,
             status->row_count);
    text(frame, 302, 10, 1, C_MUTED, buffer);

    snprintf(buffer, sizeof(buffer), "CH %02X/%02X", status->channels,
             TRACKER_MAX_CHANNELS);
    text(frame, 130, 20, 1, C_MUTED, buffer);
    format_time(status->frames, time_buffer);
    snprintf(buffer, sizeof(buffer), "TIME %s", time_buffer);
    text(frame, 216, 20, 1, C_MUTED, buffer);
    if (state_label != NULL) {
        text(frame, 302, 20, 1, status->playing ? C_ACCENT : C_MUTED,
             state_label);
    }

    snprintf(buffer, sizeof(buffer), "BPM %03d", status->tempo);
    text_right(frame, 450, 10, 1, C_MUTED, buffer);
    snprintf(buffer, sizeof(buffer), "SPD %02X", status->speed);
    text_right(frame, 450, 20, 1, C_MUTED, buffer);
    fill_rect(frame, 0, TOP_H, TRACKER_FRAME_W, 1, C_LINE);
}

static void draw_order_strip(uint16_t *frame, const tracker_status_t *status)
{
    fill_rect(frame, 0, STRIP_Y, TRACKER_FRAME_W, STRIP_H, C_PANEL);
    fill_rect(frame, 0, STRIP_Y, TRACKER_FRAME_W, 1, C_LINE);
    text(frame, 4, STRIP_Y + 3, 1, C_MUTED, "ORD");

    if (status->orders <= 0) {
        return;
    }
    int pitch = (TRACKER_FRAME_W - 44) / status->orders;
    if (pitch > 10) {
        pitch = 10;
    }
    if (pitch < 3) {
        pitch = 3;
    }
    for (int step = 0; step < status->orders; ++step) {
        uint16_t color = C_DIM;
        if (step < status->order_index) {
            color = C_ACCENT;
        } else if (step == status->order_index) {
            color = C_TEXT;
        }
        fill_rect(frame, 30 + step * pitch, STRIP_Y + 3, 6, 7, color);
    }
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
             channel < status.channels ? k_channel[channel] : C_DIM, label);
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
                uint16_t note_color = cell.note >= 0 ? k_channel[channel] : C_DIM;
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

void tracker_render_waves(uint16_t *frame, tracker_engine_t *engine)
{
    if (frame == NULL || engine == NULL) {
        return;
    }
    tracker_status_t status;
    tracker_engine_get_status(engine, &status);

    fill_rect(frame, 0, 0, TRACKER_FRAME_W, TRACKER_FRAME_H, C_BG);
    draw_top_bar(frame, &status, status.playing ? "PLAY" : "PAUSE");

    static int16_t samples[TRACKER_WAVEFORM_SAMPLES];
    for (int channel = 0; channel < TRACKER_MAX_CHANNELS; ++channel) {
        const int tile_x = WAVE_TILE_GRID_X + (channel % 4) * WAVE_TILE_W;
        const int tile_y = TRACKER_WAVE_GRID_Y + (channel / 4) * TRACKER_WAVE_TILE_H;
        const uint16_t color =
            channel < status.channels ? k_channel[channel] : C_DIM;

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

        if (channel < status.channels) {
            const int available = tracker_engine_waveform(
                engine, channel, samples, TRACKER_WAVEFORM_SAMPLES);
            if (available > 1) {
                /* Fit about three cycles of the playing note: a fixed window
                 * either misses bass cycles or crams treble ones. */
                int window = available;
                const float cycle =
                    tracker_engine_channel_cycle_frames(engine, channel);
                if (cycle > 0.0f) {
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


void tracker_render_mix(uint16_t *frame, tracker_engine_t *engine, int scroll,
                        int volume, const uint8_t *levels,
                        uint8_t master_level)
{
    if (frame == NULL || engine == NULL) {
        return;
    }
    char buffer[32];
    tracker_status_t status;
    tracker_engine_get_status(engine, &status);
    fill_rect(frame, 0, 0, TRACKER_FRAME_W, TRACKER_FRAME_H, C_BG);

    /* ---- fixed header ---- */
    text(frame, 30, 8, 2, C_TEXT, "MIX");
    text_right(frame, 450, 10, 1, C_MUTED, "MASTER");
    snprintf(buffer, sizeof(buffer), "VOL %d", volume);
    text(frame, 30, 38, 1, C_MUTED, buffer);
    fill_rect(frame, TRACKER_MIX_MASTER_X, TRACKER_MIX_MASTER_Y,
              TRACKER_MIX_MASTER_W, TRACKER_MIX_MASTER_H, C_LINE);
    fill_rect(frame, TRACKER_MIX_MASTER_X, TRACKER_MIX_MASTER_Y,
              TRACKER_MIX_MASTER_W * volume / 100, TRACKER_MIX_MASTER_H,
              C_ACCENT);
    const int master = master_level > 0
                           ? (int)master_level
                           : tracker_engine_master_level(engine);
    text(frame, 30, 64, 1, C_MUTED, "LVL");
    fill_rect(frame, TRACKER_MIX_MASTER_X, 66, TRACKER_MIX_MASTER_W, 12,
              C_LINE);
    fill_rect(frame, TRACKER_MIX_MASTER_X, 66,
              TRACKER_MIX_MASTER_W * master / 255, 12,
              master > 220 ? C_ACCENT : (master > 150 ? C_FX : C_CYAN));
    fill_rect(frame, 0, TRACKER_MIX_HEADER_H, TRACKER_FRAME_W, 1, C_LINE);

    /* ---- scrollable channel strips ---- */
    const int view_top = TRACKER_MIX_HEADER_H;
    const int view_h = TRACKER_FRAME_H - view_top;
    for (int channel = 0; channel < TRACKER_MIX_STRIPS; ++channel) {
        const int y0 = view_top + channel * TRACKER_MIX_STRIP_H - scroll;
        if (y0 > TRACKER_FRAME_H || y0 + TRACKER_MIX_STRIP_H < view_top) {
            continue;
        }
        fill_rect(frame, 8, y0 + 4, TRACKER_FRAME_W - 16,
                  TRACKER_MIX_STRIP_H - 8, C_PANEL);
        fill_rect(frame, 8, y0 + 4, 3, TRACKER_MIX_STRIP_H - 8,
                  channel < status.channels ? k_channel[channel] : C_DIM);

        snprintf(buffer, sizeof(buffer), "CH %02X", (unsigned)channel);
        text(frame, 20, y0 + 12, 2, C_TEXT, buffer);

        const float gain = tracker_engine_channel_gain(engine, channel);
        snprintf(buffer, sizeof(buffer), "%.2f", (double)gain);
        text_right(frame, 286, y0 + 14, 1, C_TEXT, buffer);

        const bool muted = tracker_engine_channel_mute(engine, channel);
        fill_rect(frame, TRACKER_MIX_MUTE_X, y0 + TRACKER_MIX_MUTE_Y,
                  TRACKER_MIX_MUTE_W, TRACKER_MIX_MUTE_H,
                  muted ? C_ACCENT : C_LINE);
        text(frame, TRACKER_MIX_MUTE_X + 22, y0 + TRACKER_MIX_MUTE_Y + 14, 1,
             muted ? C_BG : C_TEXT, "M");
        const bool solo = tracker_engine_channel_solo(engine, channel);
        fill_rect(frame, TRACKER_MIX_SOLO_X, y0 + TRACKER_MIX_MUTE_Y,
                  TRACKER_MIX_MUTE_W, TRACKER_MIX_MUTE_H,
                  solo ? C_CYAN : C_LINE);
        text(frame, TRACKER_MIX_SOLO_X + 22, y0 + TRACKER_MIX_MUTE_Y + 14, 1,
             solo ? C_BG : C_TEXT, "S");

        fill_rect(frame, TRACKER_MIX_FADER_X, y0 + TRACKER_MIX_FADER_Y,
                  TRACKER_MIX_FADER_W, TRACKER_MIX_FADER_H, C_LINE);
        const int knob_x = TRACKER_MIX_FADER_X +
                           (int)(gain / 1.5f * TRACKER_MIX_FADER_W);
        fill_rect(frame, knob_x - 5, y0 + TRACKER_MIX_FADER_Y - 6, 10, 22,
                  C_TEXT);

        const int level = levels != NULL
                              ? (int)levels[channel]
                              : tracker_engine_channel_level(engine, channel);
        fill_rect(frame, TRACKER_MIX_FADER_X, y0 + 88, 420, 16, C_LINE);
        fill_rect(frame, TRACKER_MIX_FADER_X, y0 + 88, 420 * level / 255, 16,
                  level > 220 ? C_ACCENT : (level > 150 ? C_FX : C_CYAN));
        snprintf(buffer, sizeof(buffer), "%d", level);
        text(frame, TRACKER_MIX_FADER_X, y0 + 112, 1, C_MUTED, buffer);
    }

    /* scroll indicator */
    const int content_h = TRACKER_MIX_STRIPS * TRACKER_MIX_STRIP_H;
    const int max_scroll = content_h - view_h;
    if (max_scroll > 0 && scroll >= 0 && scroll <= max_scroll) {
        const int bar_h = view_h * view_h / content_h;
        const int bar_y = view_top + (view_h - bar_h) * scroll / max_scroll;
        fill_rect(frame, TRACKER_FRAME_W - 4, view_top, 3, view_h, C_LINE);
        fill_rect(frame, TRACKER_FRAME_W - 4, bar_y, 3, bar_h, C_MUTED);
    }
}
