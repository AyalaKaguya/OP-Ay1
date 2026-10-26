// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app_settings.h"
#include "app_usb_midi.h"

typedef struct {
    bool open;
    bool pointer_down;
    bool page_pointer_down;
    bool top_candidate;
    bool brightness_drag;
    bool moved;
    bool usb_confirm;
    bool awaiting_display;
    int start_x;
    int start_y;
    int requested_brightness;
    int requested_rotation;
    bool requested_locked;
    bool display_request;
    bool usb_request;
    app_usb_mode_t requested_usb;
    app_settings_snapshot_t display;
    app_usb_midi_status_t usb;
} tracker_settings_t;

void tracker_settings_init(tracker_settings_t *settings);
void tracker_settings_receive_display(tracker_settings_t *settings,
                                      const app_settings_snapshot_t *snapshot);
/* True consumes a complete top-edge/menu gesture before the page sees it.
 * out_opened is set only when the gesture opens the menu. */
bool tracker_settings_pointer(tracker_settings_t *settings, int x, int y,
                              bool pressed, bool *out_opened);
void tracker_settings_render(uint16_t *frame, const tracker_settings_t *settings);
