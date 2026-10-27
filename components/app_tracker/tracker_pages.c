// SPDX-License-Identifier: Apache-2.0
#include "tracker_pages.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "tracker_graphics.h"
#include "tracker_render.h"

static int clamp_int(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static unsigned display_hex(int value)
{
    return (unsigned)clamp_int(value, 0, 256);
}

static void title(tracker_graphics_t *gfx, const char *name, const char *mode)
{
    tracker_gfx_text(gfx, 20, 46, 2, C_TEXT, name);
    tracker_gfx_text_right(gfx, 460, 50, 1, C_MUTED, mode);
    tracker_gfx_rect(gfx, 16, 68, 448, 1, C_BORDER);
}

static void position_card(tracker_graphics_t *gfx, int index, const char *name,
                          int value, int total, uint16_t color)
{
    const int x = 16 + index * 150;
    tracker_gfx_rect(gfx, x, 82, 148, 83, C_PANEL);
    tracker_gfx_outline(gfx, x, 82, 148, 83, C_LINE);
    tracker_gfx_text(gfx, x + 12, 94, 1, C_MUTED, name);
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%02X", display_hex(value));
    tracker_gfx_text(gfx, x + 12, 116, 4, color, buffer);
    snprintf(buffer, sizeof(buffer), "/ %02X", display_hex(total));
    tracker_gfx_text_right(gfx, x + 134, 135, 1, C_MUTED, buffer);
}

static void playback(tracker_graphics_t *gfx, const tracker_controls_view_t *view)
{
    title(gfx, "PLAYBACK", "SONG");
    const tracker_status_t *status = &view->status;
    position_card(gfx, 0, "ORDER", status->order_index, status->orders, C_TEXT);
    position_card(gfx, 1, "PATTERN", status->pattern, status->patterns, C_TEXT);
    position_card(gfx, 2, "ROW", status->row, status->row_count, C_ACCENT);
    tracker_gfx_text(gfx, 16, 177, 1, C_MUTED, "PATTERN POSITION");
    const int row_count = clamp_int(status->row_count, 1, TRACKER_PATTERN_ROWS);
    const int row = clamp_int(status->row, 0, row_count - 1);
    for (int step = 0; step < TRACKER_PATTERN_ROWS; ++step) {
        const uint16_t color = step < row ? C_ACCENT : step == row ? C_TEXT : C_BORDER;
        tracker_gfx_rect(gfx, 16 + step * 7, 192, 5, 5, color);
    }
    tracker_gfx_button(gfx, 16, 218, 218, 77, "RESTART", C_TEXT, false);
    tracker_gfx_button(gfx, 246, 218, 218, 77,
                       status->playing ? "PAUSE" : "PLAY",
                       status->playing ? C_ACCENT : C_GREEN, status->playing);
    tracker_gfx_text(gfx, 16, 314, 1, C_MUTED, "SPEED");
    float multiplier = view->speed_multiplier;
    if (!isfinite(multiplier)) {
        multiplier = 1.0f;
    }
    multiplier = fminf(fmaxf(multiplier, 0.5f), 2.0f);
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%.2fX", (double)multiplier);
    tracker_gfx_text_right(gfx, 464, 310, 2, C_CYAN, buffer);
    static const char *const labels[4] = {"0.5X", "1X", "1.5X", "2X"};
    const int selected = clamp_int((int)floorf((multiplier - 0.5f) * 2.0f + 0.5f), 0, 3);
    for (int index = 0; index < 4; ++index) {
        const bool active = selected == index;
        tracker_gfx_button(gfx, 16 + index * 114, 338, 106, 48, labels[index],
                           active ? C_CYAN : C_TEXT, active);
    }
    tracker_gfx_order_strip(gfx, status);
}

static void mix_row(tracker_graphics_t *gfx, const tracker_controls_view_t *view,
                    int channel, int y)
{
    const bool present = channel < clamp_int(view->channel_count, 0, TRACKER_MAX_CHANNELS);
    const bool muted = present && view->muted[channel];
    const bool solo = present && view->soloed[channel];
    const uint16_t channel_color = present && !muted ? tracker_gfx_channel_color(channel)
                                   : C_DISABLED;
    tracker_gfx_rect(gfx, 16, y, 434, TRACKER_MIX_STRIP_H, C_BG);
    tracker_gfx_rect(gfx, 16, y, 434, 1, C_BORDER);
    tracker_gfx_rect(gfx, 16, y + 7, 2, 50, channel_color);
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%02X", (unsigned)channel);
    tracker_gfx_text(gfx, 28, y + 21, 2, present && muted ? C_MUTED : channel_color, buffer);
    tracker_gfx_meter(gfx, 86, y + 11, 154, 6,
                      present && !muted ? view->levels[channel] : 0);
    const int gain = clamp_int(view->gain_steps[channel], 0, 120);
    if (!present) {
        snprintf(buffer, sizeof(buffer), "--");
    } else if (gain == 0) {
        snprintf(buffer, sizeof(buffer), "-INF");
    } else {
        const float db = (float)(gain - 100) * 0.3f;
        snprintf(buffer, sizeof(buffer), "%+.1f", (double)db);
    }
    tracker_gfx_text_right(gfx, 318, y + 10, 1,
                           present && !muted ? C_TEXT : C_MUTED, buffer);
    if (present) {
        tracker_gfx_slider(gfx, TRACKER_MIX_FADER_X, y + TRACKER_MIX_FADER_Y,
                           TRACKER_MIX_FADER_W, (float)gain / 120.0f,
                           muted ? C_DISABLED : C_CYAN, true);
    } else {
        tracker_gfx_rect(gfx, TRACKER_MIX_FADER_X, y + TRACKER_MIX_FADER_Y,
                         TRACKER_MIX_FADER_W, 4, C_LINE);
    }
    tracker_gfx_button(gfx, TRACKER_MIX_MUTE_X, y + TRACKER_MIX_MUTE_Y,
                       TRACKER_MIX_MUTE_W, TRACKER_MIX_MUTE_H, "M",
                       muted ? C_ACCENT : present ? C_MUTED : C_DISABLED, muted);
    tracker_gfx_button(gfx, TRACKER_MIX_SOLO_X, y + TRACKER_MIX_MUTE_Y,
                       TRACKER_MIX_MUTE_W, TRACKER_MIX_MUTE_H, "S",
                       solo ? C_CYAN : present ? C_MUTED : C_DISABLED, solo);
}

static void mixer(tracker_graphics_t *gfx, const tracker_controls_view_t *view)
{
    const int max_scroll = TRACKER_MIX_STRIPS * TRACKER_MIX_STRIP_H - TRACKER_MIX_VIEW_H;
    const int scroll = clamp_int(view->mix_scroll, 0, max_scroll);
    const int first = scroll / TRACKER_MIX_STRIP_H;
    const int last = clamp_int((scroll + TRACKER_MIX_VIEW_H - 1) / TRACKER_MIX_STRIP_H,
                               first, TRACKER_MIX_STRIPS - 1);
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "CH %02X-%02X", (unsigned)first, (unsigned)last);
    title(gfx, "MIXER", buffer);
    tracker_gfx_text(gfx, 20, 82, 1, C_MUTED, "MASTER");
    const int volume = clamp_int(view->volume, 0, 100);
    snprintf(buffer, sizeof(buffer), "%d%%", volume);
    tracker_gfx_text_right(gfx, 450, 80, 2, C_ACCENT, buffer);
    tracker_gfx_slider(gfx, TRACKER_MIX_MASTER_X, TRACKER_MIX_MASTER_Y,
                       TRACKER_MIX_MASTER_W, (float)volume / 100.0f, C_ACCENT, false);
    tracker_gfx_rect(gfx, 20, 111, 50, 17,
                     view->clipped ? TRACKER_RGB565(41, 22, 15) : C_PANEL);
    tracker_gfx_outline(gfx, 20, 111, 50, 17, view->clipped ? C_ACCENT : C_BORDER);
    tracker_gfx_text_center(gfx, 45, 116, 1, view->clipped ? C_ACCENT : C_MUTED, "CLIP");
    tracker_gfx_meter(gfx, 100, 113, 350, 8, view->master_level);
    for (int db = -48; db <= 0; db += 12) {
        snprintf(buffer, sizeof(buffer), "%d", db);
        const int x = 100 + (349 * (db + 48) + 24) / 48;
        tracker_gfx_text_center(gfx, x, 129, 1, C_MUTED, buffer);
    }
    tracker_gfx_rect(gfx, 16, 144, 448, 1, C_BORDER);
    tracker_gfx_text(gfx, 28, 151, 1, C_MUTED, "CH");
    tracker_gfx_text(gfx, 86, 151, 1, C_MUTED, "LEVEL / GAIN");
    tracker_gfx_text_right(gfx, 318, 151, 1, C_MUTED, "DB");
    tracker_gfx_text_center(gfx, 356, 151, 1, C_MUTED, "M");
    tracker_gfx_text_center(gfx, 414, 151, 1, C_MUTED, "S");

    /* All row primitives, including partial glyphs, stay below the fixed header. */
    tracker_gfx_clip(gfx, 0, TRACKER_MIX_VIEW_Y, TRACKER_FRAME_W, TRACKER_MIX_VIEW_H);
    for (int channel = first; channel <= last; ++channel) {
        const int y = TRACKER_MIX_VIEW_Y + channel * TRACKER_MIX_STRIP_H - scroll;
        mix_row(gfx, view, channel, y);
    }
    tracker_gfx_clip(gfx, 0, 0, TRACKER_FRAME_W, TRACKER_FRAME_H);
    tracker_gfx_rect(gfx, 16, 428, 434, 1, C_BORDER);
    const int content_height = TRACKER_MIX_STRIPS * TRACKER_MIX_STRIP_H;
    const int bar_height = TRACKER_MIX_VIEW_H * TRACKER_MIX_VIEW_H / content_height;
    const int bar_y = TRACKER_MIX_VIEW_Y + (TRACKER_MIX_VIEW_H - bar_height) * scroll / max_scroll;
    tracker_gfx_rect(gfx, 458, TRACKER_MIX_VIEW_Y, 3, TRACKER_MIX_VIEW_H, C_LINE);
    tracker_gfx_rect(gfx, 458, bar_y, 3, bar_height, C_MUTED);
}

static void pad(tracker_graphics_t *gfx, const tracker_controls_view_t *view, int index)
{
    const int x = 16 + index % 4 * 114;
    const int y = 115 + index / 4 * 75;
    const bool available = index < clamp_int(view->status.patterns, 0, 16);
    const bool running = available && view->status.playing && index == view->pad_playing;
    const bool queued = available && view->status.playing && index == view->pad_next && !running;
    const bool highlighted = (running && (!view->pad_stop || view->pad_blink)) ||
                             (queued && view->pad_blink);
    const uint16_t accent = running && view->pad_stop ? C_FX : running ? C_ACCENT : C_CYAN;
    const uint16_t edge = highlighted ? accent : available ? C_BORDER : C_LINE;
    const uint16_t fill = highlighted
                          ? running ? TRACKER_RGB565(41, 22, 15) : TRACKER_RGB565(17, 35, 44)
                          : available ? C_PANEL : C_BG;
    tracker_gfx_rect(gfx, x, y, 106, 67, fill);
    tracker_gfx_outline(gfx, x, y, 106, 67, edge);
    tracker_gfx_text(gfx, x + 8, y + 8, 1, available ? C_MUTED : C_DISABLED, "PAT");
    char buffer[8];
    snprintf(buffer, sizeof(buffer), "%02X", (unsigned)index);
    tracker_gfx_text(gfx, x + 8, y + 24, 3,
                     highlighted ? accent : available ? C_TEXT : C_DISABLED, buffer);
    const char *label = running ? view->pad_stop ? "STOP" : "PLAY"
                        : queued ? "NEXT" : available ? "READY" : "EMPTY";
    tracker_gfx_text_right(gfx, x + 98, y + 52, 1,
                           highlighted ? accent : available ? C_MUTED : C_DISABLED, label);
    if (highlighted) {
        tracker_gfx_rect(gfx, x + 1, y + 64, 104, 2, accent);
    }
}

static void pads(tracker_graphics_t *gfx, const tracker_controls_view_t *view)
{
    title(gfx, "SECTION PADS", "SECTION");
    tracker_gfx_rect(gfx, 16, 78, 448, 27, C_PANEL);
    tracker_gfx_outline(gfx, 16, 78, 448, 27, C_BORDER);
    char buffer[24];
    const bool active = view->status.playing && view->pad_playing >= 0 && view->pad_playing < 16;
    tracker_gfx_rect(gfx, 25, 88, 6, 6, active ? C_ACCENT : C_MUTED);
    if (active) {
        snprintf(buffer, sizeof(buffer), "PLAY %02X", (unsigned)view->pad_playing);
    } else {
        snprintf(buffer, sizeof(buffer), "%s", view->status.playing ? "PLAY" : "PAUSED");
    }
    tracker_gfx_text(gfx, 40, 88, 1, active ? C_ACCENT : C_MUTED, buffer);
    const bool next = view->status.playing && view->pad_next >= 0 && view->pad_next < 16;
    const bool stopping = active && view->pad_stop;
    const uint16_t next_color = stopping ? C_FX : next ? C_CYAN : C_MUTED;
    tracker_gfx_rect(gfx, 150, 88, 6, 6, next_color);
    if (stopping) {
        snprintf(buffer, sizeof(buffer), "STOP QUEUED");
    } else if (next) {
        snprintf(buffer, sizeof(buffer), "NEXT %02X", (unsigned)view->pad_next);
    } else {
        snprintf(buffer, sizeof(buffer), "NEXT --");
    }
    tracker_gfx_text(gfx, 165, 88, 1, next_color, buffer);
    snprintf(buffer, sizeof(buffer), "ROW %02X/%02X", display_hex(view->status.row),
             display_hex(view->status.row_count));
    tracker_gfx_text_right(gfx, 450, 88, 1, C_MUTED, buffer);
    for (int index = 0; index < 16; ++index) {
        pad(gfx, view, index);
    }
    tracker_gfx_text(gfx, 16, 423, 1, C_MUTED, "TAP PLAYING PAD: QUEUE / CANCEL STOP");
}

void tracker_render_controls(uint16_t *frame, const tracker_controls_view_t *view)
{
    if (frame == NULL || view == NULL) {
        return;
    }
    tracker_graphics_t gfx;
    tracker_gfx_init(&gfx, frame);
    tracker_gfx_rect(&gfx, 0, 0, TRACKER_FRAME_W, TRACKER_FRAME_H, C_BG);
    tracker_gfx_header(&gfx, &view->status, view->status.playing ? "PLAY" : "PAUSE");
    switch (view->page) {
    case TRACKER_PAGE_PLAYBACK:
        playback(&gfx, view);
        break;
    case TRACKER_PAGE_MIXER:
        mixer(&gfx, view);
        break;
    case TRACKER_PAGE_PADS:
        pads(&gfx, view);
        break;
    default:
        break;
    }
}
